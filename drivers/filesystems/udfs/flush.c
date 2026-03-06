////////////////////////////////////////////////////////////////////
// Copyright (C) Alexander Telyatnikov, Ivan Keliukh, Yegor Anchishkin, SKIF Software, 1999-2013. Kiev, Ukraine
// All rights reserved
// This file was released under the GPLv2 on June 2015.
////////////////////////////////////////////////////////////////////
/*************************************************************************
*
* File: Flush.c
*
* Module: UDF File System Driver (Kernel mode execution only)
*
* Description:
*   Contains code to handle the "Flush Buffers" dispatch entry point.
*
*************************************************************************/

#include            "udffs.h"

// define the file specific bug-check id
#define         UDF_BUG_CHECK_ID                UDF_FILE_FLUSH

/*************************************************************************
*
* Function: UDFCommonFlush()
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
* Return Value: STATUS_SUCCESS/Error
*
*************************************************************************/
NTSTATUS
UDFCommonFlush(
    PIRP_CONTEXT IrpContext,
    PIRP             Irp
    )
{
    NTSTATUS            Status = STATUS_SUCCESS;
    PIO_STACK_LOCATION  IrpSp = NULL;
    TYPE_OF_OPEN        TypeOfOpen;
    PFCB                Fcb = NULL;
    PCCB                Ccb = NULL;
    PVCB                Vcb = NULL;
    BOOLEAN             AcquiredVCB = FALSE;
    BOOLEAN             AcquiredFCB = FALSE;
    BOOLEAN             AcquiredParentFcb = FALSE;

    PAGED_CODE();

    IrpSp = IoGetCurrentIrpStackLocation(Irp);

    // Decode the file object

    TypeOfOpen = UDFDecodeFileObject(IrpSp->FileObject, &Fcb, &Ccb);

    Vcb = IrpContext->Vcb;

    ASSERT_CCB(Ccb);
    ASSERT_FCB(Fcb);
    ASSERT_VCB(Vcb);

    if (Vcb->VcbState & VCB_STATE_VOLUME_READ_ONLY) {

        if (Vcb->VcbState & VCB_STATE_MEDIA_WRITE_PROTECT) {

            Status = STATUS_MEDIA_WRITE_PROTECTED;
        }
        else if (Vcb->VcbState & VCB_STATE_MOUNTED_DIRTY) {

            Status = STATUS_VOLUME_DIRTY;
        }
        else {

            Status = STATUS_ACCESS_DENIED;
        }

        UDFCompleteRequest(IrpContext, Irp, Status);
        return Status;
    }

    // Check for invalid call from user mode

    if (IrpSp->MinorFunction == IRP_MN_MOUNT_VOLUME && Irp->RequestorMode == UserMode) {

        Status = STATUS_INVALID_PARAMETER;
        UDFCompleteRequest(IrpContext, Irp, Status);
        return Status;
    }

    //  CcFlushCache is always synchronous, so if we can't wait enqueue
    //  the irp to the Fsp.

    if (!FlagOn(IrpContext->Flags, IRP_CONTEXT_FLAG_WAIT)) {

        Status = UDFFsdPostRequest(IrpContext, Irp);

        return Status;
    }

    Status = STATUS_SUCCESS;

    _SEH2_TRY {

        // Check the type of object passed-in. That will determine the course of
        // action we take.
        if ((Fcb == Fcb->Vcb->VolumeDasdFcb) || (Fcb->FcbState & UDF_FCB_ROOT_DIRECTORY)) {

            UDFFspClose(Vcb);

            UDFAcquireVcbExclusive(IrpContext, Vcb, FALSE);
            AcquiredVCB = TRUE;
            // The caller wishes to flush all files for the mounted
            // logical volume. The flush volume routine below should simply
            // walk through all of the open file streams, acquire the
            // VCB resource, and request the flush operation from the Cache
            // Manager. Basically, the sequence of operations listed below
            // for a single file should be executed on all open files.

            UDFVerifyVcb(IrpContext, Vcb);

            UDFFlushVolume(IrpContext, Vcb, 0);

            UDFReleaseVcb(IrpContext, Vcb);
            AcquiredVCB = FALSE;

            try_return(Status);
        } else
        if (!(Fcb->FcbState & UDF_FCB_DIRECTORY)) {
            // This is a regular file.
            Vcb = Fcb->Vcb;
            ASSERT(Vcb);

            // Child-first lock ordering
            UDF_CHECK_PAGING_IO_RESOURCE(Fcb);
            UDFAcquireFcbExclusive(IrpContext, Fcb, FALSE);
            AcquiredFCB = TRUE;

            // Parent second (needed for DirIndex modification in UDFSetFileSizeInDirNdx)
            if (Fcb->FileInfo->ParentFile && Fcb->FileInfo->ParentFile->Fcb) {
                UDF_CHECK_PAGING_IO_RESOURCE(Fcb->FileInfo->ParentFile->Fcb);
                UDFAcquireFcbExclusive(IrpContext, Fcb->FileInfo->ParentFile->Fcb, FALSE);
                AcquiredParentFcb = TRUE;
            }

            // Request the Cache Manager to perform a flush operation.
            // Further, instruct the Cache Manager that we wish to flush the
            // entire file stream.
            UDFFlushAFile(IrpContext, Fcb, Ccb, &(Irp->IoStatus), 0);
            Status = Irp->IoStatus.Status;

            // Some log-based FSD implementations may wish to flush their
            // log files at this time. Finally, we should update the time-stamp
            // values for the file stream appropriately. This would involve
            // obtaining the current time and modifying the appropriate directory
            // entry fields.
        } else {
            Vcb = Fcb->Vcb;
        }

try_exit:   NOTHING;

    } _SEH2_FINALLY {

        // Release in reverse order of acquisition (parent first, then child)
        if (AcquiredParentFcb) {
            UDFReleaseFcb(IrpContext, Fcb->FileInfo->ParentFile->Fcb);
            AcquiredParentFcb = FALSE;
        }

        if (AcquiredFCB) {
            UDFReleaseFcb(IrpContext, Fcb);
            AcquiredFCB = FALSE;
        }

        if (AcquiredVCB) {
            UDFReleaseVcb(IrpContext, Vcb);
            AcquiredVCB = FALSE;
        }

        if (!_SEH2_AbnormalTermination()) {

            NTSTATUS DriverStatus;

            // Get the next stack location, and copy over the stack location

            IoCopyCurrentIrpStackLocationToNext(Irp);

            // Set up the completion routine

            IoSetCompletionRoutine(Irp,
                                   UDFFlushCompletion,
                                   ULongToPtr(Status),
                                   TRUE,
                                   TRUE,
                                   TRUE);

            // Send the request.

            DriverStatus = IoCallDriver(Vcb->TargetDeviceObject, Irp);

            if ((DriverStatus == STATUS_PENDING) || 
                (!NT_SUCCESS(DriverStatus) &&
                (DriverStatus != STATUS_INVALID_DEVICE_REQUEST))) {

                Status = DriverStatus;
            }

            // Release the IRP context at this time.
            UDFCompleteRequest(IrpContext, NULL, STATUS_SUCCESS);
        }
    } _SEH2_END;

    return Status;
} // end UDFCommonFlush()


/*************************************************************************
*
* Function: UDFFlushAFile()
*
* Description:
*   Tell the Cache Manager to perform a flush.
*
* Expected Interrupt Level (for execution) :
*
*  IRQL_PASSIVE_LEVEL
*
* Return Value: None
*
*************************************************************************/
ULONG
UDFFlushAFile(
    IN PIRP_CONTEXT IrpContext,
    IN PFCB                Fcb,
    IN PCCB                Ccb,
    OUT PIO_STATUS_BLOCK   PtrIoStatus,
    IN ULONG               FlushFlags
    )
{
    BOOLEAN SetArchive = FALSE;
//    BOOLEAN PurgeCache = FALSE;
    ULONG ret_val = 0;

    UDFPrint(("UDFFlushAFile: \n"));
    if (!Fcb)
        return 0;

    // Flush SDir if any
    _SEH2_TRY {
        if (UDFHasAStreamDir(Fcb->FileInfo) &&
           Fcb->FileInfo->Dloc->SDirInfo &&
           !UDFIsSDirDeleted(Fcb->FileInfo->Dloc->SDirInfo) ) {
            ret_val |=
                UDFFlushADirectory(IrpContext, Fcb->Vcb, Fcb->FileInfo->Dloc->SDirInfo, PtrIoStatus, FlushFlags);
        }
    } _SEH2_EXCEPT(EXCEPTION_EXECUTE_HANDLER) {
        BrutePoint();
    } _SEH2_END;
    // Flush File
    _SEH2_TRY {
        if ((Fcb->CachedOpenHandleCount || !Fcb->FcbCleanup) &&
            Fcb->FcbNonpaged->SegmentObject.DataSectionObject) {
            if (!(Fcb->NtReqFCBFlags & UDF_NTREQ_FCB_DELETED)
                                         &&
                (Fcb->NtReqFCBFlags & UDF_NTREQ_FCB_MODIFIED)) {
                MmPrint(("    CcFlushCache()\n"));
                CcFlushCache(&Fcb->FcbNonpaged->SegmentObject, NULL, 0, PtrIoStatus);
            }
            // notice, that we should purge cache
            // we can't do it now, because it may cause last Close
            // request & thus, structure deallocation
//            PurgeCache = TRUE;

            if (Ccb) {
                if ( (Ccb->FileObject->Flags & FO_FILE_MODIFIED) &&
                   !(Ccb->Flags & UDF_CCB_WRITE_TIME_SET)) {
                    if (Fcb->Vcb->CompatFlags & UDF_VCB_IC_UPDATE_MODIFY_TIME) {
                        LONGLONG NtTime;
                        KeQuerySystemTime((PLARGE_INTEGER)&NtTime);
                        UDFSetFileXTime(Fcb->FileInfo, NULL, NULL, NULL, &NtTime);
                        Fcb->LastWriteTime.QuadPart = NtTime;
                    }
                    SetArchive = TRUE;
                    Ccb->FileObject->Flags &= ~FO_FILE_MODIFIED;
                }
                if (Ccb->FileObject->Flags & FO_FILE_SIZE_CHANGED) {
                    LONGLONG ASize = UDFGetFileAllocationSize(Fcb->Vcb, Fcb->FileInfo);
                    UDFSetFileSizeInDirNdx(Fcb->Vcb, Fcb->FileInfo, &ASize);
                    Ccb->FileObject->Flags &= ~FO_FILE_SIZE_CHANGED;
                }
            }
        }
    } _SEH2_EXCEPT(EXCEPTION_EXECUTE_HANDLER) {
        BrutePoint();
    } _SEH2_END;

    _SEH2_TRY {
        if (SetArchive &&
           (Fcb->Vcb->CompatFlags & UDF_VCB_IC_UPDATE_ARCH_BIT)) {
            ULONG Attr;
            PDIR_INDEX_ITEM DirNdx;
            DirNdx = UDFDirIndex(UDFGetDirIndexByFileInfo(Fcb->FileInfo), Fcb->FileInfo->Index);
            // Archive bit
            Attr = UDFAttributesToNT(DirNdx, Fcb->FileInfo->Dloc->FileEntry);
            if (!(Attr & FILE_ATTRIBUTE_ARCHIVE))
                UDFAttributesToUDF(DirNdx, Fcb->FileInfo->Dloc->FileEntry, Attr | FILE_ATTRIBUTE_ARCHIVE);
        }

        UDFFlushFile__(IrpContext, Fcb->Vcb, Fcb->FileInfo, FlushFlags);
    } _SEH2_EXCEPT(EXCEPTION_EXECUTE_HANDLER) {
        BrutePoint();
    } _SEH2_END;

/*    if (PurgeCache) {
        _SEH2_TRY {
            MmPrint(("    CcPurgeCacheSection()\n"));
            CcPurgeCacheSection( &(Fcb->NTRequiredFCB->SectionObject), NULL, 0, FALSE );
        } _SEH2_EXCEPT(EXCEPTION_EXECUTE_HANDLER) {
            BrutePoint();
        } _SEH2_END;
    }*/

    return ret_val;
} // end UDFFlushAFile()

/*************************************************************************
*
* Function: UDFFlushADirectory()
*
* Description:
*   Tell the Cache Manager to perform a flush for all files
*   in current directory & all subdirectories and flush all metadata
*
* Expected Interrupt Level (for execution) :
*
*  IRQL_PASSIVE_LEVEL
*
* Return Value: None
*
*************************************************************************/
ULONG
UDFFlushADirectory(
    IN PIRP_CONTEXT IrpContext,
    IN PVCB                Vcb,
    IN PUDF_FILE_INFO      FI,
    OUT PIO_STATUS_BLOCK   PtrIoStatus,
    IN ULONG               FlushFlags
    )
{
    UDFPrint(("UDFFlushADirectory: \n"));
//    PDIR_INDEX_HDR hDI;
    PDIR_INDEX_ITEM DI;
//    BOOLEAN Referenced = FALSE;
    ULONG ret_val = 0;

    if (!FI || !FI->Dloc || !FI->Dloc->DirIndex) goto SkipFlushDir;
//    hDI = FI->Dloc->DirIndex;

    // Flush SDir if any
    _SEH2_TRY {
        if (UDFHasAStreamDir(FI) &&
           FI->Dloc->SDirInfo &&
           !UDFIsSDirDeleted(FI->Dloc->SDirInfo) ) {
            ret_val |=
                UDFFlushADirectory(IrpContext, Vcb, FI->Dloc->SDirInfo, PtrIoStatus, FlushFlags);
        }
    } _SEH2_EXCEPT(EXCEPTION_EXECUTE_HANDLER) {
        BrutePoint();
    } _SEH2_END;

    // Flush Dir Tree
    _SEH2_TRY {
        UDF_DIR_SCAN_CONTEXT ScanContext;
        PUDF_FILE_INFO      tempFI;

        if (UDFDirIndexInitScan(FI, &ScanContext, 2)) {
            while((DI = UDFDirIndexScan(&ScanContext, &tempFI))) {
                // Flush Dir entry
                _SEH2_TRY {
                    if (!tempFI) continue;
                    if (UDFIsADirectory(tempFI)) {
                        UDFFlushADirectory(IrpContext, Vcb, tempFI, PtrIoStatus, FlushFlags);
                    } else {
                        UDFFlushAFile(IrpContext, tempFI->Fcb, NULL, PtrIoStatus, FlushFlags);
                    }
                } _SEH2_EXCEPT(EXCEPTION_EXECUTE_HANDLER) {
                    BrutePoint();
                } _SEH2_END;
                if (UDFFlushIsBreaking(Vcb, FlushFlags)) {
                    ret_val |= UDF_FLUSH_FLAGS_INTERRUPTED;
                    break;
                }
            }
        }
    } _SEH2_EXCEPT(EXCEPTION_EXECUTE_HANDLER) {
        BrutePoint();
    } _SEH2_END;
SkipFlushDir:
    // Flush Dir
    _SEH2_TRY {
        UDFFlushFile__(IrpContext, Vcb, FI, FlushFlags );
    } _SEH2_EXCEPT(EXCEPTION_EXECUTE_HANDLER) {
        BrutePoint();
    } _SEH2_END;

    return ret_val;
} // end UDFFlushADirectory()

/*************************************************************************
*
* Function: UDFFlushLogicalVolume()
*
* Description:
*   Flush everything beginning from root directory.
*   Vcb must be previously acquired exclusively.
*
* Expected Interrupt Level (for execution) :
*
*  IRQL_PASSIVE_LEVEL
*
* Return Value: None
*
*************************************************************************/
NTSTATUS
UDFFlushVolume(
    IN PIRP_CONTEXT IrpContext,
    IN PVCB Vcb,
    IN ULONG FlushFlags
    )
{
    ULONG ret_val = 0;
    IO_STATUS_BLOCK IoStatus;

    UDFPrint(("UDFFlushVolume: \n"));

    ASSERT_EXCLUSIVE_VCB(Vcb);

    UDFFspClose(Vcb);

    _SEH2_TRY {
        if (Vcb->VcbState & VCB_STATE_VOLUME_READ_ONLY)
            return 0;
        if (Vcb->VcbCondition != VcbMounted)
            return 0;

        // NOTE: This function may also be invoked internally as part of
        // processing a shutdown request.
        ASSERT(Vcb->RootIndexFcb);
        ret_val |= UDFFlushADirectory(IrpContext, Vcb, Vcb->RootIndexFcb->FileInfo, &IoStatus, FlushFlags);

//        if (UDFFlushIsBreaking(Vcb, FlushFlags))
//            return;
        // flush internal cache
        if (FlushFlags & UDF_FLUSH_FLAGS_LITE) {
            UDFPrint(("  Lite flush, keep Modified=%d.\n", Vcb->Modified));
        } else {
            // umount (this is internal operation, NT will "dismount" volume later)
            UDFUmount__(IrpContext, Vcb);

            UDFPreClrModified(Vcb);
            UDFClrModified(Vcb);
        }

    } _SEH2_FINALLY {
        ;
    } _SEH2_END;

    return ret_val;
} // end UDFFlushLogicalVolume()


/*************************************************************************
*
* Function: UDFFlushCompletion()
*
* Description:
*   Eat up any bad errors.
*
* Expected Interrupt Level (for execution) :
*
*  IRQL_PASSIVE_LEVEL
*
* Return Value: None
*
*************************************************************************/
NTSTATUS
NTAPI
UDFFlushCompletion(
    PDEVICE_OBJECT  PtrDeviceObject,
    PIRP            Irp,
    PVOID           Context
    )
{
    NTSTATUS Status = (NTSTATUS)(ULONG_PTR)Context;

    // Add the hack-o-ramma to fix formats.

    if (Irp->PendingReturned) {

        IoMarkIrpPending(Irp);
    }

    //  If the Irp got STATUS_INVALID_DEVICE_REQUEST or a warning status,
    //  normalize it to the original status value.

    if (NT_WARNING(Irp->IoStatus.Status) || 
        Irp->IoStatus.Status == STATUS_INVALID_DEVICE_REQUEST) {

        Irp->IoStatus.Status = Status;
    }

    return STATUS_SUCCESS;
} // end UDFFlushCompletion()


/*
  Check if we should break FlushTree process
 */
BOOLEAN
UDFFlushIsBreaking(
    IN PVCB         Vcb,
    IN ULONG        FlushFlags
    )
{
    BOOLEAN ret_val = FALSE;
//    if (!(FlushFlags & UDF_FLUSH_FLAGS_BREAKABLE))
        return FALSE;
    UDFAcquireResourceExclusive(&(Vcb->FlushResource),TRUE);
    ret_val = (Vcb->VcbState & UDF_VCB_FLAGS_FLUSH_BREAK_REQ) ? TRUE : FALSE;
    Vcb->VcbState &= ~UDF_VCB_FLAGS_FLUSH_BREAK_REQ;
    UDFReleaseResource(&(Vcb->FlushResource));
    return ret_val;
} // end UDFFlushIsBreaking()

/*
  Signal FlushTree break request. Note, this is
  treated as recommendation only
 */
VOID
UDFFlushTryBreak(
    IN PVCB         Vcb
    )
{
    UDFAcquireResourceExclusive(&(Vcb->FlushResource),TRUE);
    Vcb->VcbState |= UDF_VCB_FLAGS_FLUSH_BREAK_REQ;
    UDFReleaseResource(&(Vcb->FlushResource));
} // end UDFFlushTryBreak()

//  Tell prefast this is a completion routine.
IO_COMPLETION_ROUTINE UDFHijackCompletionRoutine;

NTSTATUS
NTAPI
UDFHijackCompletionRoutine (
    PDEVICE_OBJECT DeviceObject,
    PIRP Irp,
    PVOID Contxt
    )

/*++

Routine Description:

    Completion routine for synchronizing back to dispatch.

Arguments:

    Contxt - pointer to KEVENT.

Return Value:

    STATUS_MORE_PROCESSING_REQUIRED

--*/

{
    PKEVENT Event = (PKEVENT)Contxt;
    _Analysis_assume_(Contxt != NULL);

    UNREFERENCED_PARAMETER( Irp );
    UNREFERENCED_PARAMETER( DeviceObject );

    KeSetEvent( Event, 0, FALSE );

    //  We don't want IO to get our IRP and free it.
    
    return STATUS_MORE_PROCESSING_REQUIRED;
}

NTSTATUS
UDFHijackIrpAndFlushDevice (
    _In_ PIRP_CONTEXT IrpContext,
    _Inout_ PIRP Irp,
    _In_ PDEVICE_OBJECT TargetDeviceObject
    )

/*++

Routine Description:

    This routine is called when we need to send a flush to a device but
    we don't have a flush Irp.  What this routine does is make a copy
    of its current Irp stack location, but changes the Irp Major code
    to a IRP_MJ_FLUSH_BUFFERS amd then send it down, but cut it off at
    the knees in the completion routine, fix it up and return to the
    user as if nothing had happened.

Arguments:

    Irp - The Irp to hijack

    TargetDeviceObject - The device to send the request to.

Return Value:

    NTSTATUS - The Status from the flush in case anybody cares.

--*/

{
    KEVENT Event;
    NTSTATUS Status;
    PIO_STACK_LOCATION NextIrpSp;

    PAGED_CODE();

    UNREFERENCED_PARAMETER(IrpContext);
    
    // Get the next stack location, and copy over the stack location

    NextIrpSp = IoGetNextIrpStackLocation( Irp );

    *NextIrpSp = *IoGetCurrentIrpStackLocation( Irp );

    NextIrpSp->MajorFunction = IRP_MJ_FLUSH_BUFFERS;
    NextIrpSp->MinorFunction = 0;

    //  Set up the completion routine

    KeInitializeEvent( &Event, NotificationEvent, FALSE );

    IoSetCompletionRoutine(Irp,
                           &UDFHijackCompletionRoutine,
                           &Event,
                           TRUE,
                           TRUE,
                           TRUE);

    //  Send the request.

    Status = IoCallDriver(TargetDeviceObject, Irp);

    if (Status == STATUS_PENDING) {

        (VOID)KeWaitForSingleObject(&Event, Executive, KernelMode, FALSE, NULL);

        Status = Irp->IoStatus.Status;
    }

    // If the driver doesn't support flushes, return SUCCESS.

    if (Status == STATUS_INVALID_DEVICE_REQUEST) {

        Status = STATUS_SUCCESS;
    }

    Irp->IoStatus.Status = 0;
    Irp->IoStatus.Information = 0;

    return Status;
}
