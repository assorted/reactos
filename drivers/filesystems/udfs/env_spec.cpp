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
    PUDF_PH_CALL_CONTEXT Context = (PUDF_PH_CALL_CONTEXT)Contxt;
    PMDL Mdl, NextMdl;

    UDFPrint(("UDFAsyncCompletionRoutine ctx=%x\n", Contxt));

    // 1. Capture the final status and information (bytes read)
    Context->IosbToUse = Irp->IoStatus;

    // 2. Cleanup MDLs manually since we used IoBuildAsynchronousFsdRequest
    Mdl = Irp->MdlAddress;
    while (Mdl) {
        NextMdl = Mdl->Next;
        
        // Only unlock if the MDL was actually locked (has the MDL_PAGES_LOCKED flag)
        if (FlagOn(Mdl->MdlFlags, MDL_PAGES_LOCKED)) {
            MmUnlockPages(Mdl);
        }
        
        IoFreeMdl(Mdl);
        Mdl = NextMdl;
    }
    Irp->MdlAddress = NULL;

    // 3. Free the IRP itself
    IoFreeIrp(Irp);

    // 4. SIGNAL THE EVENT - This releases the thread waiting in UDFPhReadSynchronous
    KeSetEvent(&(Context->event), 0, FALSE);

    // 5. Tell the I/O manager to stop - we have fully disposed of the IRP
    return STATUS_MORE_PROCESSING_REQUIRED;
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
    IN PIRP_CONTEXT IrpContext,
    IN PDEVICE_OBJECT DeviceObject,   // The physical device object
    IN PVOID Buffer,
    IN ULONG ByteCount,
    IN LONGLONG Offset,               // 64-bit offset (critical for 256GB)
    OUT PULONG ReadBytes,
    IN ULONG Flags
    )
{
    NTSTATUS RC = STATUS_SUCCESS;
    LARGE_INTEGER ROffset;
    PUDF_PH_CALL_CONTEXT Context = NULL;
    PIRP Irp = NULL;
    PIO_STACK_LOCATION IrpSp;
    PVOID IoBuf = NULL;

    ROffset.QuadPart = Offset;
    (*ReadBytes) = 0;

    // 1. Memory Allocation: Use a temporary buffer if requested or for alignment safety
    if (Flags & PH_TMP_BUFFER) {
        IoBuf = Buffer;
    } else {
        IoBuf = DbgAllocatePoolWithTag(NonPagedPool, ByteCount, 'bNWD');
        if (!IoBuf) {
            UDFPrint(("    UDFPhRead: Failed to allocate IoBuf\n"));
            return STATUS_INSUFFICIENT_RESOURCES;
        }
    }

    // 2. Context Allocation: To track the IRP completion and event signaling
    Context = (PUDF_PH_CALL_CONTEXT)MyAllocatePool__(NonPagedPool, sizeof(UDF_PH_CALL_CONTEXT));
    if (!Context) {
        UDFPrint(("    UDFPhRead: Failed to allocate Context\n"));
        if (!(Flags & PH_TMP_BUFFER)) DbgFreePool(IoBuf);
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    // 3. Initialize the synchronization event
    KeInitializeEvent(&(Context->event), NotificationEvent, FALSE);

    // 4. Build the IRP: Use Asynchronous to have full control over the completion routine
    Irp = IoBuildAsynchronousFsdRequest(
        IRP_MJ_READ, 
        DeviceObject, 
        IoBuf,
        ByteCount, 
        &ROffset, 
        &(Context->IosbToUse)
    );

    if (!Irp) {
        UDFPrint(("    UDFPhRead: Failed to build IRP\n"));
        MyFreePool__(Context);
        if (!(Flags & PH_TMP_BUFFER)) DbgFreePool(IoBuf);
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    // 5. Setup Completion Routine: This MUST signal the event in the context
    IoSetCompletionRoutine(
        Irp, 
        &UDFAsyncCompletionRoutine,
        Context, 
        TRUE, 
        TRUE, 
        TRUE
    );

    // 6. Setup Stack Flags
    IrpSp = IoGetNextIrpStackLocation(Irp);
    if (FlagOn(IrpContext->Flags, IRP_CONTEXT_FLAG_WRITE_THROUGH)) {
        SetFlag(IrpSp->Flags, SL_WRITE_THROUGH);
    }
    SetFlag(IrpSp->Flags, SL_OVERRIDE_VERIFY_VOLUME);

    // 7. Call the Disk Driver
    RC = IoCallDriver(DeviceObject, Irp);

    // 8. Strict Wait: Always wait for the event if the status is PENDING.
    // On 256GB VHDs, disk I/O is rarely immediate.
    if (RC == STATUS_PENDING) {
        KeWaitForSingleObject(&(Context->event), Executive, KernelMode, FALSE, NULL);
        RC = Context->IosbToUse.Status;
    }

    // Special handling for Data Overrun (common in some ReactOS storage stacks)
    if (RC == STATUS_DATA_OVERRUN) {
        RC = STATUS_SUCCESS;
    }

    // 9. Data Transfer: Only copy if the read was successful
    if (NT_SUCCESS(RC)) {
        *ReadBytes = (ULONG)Context->IosbToUse.Information;
        
        // Ensure we don't copy more than requested
        if (*ReadBytes > ByteCount) *ReadBytes = ByteCount;

        if (!(Flags & PH_TMP_BUFFER)) {
            RtlCopyMemory(Buffer, IoBuf, *ReadBytes);
        }
    } else {
        UDFPrint(("UDF: PhRead Error %x at LBA %llx\n", RC, Offset >> 11));
    }

    // 10. Final Cleanup
    if (Context) MyFreePool__(Context);
    if (IoBuf && !(Flags & PH_TMP_BUFFER)) DbgFreePool(IoBuf);

    return RC;
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
    KIRQL               CurIrql = KeGetCurrentIrql();
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
   // This typically occurs during IRP_NOCACHE. Otherwise, an assert occurs within IoBuildAsynchronousFsdRequest.
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

    if (TRUE || CurIrql > PASSIVE_LEVEL) {
        irp = IoBuildAsynchronousFsdRequest(IRP_MJ_WRITE, DeviceObject, IoBuf,
                                            ByteCount, &ROffset, &(Context->IosbToUse) );
        if (!irp) try_return(RC = STATUS_INSUFFICIENT_RESOURCES);
        MmPrint(("    Alloc async Irp MDL=%x, ctx=%x\n", irp->MdlAddress, Context));
        IoSetCompletionRoutine( irp, &UDFAsyncCompletionRoutine,
                                Context, TRUE, TRUE, TRUE );
    } else {
        irp = IoBuildSynchronousFsdRequest(IRP_MJ_WRITE, DeviceObject, IoBuf,
                                           ByteCount, &ROffset, &(Context->event), &(Context->IosbToUse) );
        if (!irp) try_return(RC = STATUS_INSUFFICIENT_RESOURCES);
        MmPrint(("    Alloc Irp MDL=%x\n, ctx=%x", irp->MdlAddress, Context));
    }

    (IoGetNextIrpStackLocation(irp))->Flags |= SL_OVERRIDE_VERIFY_VOLUME;
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
UDFNotifyFullReportChange(
    PVCB Vcb,
    PFCB Fcb,
    ULONG Filter,
    ULONG Action
    )
{
    USHORT TargetNameOffset = 0;

    // Skip parent name length and leading backslash from the beginning of object name

    if (Fcb->ParentFcb) {

        if (Fcb->ParentFcb->FCBName->ObjectName.Length == 2) {

            ASSERT(Fcb->ParentFcb->FCBName->ObjectName.Buffer[0] == L'\\');
            TargetNameOffset = Fcb->ParentFcb->FCBName->ObjectName.Length;
        }
        else {

            TargetNameOffset = Fcb->ParentFcb->FCBName->ObjectName.Length + sizeof(WCHAR);
        }
    }

    FsRtlNotifyFullReportChange(Vcb->NotifySync,
                                &Vcb->NextNotifyIRP,
                                (PSTRING)&Fcb->FCBName->ObjectName,
                                TargetNameOffset,
                                NULL,
                                NULL,
                                Filter,
                                Action,
                                NULL);
}

