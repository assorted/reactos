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

#ifdef DBG
ULONG UDF_SIMULATE_WRITES=0;
#endif //DBG

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
    NTSTATUS            RC = STATUS_SUCCESS;
    LARGE_INTEGER       ROffset;
    PUDF_PH_CALL_CONTEXT Context;
    PIRP                Irp;
    PIO_STACK_LOCATION IrpSp;
    PVOID               IoBuf = NULL;
    PVCB Vcb = NULL;

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
        try_return(RC = STATUS_INSUFFICIENT_RESOURCES);
    }
    // Create notification event object to be used to signal the request completion.
    KeInitializeEvent(&(Context->event), NotificationEvent, FALSE);

    {
        // Use MmCreateMdl instead of IoAllocateMdl so that large buffers
        // (> ~64 MB) are not rejected on Windows XP/Server 2003, where
        // IoAllocateMdl returns NULL when the MDL size exceeds MAXUSHORT.
        // MmProbeAndLockPages (like IoBuildAsynchronousFsdRequest does) sets
        // MDL_PAGES_LOCKED without MDL_SOURCE_IS_NONPAGED_POOL, making
        // MmUnlockPages safe in the async completion routine.
        PMDL Mdl = MmCreateMdl(NULL, IoBuf, ByteCount);
        if (!Mdl) {
            UDFPrint(("    !Mdl\n"));
            try_return(RC = STATUS_INSUFFICIENT_RESOURCES);
        }
        MmProbeAndLockPages(Mdl, KernelMode, IoWriteAccess);
        Irp = IoAllocateIrp(DeviceObject->StackSize, FALSE);
        if (!Irp) {
            UDFPrint(("    !irp\n"));
            MmUnlockPages(Mdl);
            IoFreeMdl(Mdl);
            try_return(RC = STATUS_INSUFFICIENT_RESOURCES);
        }
        Irp->MdlAddress = Mdl;
        Irp->UserIosb = &(Context->IosbToUse);
        Irp->UserEvent = NULL;
        Irp->RequestorMode = KernelMode;
        Irp->Tail.Overlay.Thread = PsGetCurrentThread();
        MmPrint(("    Alloc Irp MDL=%x, ctx=%x\n", Irp->MdlAddress, Context));
        IoSetCompletionRoutine(Irp, &UDFAsyncCompletionRoutine,
                                Context, TRUE, TRUE, TRUE);
    }

    // Setup the next IRP stack location in the associated Irp for the disk
    // driver beneath us.

    IrpSp = IoGetNextIrpStackLocation(Irp);
    IrpSp->MajorFunction = IRP_MJ_READ;
    IrpSp->Parameters.Read.Length = ByteCount;
    IrpSp->Parameters.Read.ByteOffset = ROffset;

    //  If this Irp is the result of a WriteThough operation,
    //  tell the device to write it through.

    if (FlagOn(IrpContext->Flags, IRP_CONTEXT_FLAG_WRITE_THROUGH)) {

        SetFlag(IrpSp->Flags, SL_WRITE_THROUGH);
    }

    SetFlag(IrpSp->Flags, SL_OVERRIDE_VERIFY_VOLUME);

    RC = IoCallDriver(DeviceObject, Irp);

    if (RC == STATUS_PENDING) {
        DbgWaitForSingleObject(&(Context->event), NULL);
        if ((RC = Context->IosbToUse.Status) == STATUS_DATA_OVERRUN) {
            RC = STATUS_SUCCESS;
        }
//        *ReadBytes = Context->IosbToUse.Information;
    } else {
//        *ReadBytes = irp->IoStatus.Information;
    }
    if (NT_SUCCESS(RC)) {
        (*ReadBytes) = Context->IosbToUse.Information;
    }
    if (!(Flags & PH_TMP_BUFFER)) {
        RtlCopyMemory(Buffer, IoBuf, *ReadBytes);
    }

try_exit: NOTHING;

    if (Context) MyFreePool__(Context);
    if (IoBuf && !(Flags & PH_TMP_BUFFER)) DbgFreePool(IoBuf);

    return(RC);
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
    NTSTATUS            RC = STATUS_SUCCESS;
    LARGE_INTEGER       ROffset;
    PUDF_PH_CALL_CONTEXT Context = NULL;
    PIRP                irp;
    PVOID               IoBuf = NULL;

    PVCB Vcb = NULL;

#ifdef DBG
    if (UDF_SIMULATE_WRITES) {
/* FIXME ReactOS
   If this function is to force a read from the bufffer to simulate any segfaults, then it makes sense.
   Else, this forloop is useless.
        UCHAR a;
        for(ULONG i=0; i<Length; i++) {
            a = ((PUCHAR)Buffer)[i];
        }
*/
        *WrittenBytes = ByteCount;
        return STATUS_SUCCESS;
    }
#endif //DBG

    ROffset.QuadPart = Offset;
    (*WrittenBytes) = 0;

   // Utilizing a temporary buffer to circumvent the situation where the IO buffer contains TransitionPage pages.
   // This typically occurs during IRP_NOCACHE. The buffer must be in NonPagedPool for MmProbeAndLockPages.
    if (Flags & PH_TMP_BUFFER) {
        IoBuf = Buffer;
    } else {
        IoBuf = DbgAllocatePool(NonPagedPool, ByteCount);
        if (!IoBuf) try_return (RC = STATUS_INSUFFICIENT_RESOURCES);
        RtlCopyMemory(IoBuf, Buffer, ByteCount);
    }

    Context = (PUDF_PH_CALL_CONTEXT)MyAllocatePool__( NonPagedPool, sizeof(UDF_PH_CALL_CONTEXT) );
    if (!Context) try_return (RC = STATUS_INSUFFICIENT_RESOURCES);
    // Create notification event object to be used to signal the request completion.
    KeInitializeEvent(&(Context->event), NotificationEvent, FALSE);

    {
        // Use MmCreateMdl instead of IoAllocateMdl so that large buffers
        // (> ~64 MB) are not rejected on Windows XP/Server 2003, where
        // IoAllocateMdl returns NULL when the MDL size exceeds MAXUSHORT.
        // MmProbeAndLockPages (like IoBuildAsynchronousFsdRequest does) sets
        // MDL_PAGES_LOCKED without MDL_SOURCE_IS_NONPAGED_POOL, making
        // MmUnlockPages safe in the async completion routine.
        PMDL Mdl = MmCreateMdl(NULL, IoBuf, ByteCount);
        if (!Mdl) {
            UDFPrint(("    !Mdl\n"));
            try_return(RC = STATUS_INSUFFICIENT_RESOURCES);
        }
        MmProbeAndLockPages(Mdl, KernelMode, IoReadAccess);
        irp = IoAllocateIrp(DeviceObject->StackSize, FALSE);
        if (!irp) {
            UDFPrint(("    !irp\n"));
            MmUnlockPages(Mdl);
            IoFreeMdl(Mdl);
            try_return(RC = STATUS_INSUFFICIENT_RESOURCES);
        }
        irp->MdlAddress = Mdl;
        irp->UserIosb = &(Context->IosbToUse);
        irp->UserEvent = NULL;
        irp->RequestorMode = KernelMode;
        irp->Tail.Overlay.Thread = PsGetCurrentThread();
        MmPrint(("    Alloc Irp MDL=%x, ctx=%x\n", irp->MdlAddress, Context));
        IoSetCompletionRoutine(irp, &UDFAsyncCompletionRoutine,
                                Context, TRUE, TRUE, TRUE);
    }

    PIO_STACK_LOCATION IrpSp = IoGetNextIrpStackLocation(irp);
    IrpSp->MajorFunction = IRP_MJ_WRITE;
    IrpSp->Parameters.Write.Length = ByteCount;
    IrpSp->Parameters.Write.ByteOffset = ROffset;
    IrpSp->Flags |= SL_OVERRIDE_VERIFY_VOLUME;
    RC = IoCallDriver(DeviceObject, irp);

    if (RC == STATUS_PENDING) {
        DbgWaitForSingleObject(&(Context->event), NULL);
        if ((RC = Context->IosbToUse.Status) == STATUS_DATA_OVERRUN) {
            RC = STATUS_SUCCESS;
        }
//        *WrittenBytes = Context->IosbToUse.Information;
    } else {
//        *WrittenBytes = irp->IoStatus.Information;
    }
    if (NT_SUCCESS(RC)) {
        (*WrittenBytes) = Context->IosbToUse.Information;
    }

try_exit: NOTHING;

    if (Context) MyFreePool__(Context);
    if (IoBuf && !(Flags & PH_TMP_BUFFER)) DbgFreePool(IoBuf);
    if (!NT_SUCCESS(RC)) {
        UDFPrint(("WriteError\n"));
    }

    return(RC);
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

        RC = UDFPhSendIOCTL(IoControlCode,
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

 Function: UDFPhSendIOCTL()

 Description:
    UDF FSD will invoke this rotine to send IOCTL's to physical
    device

 Return Value: STATUS_SUCCESS/Error

*/
NTSTATUS
NTAPI
UDFPhSendIOCTL(
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
    NTSTATUS            RC = STATUS_SUCCESS;
    PIRP                irp;
    PUDF_PH_CALL_CONTEXT Context;
    LARGE_INTEGER timeout;

    UDFPrint(("UDFPhDevIOCTL: Code %8x  \n",IoControlCode));

    Context = (PUDF_PH_CALL_CONTEXT)MyAllocatePool__( NonPagedPool, sizeof(UDF_PH_CALL_CONTEXT) );
    if (!Context) return STATUS_INSUFFICIENT_RESOURCES;
    //  Check if the user gave us an Iosb.

    // Create notification event object to be used to signal the request completion.
    KeInitializeEvent(&(Context->event), NotificationEvent, FALSE);

    irp = IoBuildDeviceIoControlRequest(IoControlCode, DeviceObject, InputBuffer ,
        InputBufferLength, OutputBuffer, OutputBufferLength,FALSE,&(Context->event),&(Context->IosbToUse));

    if (!irp) try_return (RC = STATUS_INSUFFICIENT_RESOURCES);
    MmPrint(("    Alloc Irp MDL=%x, ctx=%x\n", irp->MdlAddress, Context));
/*
    if (KeGetCurrentIrql() > PASSIVE_LEVEL) {
        UDFPrint(("Setting completion routine\n"));
        IoSetCompletionRoutine( irp, &UDFSyncCompletionRoutine,
                                Context, TRUE, TRUE, TRUE );
    }
*/
    if (OverrideVerify) {
        (IoGetNextIrpStackLocation(irp))->Flags |= SL_OVERRIDE_VERIFY_VOLUME;
    }

    RC = IoCallDriver(DeviceObject, irp);

    if (RC == STATUS_PENDING) {
        ASSERT(KeGetCurrentIrql() < DISPATCH_LEVEL);
        UDFPrint(("Enter wait state on evt %x\n", Context));

        if (KeGetCurrentIrql() > PASSIVE_LEVEL) {
            timeout.QuadPart = -1000;
            UDFPrint(("waiting, TO=%I64d\n", timeout.QuadPart));
            RC = DbgWaitForSingleObject(&(Context->event), &timeout);
            while(RC == STATUS_TIMEOUT) {
                timeout.QuadPart *= 2;
                UDFPrint(("waiting, TO=%I64d\n", timeout.QuadPart));
                RC = DbgWaitForSingleObject(&(Context->event), &timeout);
            }

        } else {
            DbgWaitForSingleObject(&(Context->event), NULL);
        }
        if ((RC = Context->IosbToUse.Status) == STATUS_DATA_OVERRUN) {
            RC = STATUS_SUCCESS;
        }
        UDFPrint(("Exit wait state on evt %x, status %8.8x\n", Context, RC));
/*        if (Iosb) {
            (*Iosb) = Context->IosbToUse;
        }*/
    } else {
        UDFPrint(("No wait completion on evt %x\n", Context));
/*        if (Iosb) {
            (*Iosb) = irp->IoStatus;
        }*/
    }

    if (Iosb) {
        (*Iosb) = Context->IosbToUse;
    }

try_exit: NOTHING;

    if (Context) MyFreePool__(Context);
    return(RC);
} // end UDFPhSendIOCTL()

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

