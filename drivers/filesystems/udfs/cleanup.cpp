////////////////////////////////////////////////////////////////////
// Copyright (C) Alexander Telyatnikov, Ivan Keliukh, Yegor Anchishkin, SKIF Software, 1999-2013. Kiev, Ukraine
// All rights reserved
// This file was released under the GPLv2 on June 2015.
////////////////////////////////////////////////////////////////////
/*

 Module name: Cleanup.cpp

 Abstract:

    Contains code to handle the "Cleanup" dispatch entry point.

 Environment:

    Kernel mode only
*/

#include            "udffs.h"

// define the file specific bug-check id
#define         UDF_BUG_CHECK_ID                UDF_FILE_CLEANUP

VOID
UDFAutoUnlock (
    IN PVCB Vcb
    );

/*************************************************************************
*
* Function: UDFCommonCleanup()
*
* Description:
*   The actual work is performed here. This routine may be invoked in one'
*   of the two possible contexts:
*   (a) in the context of a system worker thread
*   (b) in the context of the original caller
*
* Expected Interrupt Level (for execution) :
*
*  IRQL_PASSIVE_LEVEL
*
* Return Value: Does not matter!
*
*************************************************************************/
NTSTATUS
UDFCommonCleanup(
    PIRP_CONTEXT IrpContext,
    PIRP Irp)
{
    IO_STATUS_BLOCK    IoStatus;
    NTSTATUS           RC = STATUS_SUCCESS;
    PFILE_OBJECT       FileObject = NULL;
    PFCB               Fcb = NULL;
    PCCB               Ccb = NULL;
    PVCB               Vcb = NULL;
    ULONG              lc = 0;
    BOOLEAN            AcquiredVcb = FALSE;
    BOOLEAN            AcquiredFCB = FALSE;
    BOOLEAN            AcquiredParentFCB = FALSE;
    BOOLEAN            SendUnlockNotification = FALSE;
    BOOLEAN            ChangeTime = FALSE;
    BOOLEAN            ForcedCleanUp = FALSE;

    PUDF_FILE_INFO     NextFileInfo = NULL;
    TYPE_OF_OPEN       TypeOfOpen;

    ASSERT_IRP_CONTEXT(IrpContext);
    ASSERT_IRP(Irp);

    // If called with our file system device object, just complete request.
    if (IrpContext->Vcb == NULL) {
        UDFCompleteRequest(IrpContext, Irp, STATUS_SUCCESS);
        return STATUS_SUCCESS;
    }

    FileObject = IoGetCurrentIrpStackLocation(Irp)->FileObject;
    TypeOfOpen = UDFDecodeFileObject(FileObject, &Fcb, &Ccb);

    // No work for unopened or stream file objects.
    if (TypeOfOpen <= StreamFileOpen) {
        UDFCompleteRequest(IrpContext, Irp, STATUS_SUCCESS);
        return STATUS_SUCCESS;
    }

    Vcb = Fcb->Vcb;

    ASSERT_CCB(Ccb);
    ASSERT_FCB(Fcb);
    ASSERT_VCB(Vcb);

    _SEH2_TRY {

        if (TypeOfOpen == UserVolumeOpen) {
            if (FlagOn(Ccb->Flags, CCB_FLAG_DISMOUNT_ON_CLOSE)) {
                UDFAcquireUdfData(IrpContext);
                UDFCheckForDismount(IrpContext, Vcb, TRUE);
                UDFReleaseUdfData(IrpContext);
            } else if (FlagOn(FileObject->Flags, FO_FILE_MODIFIED)) {
                UDFHijackIrpAndFlushDevice(IrpContext, Irp, Vcb->TargetDeviceObject);
                UDFUpdateMediaChangeCount(Vcb, 0);
                UDFMarkDevForVerifyIfVcbMounted(Vcb);
            }

            if (FlagOn(Vcb->VcbState, VCB_STATE_LOCKED) &&
                FileObject == Vcb->VolumeLockFileObject) {
                UDFAutoUnlock(Vcb);
                SendUnlockNotification = TRUE;
            }

            UDFLockVcb(IrpContext, Vcb);
            UDFDecrementCleanupCounts(IrpContext, Fcb);
            UDFUnlockVcb(IrpContext, Vcb);

            if (FileObject->Flags & FO_CACHE_SUPPORTED) {
                InterlockedDecrement((PLONG)&Fcb->CachedOpenHandleCount);
            }

            CcUninitializeCacheMap(FileObject, NULL, NULL);
            IoRemoveShareAccess(FileObject, &Fcb->ShareAccess);

            try_return(RC = STATUS_SUCCESS);
        }
        else if ((TypeOfOpen == UserFileOpen) || (TypeOfOpen == UserDirectoryOpen)) {
            UDFAcquireVcbExclusive(IrpContext, Vcb, FALSE);
            AcquiredVcb = TRUE;
        }

        UDF_CHECK_PAGING_IO_RESOURCE(Fcb);
        UDFAcquireResourceExclusive(&Fcb->FcbNonpaged->FcbResource, TRUE);
        AcquiredFCB = TRUE;

        UDFLockVcb(IrpContext, Vcb);
        UDFDecrementCleanupCounts(IrpContext, Fcb);
        UDFUnlockVcb(IrpContext, Vcb);

        if (FileObject->Flags & FO_CACHE_SUPPORTED) {
            InterlockedDecrement((PLONG)&Fcb->CachedOpenHandleCount);
        }

        // Set DeletePending status if requested by CCB.
        if (Ccb->Flags & UDF_CCB_DELETE_ON_CLOSE) {
            ASSERT(!(Fcb->FcbState & UDF_FCB_ROOT_DIRECTORY));
            Fcb->FcbState |= UDF_FCB_DELETE_ON_CLOSE;
            FileObject->DeletePending = TRUE;
            
            if (Fcb->FcbState & UDF_FCB_DIRECTORY) {
                FsRtlNotifyFullChangeDirectory(Vcb->NotifySync, &(Vcb->NextNotifyIRP),
                                               (PVOID)Ccb, NULL, FALSE, FALSE,
                                               0, NULL, NULL, NULL);
            }
        }

        if (!(Fcb->FcbState & UDF_FCB_DIRECTORY)) {
            if (Fcb->FileLock != NULL) {
                FsRtlFastUnlockAll(Fcb->FileLock, FileObject, IoGetRequestorProcess(Irp), NULL);
            }
        }

        lc = UDFGetFileLinkCount(Fcb->FileInfo);

        // Prep for potential deletion.
        if ((Fcb->FcbState & UDF_FCB_DELETE_ON_CLOSE) && !(Fcb->FcbCleanup)) {
            FileObject->DeletePending = TRUE;

            if ((lc <= 1) && !UDFIsSDirDeleted(Fcb->FileInfo->Dloc->SDirInfo)) {
                RC = UDFMarkStreamsForDeletion(IrpContext, Vcb, Fcb, TRUE);
            }
            
            if (Fcb->FileInfo->ParentFile) {
                UDF_CHECK_PAGING_IO_RESOURCE(Fcb->ParentFcb);
                UDFAcquireResourceExclusive(&(Fcb->ParentFcb->FcbNonpaged->FcbResource), TRUE);
                AcquiredParentFCB = TRUE;
            }

            if (lc <= 1) {
                UDFAcquireResourceExclusive(&Fcb->FcbNonpaged->FcbPagingIoResource, TRUE);
                Fcb->Header.FileSize.QuadPart = 
                Fcb->Header.ValidDataLength.QuadPart = 0;
                CcSetFileSizes(FileObject, (PCC_FILE_SIZES)&Fcb->Header.AllocationSize);
                UDFReleaseResource(&Fcb->FcbNonpaged->FcbPagingIoResource);
            }
        }

        NextFileInfo = Fcb->FileInfo;

        // EXECUTE DELETION
        if ((Fcb->FcbState & UDF_FCB_DELETE_ON_CLOSE) && !(Fcb->FcbCleanup)) {

            // SAFETY CHECK: If it's a directory, check if empty.
            if (Fcb->FcbState & UDF_FCB_DIRECTORY) {
                ASSERT(!(Fcb->FcbState & UDF_FCB_ROOT_DIRECTORY));
                if (!UDFIsDirEmpty__(NextFileInfo)) {
                    // ROLLBACK: Directory not empty, stop delete and clear flags.
                    Fcb->FcbState &= ~UDF_FCB_DELETE_ON_CLOSE;
                    FileObject->DeletePending = FALSE;
                    AdPrint(("UDF: Cleanup - Directory not empty. Deletion rolled back.\n"));
                    goto DiscardDelete;
                }
            } else if (lc <= 1) {
                BOOLEAN AcquiredPagingIo = UDFAcquireResourceExclusiveWithCheck(&Fcb->FcbNonpaged->FcbPagingIoResource);
                UDFResizeFile__(IrpContext, Vcb, NextFileInfo, 0);
                if (AcquiredPagingIo) {
                    UDFReleaseResource(&Fcb->FcbNonpaged->FcbPagingIoResource);
                }
            }

            if ((Fcb->FcbState & UDF_FCB_DELETE_PARENT) && Fcb->ParentFcb) {
                ASSERT(!(Fcb->ParentFcb->FcbState & UDF_FCB_ROOT_DIRECTORY));
                Fcb->ParentFcb->FcbState |= UDF_FCB_DELETE_ON_CLOSE;
            }

            RC = UDFFlushFile__(IrpContext, Vcb, NextFileInfo);
            
            // ATTEMPT PHYSICAL UNLINK
            RC = UDFUnlinkFile__(IrpContext, Vcb, NextFileInfo, TRUE);

            if (!NT_SUCCESS(RC)) {
                // FAILURE RECOVERY: If unlink failed for any reason (e.g. nested subfolders),
                // we MUST clear the flags to avoid the "Permission Denied" zombie state.
                Fcb->FcbState &= ~UDF_FCB_DELETE_ON_CLOSE;
                FileObject->DeletePending = FALSE;

                if (RC == STATUS_CANNOT_DELETE) {
                    if (NextFileInfo->Dloc && NextFileInfo->Dloc->SDirInfo && NextFileInfo->Dloc->SDirInfo->Fcb) {
                        if (!UDFIsSDirDeleted(NextFileInfo->Dloc->SDirInfo)) {
                            UDFPretendFileDeleted__(Vcb, Fcb->FileInfo);
                        }
                        goto NotifyDelete;
                    } else {
                        goto DiscardDelete_1;
                    }
                } else {
                    goto DiscardDelete_1;
                }
            } else {
DiscardDelete_1:
                ASSERT(!(Fcb->FcbState & UDF_FCB_ROOT_DIRECTORY));
                ForcedCleanUp = TRUE;
                // Only mark as fully deleted if the RC was actually success.
                if (NT_SUCCESS(RC)) {
                    Fcb->FcbState &= ~UDF_FCB_DELETE_ON_CLOSE;
                    Fcb->FcbState |= UDF_FCB_DELETED;
                } else {
                    // Unlink failed, ensure flags are cleared.
                    Fcb->FcbState &= ~UDF_FCB_DELETE_ON_CLOSE;
                    FileObject->DeletePending = FALSE;
                }
                RC = STATUS_SUCCESS;
            }

NotifyDelete:
            if (lc < 1) {
                Fcb->NtReqFCBFlags |= UDF_NTREQ_FCB_DELETED;
            }
            UDFNotifyFullReportChange(Vcb, NextFileInfo->Fcb,
                                       UDFIsADirectory(NextFileInfo) ? FILE_NOTIFY_CHANGE_DIR_NAME : FILE_NOTIFY_CHANGE_FILE_NAME,
                                       FILE_ACTION_REMOVED);
        } else if (Fcb->FcbState & UDF_FCB_DELETE_ON_CLOSE) {
DiscardDelete:
            UDFNotifyFullReportChange(Vcb, NextFileInfo->Fcb,
                                     ((Ccb->Flags & UDF_CCB_ACCESS_TIME_SET) ? FILE_NOTIFY_CHANGE_LAST_ACCESS : 0) |
                                     ((Ccb->Flags & UDF_CCB_WRITE_TIME_SET) ? (FILE_NOTIFY_CHANGE_ATTRIBUTES | FILE_NOTIFY_CHANGE_LAST_WRITE) : 0),
                                     UDFIsAStream(NextFileInfo) ? FILE_ACTION_MODIFIED_STREAM : FILE_ACTION_MODIFIED);
        }

        if (Fcb->FcbState & UDF_FCB_DIRECTORY) {
            FsRtlNotifyCleanup(Vcb->NotifySync, &(Vcb->NextNotifyIRP), (PVOID)Ccb);
        }

        if (lc > 1) {
            ForcedCleanUp = FALSE;
        }

        // Cache Management.
        if (FileObject->Flags & FO_CACHE_SUPPORTED && Fcb->FcbNonpaged->SegmentObject.DataSectionObject) {
            BOOLEAN LastNonCached = (!Fcb->CachedOpenHandleCount && Fcb->FcbCleanup);
            RC = STATUS_SUCCESS;
            if (LastNonCached || (!Fcb->FcbCleanup && !ForcedCleanUp)) {
                LONGLONG OldFileSize = Fcb->Header.ValidDataLength.QuadPart;
                LONGLONG NewFileSize = Fcb->Header.FileSize.QuadPart;

                if (OldFileSize < NewFileSize) {
                    UDFZeroData(Vcb, FileObject, OldFileSize, NewFileSize - OldFileSize, TRUE);
                    Fcb->Header.ValidDataLength.QuadPart = NewFileSize;
                }

                CcFlushCache(&Fcb->FcbNonpaged->SegmentObject, NULL, 0, &IoStatus);
            }
            if (ForcedCleanUp || LastNonCached) {
                if (Fcb->FcbNonpaged->SegmentObject.DataSectionObject) {
                    CcPurgeCacheSection(&Fcb->FcbNonpaged->SegmentObject, NULL, 0, FALSE);
                }
            }
        }

        // --- Time and Size Updates ---
        if (!(Vcb->VcbState & VCB_STATE_VOLUME_READ_ONLY) &&
            !(Fcb->FcbState & (UDF_FCB_DELETE_ON_CLOSE | UDF_FCB_DELETED)) &&
            !UDFIsAStreamDir(NextFileInfo)) {
            
            LONGLONG NtTime;
            LONGLONG ASize;
            KeQuerySystemTime((PLARGE_INTEGER)&NtTime);

            if (FileObject->Flags & FO_FILE_MODIFIED) {
                PDIR_INDEX_ITEM DirNdx = UDFDirIndex(UDFGetDirIndexByFileInfo(NextFileInfo), NextFileInfo->Index);
                if (DirNdx) {
                    if (!(Ccb->Flags & UDF_CCB_ATTRIBUTES_SET) && (Vcb->CompatFlags & UDF_VCB_IC_UPDATE_ARCH_BIT)) {
                        ULONG Attr = UDFAttributesToNT(DirNdx, NextFileInfo->Dloc->FileEntry);
                        if (!(Attr & FILE_ATTRIBUTE_ARCHIVE))
                            UDFAttributesToUDF(DirNdx, NextFileInfo->Dloc->FileEntry, Attr | FILE_ATTRIBUTE_ARCHIVE);
                    }
                    if (!(Ccb->Flags & UDF_CCB_WRITE_TIME_SET) && (Vcb->CompatFlags & UDF_VCB_IC_UPDATE_MODIFY_TIME)) {
                        UDFSetFileXTime(NextFileInfo, NULL, &NtTime, NULL, &NtTime);
                        Fcb->LastWriteTime.QuadPart = Fcb->LastAccessTime.QuadPart = NtTime;
                        ChangeTime = TRUE;
                    }
                }
            }

            if (!(Fcb->FcbState & UDF_FCB_DIRECTORY)) {
                if (!Fcb->FcbCleanup) {
                    ASize = UDFGetFileAllocationSize(Vcb, NextFileInfo);
                    UDFSetFileSizeInDirNdx(Vcb, NextFileInfo, &ASize);
                } else if (FileObject->Flags & FO_FILE_SIZE_CHANGED) {
                    ASize = Fcb->Header.AllocationSize.QuadPart;
                    UDFSetFileSizeInDirNdx(Vcb, NextFileInfo, &ASize);
                }
            }
        }

        if (!(Fcb->FcbState & UDF_FCB_DIRECTORY) && ForcedCleanUp) {
            CcUninitializeCacheMap(FileObject, &(UdfData.UDFLargeZero), NULL);
        } else {
            CcUninitializeCacheMap(FileObject, NULL, NULL);
        }

        UDF_CHECK_PAGING_IO_RESOURCE(Fcb);
        UDFReleaseResource(&Fcb->FcbNonpaged->FcbResource);
        AcquiredFCB = FALSE;

        if (AcquiredParentFCB && Fcb->FileInfo->ParentFile) {
            UDF_CHECK_PAGING_IO_RESOURCE(Fcb->FileInfo->ParentFile->Fcb);
            UDFReleaseResource(&Fcb->FileInfo->ParentFile->Fcb->FcbNonpaged->FcbResource);
            AcquiredParentFCB = FALSE;
        }

        if (NextFileInfo) {
            UDFCloseFile__(IrpContext, Vcb, NextFileInfo);
        }

        Ccb->Flags |= UDF_CCB_CLEANED;
        IoRemoveShareAccess(FileObject, &Fcb->ShareAccess);
        Fcb->Header.IsFastIoPossible = UDFIsFastIoPossible(Fcb);
        FileObject->Flags |= FO_CLEANUP_COMPLETE;

try_exit: NOTHING;

    } _SEH2_FINALLY {
        if (AcquiredFCB) {
            UDFReleaseResource(&Fcb->FcbNonpaged->FcbResource);
        }
        if (AcquiredParentFCB && Fcb->FileInfo->ParentFile) {
            UDFReleaseResource(&Fcb->FileInfo->ParentFile->Fcb->FcbNonpaged->FcbResource);
        }
        if (AcquiredVcb) {
            UDFReleaseResource(&Vcb->VcbResource);
        }
        if (SendUnlockNotification) {
            FsRtlNotifyVolumeEvent(FileObject, FSRTL_VOLUME_UNLOCK);
        }
        if (!_SEH2_AbnormalTermination()) {
            UDFCompleteRequest(IrpContext, Irp, RC);
        }
    } _SEH2_END;

    return(RC);
} // end UDFCommonCleanup()

VOID
UDFAutoUnlock (
    IN PVCB Vcb
    )
{
    KIRQL SavedIrql;

    //  Unlock the volume.
 
    IoAcquireVpbSpinLock( &SavedIrql );

    ClearFlag(Vcb->Vpb->Flags, VPB_LOCKED | VPB_DIRECT_WRITES_ALLOWED);
    ClearFlag(Vcb->VcbState, VCB_STATE_LOCKED);
    Vcb->VolumeLockFileObject = NULL;

    IoReleaseVpbSpinLock( SavedIrql );
}
