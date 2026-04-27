////////////////////////////////////////////////////////////////////
// Copyright (C) Alexander Telyatnikov, Ivan Keliukh, Yegor Anchishkin, SKIF Software, 1999-2013. Kiev, Ukraine
// All rights reserved
// This file was released under the GPLv2 on June 2015.
////////////////////////////////////////////////////////////////////
/*************************************************************************
*
* File: Env_Spec.cpp
*
* Module: UDF File System Driver (Kernel mode execution only)
*
* Description:
*   Contains environment-secific code to handle physical
*   operations: read, write and device IOCTLS
*
*************************************************************************/

#include "udffs.h"
// define the file specific bug-check id
#define         UDF_BUG_CHECK_ID        UDF_FILE_ENV_SPEC

/*

 */
NTSTATUS
NTAPI
UDFAsyncCompletionRoutine(
    IN PDEVICE_OBJECT DeviceObject,
    IN PIRP Irp,
    IN PVOID Contxt
    )
{
    UDFPrint(("UDFAsyncCompletionRoutine ctx=%x\n", Contxt));
    PUDF_PH_CALL_CONTEXT Context = (PUDF_PH_CALL_CONTEXT)Contxt;
    PMDL Mdl, NextMdl;

    Context->IosbToUse = Irp->IoStatus;
#if 1
    // Unlock pages that are described by MDL (if any)...
    Mdl = Irp->MdlAddress;
    while(Mdl) {
        MmPrint(("    Unlock MDL=%x\n", Mdl));
        MmUnlockPages(Mdl);
        Mdl = Mdl->Next;
    }
    // ... and free MDL
    Mdl = Irp->MdlAddress;
    while(Mdl) {
        MmPrint(("    Free MDL=%x\n", Mdl));
        NextMdl = Mdl->Next;
        IoFreeMdl(Mdl);
        Mdl = NextMdl;
    }
    Irp->MdlAddress = NULL;
    IoFreeIrp(Irp);

    KeSetEvent( &(Context->event), 0, FALSE );

    return STATUS_MORE_PROCESSING_REQUIRED;
#else
    KeSetEvent( &(Context->event), 0, FALSE );

    return STATUS_SUCCESS;
#endif
} // end UDFAsyncCompletionRoutine()

NTSTATUS
NTAPI
UDFSyncCompletionRoutine(
    IN PDEVICE_OBJECT DeviceObject,
    IN PIRP Irp,
    IN PVOID Contxt
    )
{
    UDFPrint(("UDFSyncCompletionRoutine ctx=%x\n", Contxt));
    PUDF_PH_CALL_CONTEXT Context = (PUDF_PH_CALL_CONTEXT)Contxt;

    Context->IosbToUse = Irp->IoStatus;
    //KeSetEvent( &(Context->event), 0, FALSE );

    return STATUS_SUCCESS;
} // end UDFSyncCompletionRoutine()

/*
NTSTATUS
UDFSyncCompletionRoutine2(
    IN PDEVICE_OBJECT DeviceObject,
    IN PIRP Irp,
    IN PVOID Contxt
    )
{
    UDFPrint(("UDFSyncCompletionRoutine2\n"));
    PKEVENT SyncEvent = (PKEVENT)Contxt;

    KeSetEvent( SyncEvent, 0, FALSE );

    return STATUS_SUCCESS;
} // end UDFSyncCompletionRoutine2()
*/

/*

 Function: UDFPhReadSynchronous()

 Description:
    UDFFSD will invoke this rotine to read physical device synchronously/asynchronously

 Expected Interrupt Level (for execution) :

  <= IRQL_DISPATCH_LEVEL

 Return Value: STATUS_SUCCESS/Error

*/
NTSTATUS
UDFPhReadSynchronous(
    PIRP_CONTEXT IrpContext,
    PDEVICE_OBJECT DeviceObject,   // the physical device object
    PVOID Buffer,
    ULONG ByteCount,
    LONGLONG Offset,
    PULONG ReadBytes,
    ULONG Flags
    )
{
    NTSTATUS            Status = STATUS_SUCCESS;
    LARGE_INTEGER       ROffset;
    PUDF_PH_CALL_CONTEXT Context;
    PIRP                Irp;
    PIO_STACK_LOCATION IrpSp;
    KIRQL               CurIrql = KeGetCurrentIrql();
    PVOID               IoBuf = NULL;
    PVCB Vcb = NULL;

    PAGED_CODE();

    ROffset.QuadPart = Offset;
    (*ReadBytes) = 0;
/*
    // DEBUG !!!
    Flags |= PH_TMP_BUFFER;
*/
    if (Flags & PH_TMP_BUFFER) {
        IoBuf = Buffer;
    } else {
        IoBuf = DbgAllocatePoolWithTag(NonPagedPool, ByteCount, 'bNWD');
    }
    if (!IoBuf) {
        UDFPrint(("    !IoBuf\n"));
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    Context = (PUDF_PH_CALL_CONTEXT)MyAllocatePool__( NonPagedPool, sizeof(UDF_PH_CALL_CONTEXT) );
    if (!Context) {
        UDFPrint(("    !Context\n"));
        try_return(Status = STATUS_INSUFFICIENT_RESOURCES);
    }
    // Create notification event object to be used to signal the request completion.
    KeInitializeEvent(&(Context->event), NotificationEvent, FALSE);

    if (TRUE || CurIrql > PASSIVE_LEVEL) {
        Irp = IoBuildAsynchronousFsdRequest(IRP_MJ_READ, DeviceObject, IoBuf,
                                               ByteCount, &ROffset, &(Context->IosbToUse) );
        if (!Irp) {
            UDFPrint(("    !irp Async\n"));
            try_return(Status = STATUS_INSUFFICIENT_RESOURCES);
        }
        MmPrint(("    Alloc async Irp MDL=%x, ctx=%x\n", Irp->MdlAddress, Context));
        IoSetCompletionRoutine(Irp, &UDFAsyncCompletionRoutine,
                                Context, TRUE, TRUE, TRUE );
    } else {
        Irp = IoBuildSynchronousFsdRequest(IRP_MJ_READ, DeviceObject, IoBuf,
                                               ByteCount, &ROffset, &(Context->event), &(Context->IosbToUse) );
        if (!Irp) {
            UDFPrint(("    !irp Sync\n"));
            try_return(Status = STATUS_INSUFFICIENT_RESOURCES);
        }
        MmPrint(("    Alloc Irp MDL=%x, ctx=%x\n", Irp->MdlAddress, Context));
    }

    // Setup the next IRP stack location in the associated Irp for the disk
    // driver beneath us.

    IrpSp = IoGetNextIrpStackLocation(Irp);

    //  If this Irp is the result of a WriteThough operation,
    //  tell the device to write it through.

    if (FlagOn(IrpContext->Flags, IRP_CONTEXT_FLAG_WRITE_THROUGH)) {

        SetFlag(IrpSp->Flags, SL_WRITE_THROUGH);
    }

    SetFlag(IrpSp->Flags, SL_OVERRIDE_VERIFY_VOLUME);

    // Send the request down to the driver. If an error occurs return
    // it to the caller.

    Status = IoCallDriver(DeviceObject, Irp);

    // If the status was STATUS_PENDING then wait on the event.

    if (Status == STATUS_PENDING) {

        Status = KeWaitForSingleObject(&Context->event,
                                       Executive,
                                       KernelMode,
                                       FALSE,
                                       NULL);
    }

    if (NT_SUCCESS(Status)) {
        (*ReadBytes) = Context->IosbToUse.Information;
    }
    if (!(Flags & PH_TMP_BUFFER)) {
        RtlCopyMemory(Buffer, IoBuf, *ReadBytes);
    }

try_exit: NOTHING;

    if (Context) MyFreePool__(Context);
    if (IoBuf && !(Flags & PH_TMP_BUFFER)) DbgFreePool(IoBuf);

    return(Status);
} // end UDFPhReadSynchronous()


/*

 Function: UDFPhWriteSynchronous()

 Description:
    UDFFSD will invoke this rotine to write physical device synchronously

 Expected Interrupt Level (for execution) :

  <= IRQL_DISPATCH_LEVEL

 Return Value: STATUS_SUCCESS/Error

*/
NTSTATUS
UDFPhWriteSynchronous(
    PDEVICE_OBJECT DeviceObject,   // the physical device object
    PVOID Buffer,
    ULONG ByteCount,
    LONGLONG Offset,
    PSIZE_T WrittenBytes,
    ULONG Flags
    )
{
    NTSTATUS            Status = STATUS_SUCCESS;
    LARGE_INTEGER       ROffset;
    PUDF_PH_CALL_CONTEXT Context = NULL;
    PIRP                irp;
    KIRQL               CurIrql = KeGetCurrentIrql();
    PVOID               IoBuf = NULL;

    PAGED_CODE();

    PVCB Vcb = NULL;

    ROffset.QuadPart = Offset;
    (*WrittenBytes) = 0;

   // Utilizing a temporary buffer to circumvent the situation where the IO buffer contains TransitionPage pages.
   // This typically occurs during IRP_NOCACHE. Otherwise, an assert occurs within IoBuildAsynchronousFsdRequest.
    if (Flags & PH_TMP_BUFFER) {
        IoBuf = Buffer;
    } else {
        IoBuf = DbgAllocatePool(NonPagedPool, ByteCount);
        if (!IoBuf) try_return (Status = STATUS_INSUFFICIENT_RESOURCES);
        RtlCopyMemory(IoBuf, Buffer, ByteCount);
    }

    Context = (PUDF_PH_CALL_CONTEXT)MyAllocatePool__( NonPagedPool, sizeof(UDF_PH_CALL_CONTEXT) );
    if (!Context) try_return (Status = STATUS_INSUFFICIENT_RESOURCES);
    // Create notification event object to be used to signal the request completion.
    KeInitializeEvent(&(Context->event), NotificationEvent, FALSE);

    if (TRUE || CurIrql > PASSIVE_LEVEL) {
        irp = IoBuildAsynchronousFsdRequest(IRP_MJ_WRITE, DeviceObject, IoBuf,
                                            ByteCount, &ROffset, &(Context->IosbToUse) );
        if (!irp) try_return(Status = STATUS_INSUFFICIENT_RESOURCES);
        MmPrint(("    Alloc async Irp MDL=%x, ctx=%x\n", irp->MdlAddress, Context));
        IoSetCompletionRoutine( irp, &UDFAsyncCompletionRoutine,
                                Context, TRUE, TRUE, TRUE );
    } else {
        irp = IoBuildSynchronousFsdRequest(IRP_MJ_WRITE, DeviceObject, IoBuf,
                                           ByteCount, &ROffset, &(Context->event), &(Context->IosbToUse) );
        if (!irp) try_return(Status = STATUS_INSUFFICIENT_RESOURCES);
        MmPrint(("    Alloc Irp MDL=%x\n, ctx=%x", irp->MdlAddress, Context));
    }

    (IoGetNextIrpStackLocation(irp))->Flags |= SL_OVERRIDE_VERIFY_VOLUME;

    Status = IoCallDriver(DeviceObject, irp);

    if (Status == STATUS_PENDING) {

        Status = KeWaitForSingleObject(&Context->event,
                                       Executive,
                                       KernelMode,
                                       FALSE,
                                       NULL);

    }

    if (NT_SUCCESS(Status)) {

        (*WrittenBytes) = Context->IosbToUse.Information;
    }

try_exit: NOTHING;

    if (Context) MyFreePool__(Context);
    if (IoBuf && !(Flags & PH_TMP_BUFFER)) DbgFreePool(IoBuf);

    return(Status);
} // end UDFPhWriteSynchronous()

NTSTATUS
NTAPI
UDFTSendIOCTL(
    IN ULONG IoControlCode,
    IN PVCB Vcb,
    IN PVOID InputBuffer ,
    IN ULONG InputBufferLength,
    OUT PVOID OutputBuffer ,
    IN ULONG OutputBufferLength,
    IN BOOLEAN OverrideVerify,
    OUT PIO_STATUS_BLOCK Iosb OPTIONAL
    )
{
    NTSTATUS            RC = STATUS_SUCCESS;
    BOOLEAN Acquired;

    Acquired = UDFAcquireResourceExclusiveWithCheck(&(Vcb->IoResource));

    _SEH2_TRY {

        RC = UDFPerformDevIoCtrl(IoControlCode,
                            Vcb->TargetDeviceObject,
                            InputBuffer ,
                            InputBufferLength,
                            OutputBuffer ,
                            OutputBufferLength,
                            OverrideVerify,
                            Iosb
                            );

    } _SEH2_FINALLY {
        if (Acquired)
            UDFReleaseResource(&(Vcb->IoResource));
    } _SEH2_END;

    return RC;
} // end UDFTSendIOCTL()

/*

 Function: UDFPerformDevIoCtrl()

 Description:
    UDF FSD will invoke this rotine to send IOCTL's to physical
    device

 Return Value: STATUS_SUCCESS/Error

*/
NTSTATUS
UDFPerformDevIoCtrl(
    IN ULONG IoControlCode,
    IN PDEVICE_OBJECT DeviceObject,
    IN PVOID InputBuffer ,
    IN ULONG InputBufferLength,
    OUT PVOID OutputBuffer ,
    IN ULONG OutputBufferLength,
    IN BOOLEAN OverrideVerify,
    OUT PIO_STATUS_BLOCK Iosb OPTIONAL
    )
{
    NTSTATUS Status;
    KEVENT Event;
    PIRP Irp;
    IO_STATUS_BLOCK LocalIosb;
    PIO_STATUS_BLOCK IosbToUse = &LocalIosb;

    PAGED_CODE();

    // Check if the user gave us an Iosb.

    if (ARGUMENT_PRESENT(Iosb)) {

        IosbToUse = Iosb;
    }

    IosbToUse->Status = 0;
    IosbToUse->Information = 0;

    // Initialize the event.

    KeInitializeEvent(&Event, NotificationEvent, FALSE);

    // Attempt to allocate the IRP.  If unsuccessful, raise
    // STATUS_INSUFFICIENT_RESOURCES.

    Irp = IoBuildDeviceIoControlRequest(IoControlCode,
        DeviceObject,
        InputBuffer,
        InputBufferLength,
        OutputBuffer,
        OutputBufferLength,
        FALSE,
        &Event,
        IosbToUse);

    if (!Irp) {

        return STATUS_INSUFFICIENT_RESOURCES;
    }

    if (OverrideVerify) {

        SetFlag(IoGetNextIrpStackLocation(Irp)->Flags, SL_OVERRIDE_VERIFY_VOLUME);
    }

    Status = IoCallDriver(DeviceObject, Irp);

    // We check for device not ready by first checking Status
    // and then if status pending was returned, the Iosb status
    // value.

    if (Status == STATUS_PENDING) {

        Status = KeWaitForSingleObject(&Event,
                                       Executive,
                                       KernelMode,
                                       FALSE,
                                       NULL);

        Status = IosbToUse->Status;
    }

    NT_ASSERT(!(OverrideVerify && (STATUS_VERIFY_REQUIRED == Status)));

    return Status;
} // end UDFPerformDevIoCtrl()

VOID
UDFNotifyReportChange(
    PIRP_CONTEXT IrpContext,
    PVCB Vcb,
    PFCB Fcb,
    ULONG Filter,
    ULONG Action,
    PLCB Lcb,
    PFILE_OBJECT FileObject
    )
{
    USHORT TargetNameOffset = 0;
    UNICODE_STRING FullPath;
    BOOLEAN PathAllocated = FALSE;
    WCHAR RootChar;

    FullPath.Buffer = NULL;
    FullPath.Length = 0;
    FullPath.MaximumLength = 0;

    //
    // Acquire FcbResource shared for the duration of the notify.
    // Protects LCB chain and name buffers from concurrent teardown.
    //
    UDFAcquireFcbShared(IrpContext, Fcb, FALSE);

    _SEH2_TRY {

        // If no LCB provided, find first valid (non-deleted) one
        if (!Lcb) {
            if (!IsListEmpty(&Fcb->ParentLcbQueue)) {
                PLIST_ENTRY ListEntry = Fcb->ParentLcbQueue.Flink;
                while (ListEntry != &Fcb->ParentLcbQueue) {
                    PLCB CandidateLcb = CONTAINING_RECORD(ListEntry, LCB, ChildFcbLinks);
                    if (!(CandidateLcb->Flags & UDF_LCB_FLAG_LINK_DELETED)) {
                        Lcb = CandidateLcb;
                        break;
                    }
                    ListEntry = ListEntry->Flink;
                }
            }
        }

        //
        // Build the full target name. Prefer FileObject->FileName (stable
        // pointer, does not depend on LCB chain state). Fall back to
        // building from LCB chain.
        //
        if (FileObject && FileObject->FileName.Buffer &&
            FileObject->FileName.Length > 0) {
            FullPath = FileObject->FileName;
        } else if (Lcb) {
            NTSTATUS Status = UDFBuildFullPathFromLcb(NULL, Lcb, &FullPath, FALSE);
            if (NT_SUCCESS(Status)) {
                PathAllocated = TRUE;
            }
        }

        // Fallback to root
        if (!FullPath.Buffer || FullPath.Length == 0) {
            RootChar = L'\\';
            FullPath.Buffer = &RootChar;
            FullPath.Length = sizeof(WCHAR);
            FullPath.MaximumLength = sizeof(WCHAR);
            PathAllocated = FALSE;
            Lcb = NULL;
        }

        // TargetNameOffset: byte offset within FullPath to the final name component
        if (Lcb && Lcb->ExactCaseLinkName.Length > 0 &&
            FullPath.Length > Lcb->ExactCaseLinkName.Length) {
            TargetNameOffset = FullPath.Length - Lcb->ExactCaseLinkName.Length;
        }

        FsRtlNotifyFullReportChange(Vcb->NotifySync,
                                    &Vcb->NextNotifyIRP,
                                    (PSTRING)&FullPath,
                                    TargetNameOffset,
                                    NULL,
                                    NULL,
                                    Filter,
                                    Action,
                                    NULL);

    } _SEH2_EXCEPT(EXCEPTION_EXECUTE_HANDLER) {
        NOTHING;
    } _SEH2_END;

    UDFReleaseFcb(IrpContext, Fcb);

    if (PathAllocated && FullPath.Buffer) {
        ExFreePool(FullPath.Buffer);
    }
}

