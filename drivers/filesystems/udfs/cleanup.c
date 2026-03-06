////////////////////////////////////////////////////////////////////
// Copyright (C) Alexander Telyatnikov, Ivan Keliukh, Yegor Anchishkin, SKIF Software, 1999-2013. Kiev, Ukraine
// All rights reserved
// This file was released under the GPLv2 on June 2015.
////////////////////////////////////////////////////////////////////
/*

 Module name: Cleanup.c

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
    IO_STATUS_BLOCK         IoStatus;
    NTSTATUS                RC = STATUS_SUCCESS;
    PFILE_OBJECT            FileObject = NULL;
    PFCB                    Fcb = NULL;
    PCCB                    Ccb = NULL;
    PVCB                    Vcb = NULL;
    ULONG                   lc = 0;
    BOOLEAN                 AcquiredVcb = FALSE;
    BOOLEAN                 AcquiredFCB = FALSE;
    BOOLEAN                 AcquiredParentFCB = FALSE;
    BOOLEAN SendUnlockNotification = FALSE;
    BOOLEAN                 ChangeTime = FALSE;
    BOOLEAN                 ForcedCleanUp = FALSE;

    PUDF_FILE_INFO          NextFileInfo = NULL;
    TYPE_OF_OPEN            TypeOfOpen;

    ASSERT_IRP_CONTEXT(IrpContext);
    ASSERT_IRP(Irp);

    // If we were called with our file system device object instead of a
    // volume device object, just complete this request with STATUS_SUCCESS.

    if (IrpContext->Vcb == NULL) {

        UDFCompleteRequest(IrpContext, Irp, STATUS_SUCCESS);
        return STATUS_SUCCESS;
    }

    // Get the file object out of the Irp and decode the type of open.

    FileObject = IoGetCurrentIrpStackLocation(Irp)->FileObject;

    TypeOfOpen = UDFDecodeFileObject(FileObject, &Fcb, &Ccb);

    // No work here for either an UnopenedFile object or a StreamFileObject.

    if (TypeOfOpen <= StreamFileOpen) {

        UDFCompleteRequest(IrpContext, Irp, STATUS_SUCCESS);

        return STATUS_SUCCESS;
    }

    //  Keep a local pointer to the Vcb.
    Vcb = Fcb->Vcb;

    ASSERT_CCB(Ccb);
    ASSERT_FCB(Fcb);
    ASSERT_VCB(Vcb);

    _SEH2_TRY {

        // Steps we shall take at this point are:
        // (a) Acquire the file (FCB) exclusively
        // (b) Flush file data to disk
        // (c) Talk to the FSRTL package (if we use it) about pending oplocks.
        // (d) Notify the FSRTL package for use with pending notification IRPs
        // (e) Unlock byte-range locks (if any were acquired by process)
        // (f) Update time stamp values (e.g. fast-IO had been performed)
        // (g) Inform the Cache Manager to uninitialize Cache Maps ...
        // and other similar stuff.
        //  BrutePoint();

        if (TypeOfOpen == UserVolumeOpen) {

            // For a force dismount, physically disconnect this Vcb from the device so 
            // a new mount can occur.  Vcb deletion cannot happen at this time since 
            // there is a reference on it associated with this very request,  but we'll 
            // call check for dismount again later after we process this close.

            if (FlagOn(Ccb->Flags, CCB_FLAG_DISMOUNT_ON_CLOSE)) {
        
                UDFAcquireUdfData(IrpContext);
                UDFCheckForDismount(IrpContext, Vcb, TRUE);
                UDFReleaseUdfData(IrpContext);

            // If this handle actually wrote something, flush the device buffers,
            // and then set the verify bit now just to be safe (in case there is no
            // dismount).
        
            } else if (FlagOn(FileObject->Flags, FO_FILE_MODIFIED)) {
        
                UDFHijackIrpAndFlushDevice(IrpContext, Irp, Vcb->TargetDeviceObject);
                UDFUpdateMediaChangeCount(Vcb, 0);
                UDFMarkDevForVerifyIfVcbMounted(Vcb);
            }

            //  If the volume is locked by this file object then release
            //  the volume and send notification.

            if (FlagOn(Vcb->VcbState, VCB_STATE_LOCKED) &&
                FileObject == Vcb->VolumeLockFileObject) {

                UDFAutoUnlock(Vcb);
                SendUnlockNotification = TRUE;
            }

            UDFLockVcb(IrpContext, Vcb);
            UDFDecrementCleanupCounts(IrpContext, Fcb);
            UDFUnlockVcb(IrpContext, Vcb);

            if (FileObject->Flags & FO_CACHE_SUPPORTED) {
                // we've cached close
                InterlockedDecrement((PLONG)&Fcb->CachedOpenHandleCount);
            }
            ASSERT(Fcb->FcbCleanup <= (Fcb->FcbReference-1));


            MmPrint(("    CcUninitializeCacheMap()\n"));
            CcUninitializeCacheMap(FileObject, NULL, NULL);

            //  We must clean up the share access at this time, since we may not
            //  get a Close call for awhile if the file was mapped through this
            //  File Object.
            IoRemoveShareAccess( FileObject, &Fcb->ShareAccess);

            try_return(RC = STATUS_SUCCESS);
        }
        else if ((TypeOfOpen == UserFileOpen) || (TypeOfOpen == UserDirectoryOpen)) {

            UDFAcquireVcbExclusive(IrpContext, Vcb, FALSE);
            AcquiredVcb = TRUE;
        }

        // Acquire current object only
        // Parent is acquired later only for delete operations (Child → Parent order)
        UDF_CHECK_PAGING_IO_RESOURCE(Fcb);
        UDFAcquireResourceExclusive(&Fcb->FcbNonpaged->FcbResource, TRUE);
        AcquiredFCB = TRUE;

        // Decrement the cleanup counts in the Vcb and Fcb.

        UDFLockVcb(IrpContext, Vcb);
        UDFDecrementCleanupCounts(IrpContext, Fcb);
        UDFUnlockVcb(IrpContext, Vcb);

        if (FileObject->Flags & FO_CACHE_SUPPORTED) {
            // we've cached close
            InterlockedDecrement((PLONG)&Fcb->CachedOpenHandleCount);
        }
        ASSERT(Fcb->FcbCleanup <= (Fcb->FcbReference-1));

        // check if Ccb being cleaned up has DeleteOnClose flag set
        if (Ccb->Flags & UDF_CCB_DELETE_ON_CLOSE) {
            AdPrint(("    DeleteOnClose\n"));
            // Ok, now we'll become 'delete on close'...
            ASSERT(!(Fcb->FcbState & UDF_FCB_ROOT_DIRECTORY));
            Fcb->FcbState |= UDF_FCB_DELETE_ON_CLOSE;
            FileObject->DeletePending = TRUE;
            //  Report this to the dir notify package for a directory.
            if (Fcb->FcbState & UDF_FCB_DIRECTORY) {
                FsRtlNotifyFullChangeDirectory( Vcb->NotifySync, &(Vcb->NextNotifyIRP),
                                                (PVOID)Ccb, NULL, FALSE, FALSE,
                                                0, NULL, NULL, NULL );
            }
        }

        if (!(Fcb->FcbState & UDF_FCB_DIRECTORY)) {

            //  Unlock all outstanding file locks.
            if (Fcb->FileLock != NULL) {

                FsRtlFastUnlockAll(Fcb->FileLock,
                                   FileObject,
                                   IoGetRequestorProcess(Irp),
                                   NULL);
            }
        }
        // get Link count
        lc = UDFGetFileLinkCount(Fcb->FileInfo);

        if ( (Fcb->FcbState & UDF_FCB_DELETE_ON_CLOSE) &&
           !(Fcb->FcbCleanup)) {
            // This can be useful for Streams, those were brutally deleted
            // (together with parent object)
            ASSERT(!(Fcb->FcbState & UDF_FCB_ROOT_DIRECTORY));
            FileObject->DeletePending = TRUE;

            // we should mark all streams of the file being deleted
            // for deletion too, if there are no more Links to
            // main data stream
            if ((lc <= 1) &&
               !UDFIsSDirDeleted(Fcb->FileInfo->Dloc->SDirInfo)) {
                RC = UDFMarkStreamsForDeletion(IrpContext, Vcb, Fcb, TRUE); // Delete
            }
            // Acquire parent for delete operation (after current - child first order)
            if (Fcb->FileInfo->ParentFile) {
                UDF_CHECK_PAGING_IO_RESOURCE(Fcb->ParentFcb);
                UDFAcquireResourceExclusive(&(Fcb->ParentFcb->FcbNonpaged->FcbResource), TRUE);
                AcquiredParentFCB = TRUE;
            }

            // we should set file sizes to zero if there are no more
            // links to this file
            if (lc <= 1) {
                // Synchronize here with paging IO
                UDFAcquireResourceExclusive(&Fcb->FcbNonpaged->FcbPagingIoResource, TRUE);
                // set file size to zero (for system cache manager)
//                Fcb->CommonFCBHeader.ValidDataLength.QuadPart =
                Fcb->Header.FileSize.QuadPart =
                    Fcb->Header.ValidDataLength.QuadPart = 0;
                CcSetFileSizes(FileObject, (PCC_FILE_SIZES)&Fcb->Header.AllocationSize);

                UDFReleaseResource(&Fcb->FcbNonpaged->FcbPagingIoResource);
            }
        }

        NextFileInfo = Fcb->FileInfo;

        // do we need to delete it now ?
        if ( (Fcb->FcbState & UDF_FCB_DELETE_ON_CLOSE) &&
           !(Fcb->FcbCleanup)) {

            // can we do it ?
            if (Fcb->FcbState & UDF_FCB_DIRECTORY) {
                ASSERT(!(Fcb->FcbState & UDF_FCB_ROOT_DIRECTORY));
                if (!UDFIsDirEmpty__(NextFileInfo)) {
                    // forget about it
                    Fcb->FcbState &= ~UDF_FCB_DELETE_ON_CLOSE;
                    goto DiscardDelete;
                }
            } else
            if (lc <= 1) {
                // Synchronize here with paging IO
                BOOLEAN AcquiredPagingIo;
                AcquiredPagingIo = UDFAcquireResourceExclusiveWithCheck(&Fcb->FcbNonpaged->FcbPagingIoResource);
                // set file size to zero (for UdfInfo package)
                // we should not do this for directories and linked files
                UDFResizeFile__(IrpContext, Vcb, NextFileInfo, 0);
                if (AcquiredPagingIo) {
                    UDFReleaseResource(&Fcb->FcbNonpaged->FcbPagingIoResource);
                }
            }
            // mark parent object for deletion if requested
            if ((Fcb->FcbState & UDF_FCB_DELETE_PARENT) &&
                Fcb->ParentFcb) {
                ASSERT(!(Fcb->ParentFcb->FcbState & UDF_FCB_ROOT_DIRECTORY));
                Fcb->ParentFcb->FcbState |= UDF_FCB_DELETE_ON_CLOSE;
            }
            // flush file. It is required by UDFUnlinkFile__()
            RC = UDFFlushFile__(IrpContext, Vcb, NextFileInfo, 0);
            if (!NT_SUCCESS(RC)) {
                AdPrint(("Error flushing file !!!\n"));
            }
            // try to unlink
            if ((RC = UDFUnlinkFile__(IrpContext, Vcb, NextFileInfo, TRUE)) == STATUS_CANNOT_DELETE) {
                // If we can't delete file with Streams due to references,
                // mark SDir & Streams
                // for Deletion. We shall also set DELETE_PARENT flag to
                // force Deletion of the current file later... when curently
                // opened Streams would be cleaned up.

                // WARNING! We should keep SDir & Streams if there is a
                // link to this file
                if (NextFileInfo->Dloc &&
                   NextFileInfo->Dloc->SDirInfo &&
                   NextFileInfo->Dloc->SDirInfo->Fcb) {

                    BrutePoint();
                    if (!UDFIsSDirDeleted(NextFileInfo->Dloc->SDirInfo)) {
//                        RC = UDFMarkStreamsForDeletion(Vcb, Fcb, TRUE); // Delete
//#ifdef UDF_ALLOW_PRETEND_DELETED
                        UDFPretendFileDeleted__(Vcb, Fcb->FileInfo);
//#endif //UDF_ALLOW_PRETEND_DELETED
                    }
                    goto NotifyDelete;

                } else {
                    // Getting here means that we can't delete file because of
                    // References/PemissionsDenied/Smth.Else,
                    // but not Linked+OpenedStream
                    BrutePoint();
//                    RC = STATUS_SUCCESS;
                    goto DiscardDelete_1;
                }
            } else {
DiscardDelete_1:
                // We have got an ugly ERROR, or
                // file is deleted, so forget about it
                ASSERT(!(Fcb->FcbState & UDF_FCB_ROOT_DIRECTORY));
                ForcedCleanUp = TRUE;
                if (NT_SUCCESS(RC))
                    Fcb->FcbState &= ~UDF_FCB_DELETE_ON_CLOSE;
                Fcb->FcbState |= UDF_FCB_DELETED;
                RC = STATUS_SUCCESS;
            }
NotifyDelete:
            // We should prevent SetEOF operations on completly
            // deleted data streams
            if (lc < 1) {
                Fcb->NtReqFCBFlags |= UDF_NTREQ_FCB_DELETED;
            }
            // Report that we have removed an entry.
            if (UDFIsAStream(NextFileInfo)) {
                UDFNotifyFullReportChange( Vcb, NextFileInfo->Fcb,
                                       FILE_NOTIFY_CHANGE_STREAM_NAME,
                                       FILE_ACTION_REMOVED_STREAM);
            } else {
                UDFNotifyFullReportChange( Vcb, NextFileInfo->Fcb,
                                       UDFIsADirectory(NextFileInfo) ? FILE_NOTIFY_CHANGE_DIR_NAME : FILE_NOTIFY_CHANGE_FILE_NAME,
                                       FILE_ACTION_REMOVED);
            }
        } else
        if (Fcb->FcbState & UDF_FCB_DELETE_ON_CLOSE) {
DiscardDelete:
            UDFNotifyFullReportChange( Vcb, NextFileInfo->Fcb,
                                     ((Ccb->Flags & UDF_CCB_ACCESS_TIME_SET) ? FILE_NOTIFY_CHANGE_LAST_ACCESS : 0) |
                                     ((Ccb->Flags & UDF_CCB_WRITE_TIME_SET) ? (FILE_NOTIFY_CHANGE_ATTRIBUTES | FILE_NOTIFY_CHANGE_LAST_WRITE) : 0) |
                                     0,
                                     UDFIsAStream(NextFileInfo) ? FILE_ACTION_MODIFIED_STREAM : FILE_ACTION_MODIFIED);
        }

        if (Fcb->FcbState & UDF_FCB_DIRECTORY) {
            //  Report to the dir notify package for a directory.
            FsRtlNotifyCleanup( Vcb->NotifySync, &(Vcb->NextNotifyIRP), (PVOID)Ccb );
        }

        // we can't purge Cache when more than one link exists
        if (lc > 1) {
            ForcedCleanUp = FALSE;
        }

        if (FileObject->Flags & FO_CACHE_SUPPORTED &&
             Fcb->FcbNonpaged->SegmentObject.DataSectionObject) {
            BOOLEAN LastNonCached = (!Fcb->CachedOpenHandleCount &&
                                      Fcb->FcbCleanup);
            // If this was the last cached open, and there are open
            // non-cached handles, attempt a flush and purge operation
            // to avoid cache coherency overhead from these non-cached
            // handles later.  We ignore any I/O errors from the flush.
            // We shall not flush deleted files
            RC = STATUS_SUCCESS;
            if (  LastNonCached
                      ||
                (!Fcb->FcbCleanup &&
                 !ForcedCleanUp) ) {

                LONGLONG OldFileSize, NewFileSize;

                if ((OldFileSize = Fcb->Header.ValidDataLength.QuadPart) <
                    (NewFileSize = Fcb->Header.FileSize.QuadPart)) {
                    UDFZeroData(Vcb,
                                FileObject,
                                OldFileSize,
                                NewFileSize - OldFileSize,
                                TRUE);

                    Fcb->Header.ValidDataLength.QuadPart = NewFileSize;
                }

                MmPrint(("    CcFlushCache()\n"));
                CcFlushCache(&Fcb->FcbNonpaged->SegmentObject, NULL, 0, &IoStatus);
                if (!NT_SUCCESS(IoStatus.Status)) {
                    MmPrint(("    CcFlushCache() error: %x\n", IoStatus.Status));
                    RC = IoStatus.Status;
                }
            }
            // If file is deleted or it is last cached open, but there are
            // some non-cached handles we should purge cache section
            if (ForcedCleanUp || LastNonCached) {
                if (Fcb->FcbNonpaged->SegmentObject.DataSectionObject) {
                    MmPrint(("    CcPurgeCacheSection()\n"));
                    CcPurgeCacheSection(&Fcb->FcbNonpaged->SegmentObject, NULL, 0, FALSE);
                }
/*                MmPrint(("    CcPurgeCacheSection()\n"));
                CcPurgeCacheSection(&Fcb->SectionObject, NULL, 0, FALSE);*/
            }
            // we needn't Flush here. It will be done in UDFCloseFile__
        }

        // Update FileTimes & Attrs
        if (!(Vcb->VcbState & VCB_STATE_VOLUME_READ_ONLY) &&
           !(Fcb->FcbState & (UDF_FCB_DELETE_ON_CLOSE |
                              UDF_FCB_DELETED /*|
                              UDF_FCB_DIRECTORY |
                              UDF_FCB_READ_ONLY*/)) &&
           !UDFIsAStreamDir(NextFileInfo)) {
            LONGLONG NtTime;
            LONGLONG ASize;
            KeQuerySystemTime((PLARGE_INTEGER)&NtTime);
            // Check if we should set ARCHIVE bit & LastWriteTime
            if (FileObject->Flags & FO_FILE_MODIFIED) {
                ULONG Attr;
                PDIR_INDEX_ITEM DirNdx;
                DirNdx = UDFDirIndex(UDFGetDirIndexByFileInfo(NextFileInfo), NextFileInfo->Index);
                ASSERT(DirNdx);
                // Archive bit
                if (!(Ccb->Flags & UDF_CCB_ATTRIBUTES_SET) &&
                    (Vcb->CompatFlags & UDF_VCB_IC_UPDATE_ARCH_BIT)) {
                    Attr = UDFAttributesToNT(DirNdx, NextFileInfo->Dloc->FileEntry);
                    if (!(Attr & FILE_ATTRIBUTE_ARCHIVE))
                        UDFAttributesToUDF(DirNdx, NextFileInfo->Dloc->FileEntry, Attr | FILE_ATTRIBUTE_ARCHIVE);
                }
                // WriteTime
                if (!(Ccb->Flags & UDF_CCB_WRITE_TIME_SET) &&
                    (Vcb->CompatFlags & UDF_VCB_IC_UPDATE_MODIFY_TIME)) {
                    UDFSetFileXTime(NextFileInfo, NULL, &NtTime, NULL, &NtTime);
                    Fcb->LastWriteTime.QuadPart =
                    Fcb->LastAccessTime.QuadPart = NtTime;
                    ChangeTime = TRUE;
                }
            }
            if (!(Fcb->FcbState & UDF_FCB_DIRECTORY)) {
                // Update sizes in DirIndex
                if (!Fcb->FcbCleanup) {
                    ASize = UDFGetFileAllocationSize(Vcb, NextFileInfo);
//                        Fcb->CommonFCBHeader.AllocationSize.QuadPart;
                    UDFSetFileSizeInDirNdx(Vcb, NextFileInfo, &ASize);

                } else if (FileObject->Flags & FO_FILE_SIZE_CHANGED) {

                    ASize = //UDFGetFileAllocationSize(Vcb, NextFileInfo);
                    Fcb->Header.AllocationSize.QuadPart;
                    UDFSetFileSizeInDirNdx(Vcb, NextFileInfo, &ASize);

                    if (UDFIsAStream(Fcb->FileInfo)) {

                        UDFNotifyFullReportChange(Vcb,
                            Fcb,
                            FILE_NOTIFY_CHANGE_STREAM_SIZE,
                            FILE_ACTION_MODIFIED_STREAM);
                    }
                    else {

                        UDFNotifyFullReportChange(Vcb,
                            Fcb,
                            FILE_NOTIFY_CHANGE_SIZE,
                            FILE_ACTION_MODIFIED);
                    }

                }
            }
            // AccessTime
            if ((FileObject->Flags & FO_FILE_FAST_IO_READ) &&
               !(Ccb->Flags & UDF_CCB_ACCESS_TIME_SET) &&
                (Vcb->CompatFlags & UDF_VCB_IC_UPDATE_ACCESS_TIME)) {
                UDFSetFileXTime(NextFileInfo, NULL, &NtTime, NULL, NULL);
                Fcb->LastAccessTime.QuadPart = NtTime;
//                ChangeTime = TRUE;
            }
            // ChangeTime (AttrTime)
            if (!(Ccb->Flags & UDF_CCB_MODIFY_TIME_SET) &&
                (Vcb->CompatFlags & UDF_VCB_IC_UPDATE_ATTR_TIME) &&
                (ChangeTime || (Ccb->Flags & (UDF_CCB_ATTRIBUTES_SET |
                                                 UDF_CCB_CREATE_TIME_SET |
                                                 UDF_CCB_ACCESS_TIME_SET |
                                                 UDF_CCB_WRITE_TIME_SET))) ) {
                UDFSetFileXTime(NextFileInfo, NULL, NULL, &NtTime, NULL);
                Fcb->ChangeTime.QuadPart = NtTime;
            }
        }

        if (!(Fcb->FcbState & UDF_FCB_DIRECTORY) &&
            ForcedCleanUp) {
            // flush system cache
            MmPrint(("    CcUninitializeCacheMap()\n"));
            CcUninitializeCacheMap(FileObject, &(UdfData.UDFLargeZero), NULL);
        } else {
            MmPrint(("    CcUninitializeCacheMap()\n"));
            CcUninitializeCacheMap(FileObject, NULL, NULL);
        }

        // release resources now.
        UDF_CHECK_PAGING_IO_RESOURCE(Fcb);
        UDFReleaseResource(&Fcb->FcbNonpaged->FcbResource);
        AcquiredFCB = FALSE;

        if (AcquiredParentFCB && Fcb->FileInfo->ParentFile) {
            UDF_CHECK_PAGING_IO_RESOURCE(Fcb->FileInfo->ParentFile->Fcb);
            UDFReleaseResource(&Fcb->FileInfo->ParentFile->Fcb->FcbNonpaged->FcbResource);
            AcquiredParentFCB = FALSE;
        }

        // Close the target file's FileInfo - this decrements FileInfo->RefCount
        // Parent FileInfo references are now handled by LCB mechanism in UDFTeardownStructures
        ASSERT(AcquiredVcb);
        if (NextFileInfo) {
            UDFCloseFile__(IrpContext, Vcb, NextFileInfo);
        }

        Ccb->Flags |= UDF_CCB_CLEANED;

        //  We must clean up the share access at this time, since we may not
        //  get a Close call for awhile if the file was mapped through this
        //  File Object.
        IoRemoveShareAccess(FileObject, &Fcb->ShareAccess);

        Fcb->Header.IsFastIoPossible = UDFIsFastIoPossible(Fcb);

        FileObject->Flags |= FO_CLEANUP_COMPLETE;

try_exit: NOTHING;

    } _SEH2_FINALLY {

        if (AcquiredFCB) {
            UDF_CHECK_PAGING_IO_RESOURCE(Fcb);
            UDFReleaseResource(&Fcb->FcbNonpaged->FcbResource);
        }

        if (AcquiredParentFCB && Fcb->FileInfo->ParentFile) {
            UDF_CHECK_PAGING_IO_RESOURCE(Fcb->FileInfo->ParentFile->Fcb);
            UDFReleaseResource(&Fcb->FileInfo->ParentFile->Fcb->FcbNonpaged->FcbResource);
        }

        if (AcquiredVcb) {
            UDFReleaseResource(&Vcb->VcbResource);
            AcquiredVcb = FALSE;
        }

        if (SendUnlockNotification) {

            FsRtlNotifyVolumeEvent(FileObject, FSRTL_VOLUME_UNLOCK);
        }

        if (!_SEH2_AbnormalTermination()) {

                UDFCompleteRequest(IrpContext, Irp, RC);
        }

    } _SEH2_END; // end of "__finally" processing
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
