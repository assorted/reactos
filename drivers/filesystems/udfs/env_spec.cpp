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
    PUDF_PH_CALL_CONTEXT Context = NULL;
    PIRP                Irp;
    PIO_STACK_LOCATION  IrpSp;
    KIRQL               CurIrql = KeGetCurrentIrql();
    PVOID               IoBuf = NULL;
    
    // Chunking variables
    ULONG               BytesRemaining = ByteCount;
    ULONG               CurrentChunkSize = 0;
    ULONG               TotalRead = 0;
    ULONG               ChunkRead = 0;
    const ULONG         MAX_UDF_CHUNK = 1048576; // 1MB limit for NonPagedPool safety

    ROffset.QuadPart = Offset;
    (*ReadBytes) = 0;

    // --- CHUNKING LOGIC START ---
    // If the request is large and we aren't already using a temporary buffer,
    // split the request into smaller chunks to avoid NonPagedPool exhaustion.
    if (!(Flags & PH_TMP_BUFFER) && ByteCount > MAX_UDF_CHUNK) {
        
        UDFPrint(("    UDF: Chunking large read: %lu bytes at Offset %I64x\n", ByteCount, Offset));
        
        // Allocate one small reusable buffer for the chunks
        IoBuf = DbgAllocatePoolWithTag(NonPagedPool, MAX_UDF_CHUNK, 'bNWD');
        if (!IoBuf) {
            UDFPrint(("    !IoBuf (Chunk buffer allocation failed)\n"));
            return STATUS_INSUFFICIENT_RESOURCES;
        }

        while (BytesRemaining > 0) {
            CurrentChunkSize = (BytesRemaining > MAX_UDF_CHUNK) ? MAX_UDF_CHUNK : BytesRemaining;
            ChunkRead = 0;

            // Perform a recursive synchronous read for this specific chunk.
            // We pass PH_TMP_BUFFER so the recursive call uses our IoBuf directly.
            RC = UDFPhReadSynchronous(
                    IrpContext, 
                    DeviceObject, 
                    IoBuf, 
                    CurrentChunkSize, 
                    ROffset.QuadPart, 
                    &ChunkRead, 
                    Flags | PH_TMP_BUFFER);
            
            if (!NT_SUCCESS(RC)) {
                UDFPrint(("    UDF: Chunked read failed at Offset %I64x, RC=%x\n", ROffset.QuadPart, RC));
                break;
            }

            // Copy the data from the chunk buffer to the caller's main buffer
            RtlCopyMemory((PVOID)((PUCHAR)Buffer + TotalRead), IoBuf, ChunkRead);
            
            TotalRead += ChunkRead;
            BytesRemaining -= ChunkRead;
            ROffset.QuadPart += ChunkRead;

            if (ChunkRead < CurrentChunkSize) break; // Short read, stop here
        }
        
        *ReadBytes = TotalRead;
        if (IoBuf) DbgFreePool(IoBuf);
        return RC;
    }
    // --- CHUNKING LOGIC END ---

    // Original allocation/execution logic for standard or chunk-sized reads
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
        RC = STATUS_INSUFFICIENT_RESOURCES;
        goto try_exit;
    }

    // Create notification event object to be used to signal the request completion.
    KeInitializeEvent(&(Context->event), NotificationEvent, FALSE);

    if (TRUE || CurIrql > PASSIVE_LEVEL) {
        Irp = IoBuildAsynchronousFsdRequest(IRP_MJ_READ, DeviceObject, IoBuf,
                                               ByteCount, &ROffset, &(Context->IosbToUse) );
        if (!Irp) {
            UDFPrint(("    !irp Async\n"));
            RC = STATUS_INSUFFICIENT_RESOURCES;
            goto try_exit;
        }
        MmPrint(("    Alloc async Irp MDL=%x, ctx=%x\n", Irp->MdlAddress, Context));
        IoSetCompletionRoutine(Irp, &UDFAsyncCompletionRoutine,
                                Context, TRUE, TRUE, TRUE );
    } else {
        Irp = IoBuildSynchronousFsdRequest(IRP_MJ_READ, DeviceObject, IoBuf,
                                               ByteCount, &ROffset, &(Context->event), &(Context->IosbToUse) );
        if (!Irp) {
            UDFPrint(("    !irp Sync\n"));
            RC = STATUS_INSUFFICIENT_RESOURCES;
            goto try_exit;
        }
        MmPrint(("    Alloc Irp MDL=%x, ctx=%x\n", Irp->MdlAddress, Context));
    }

    IrpSp = IoGetNextIrpStackLocation(Irp);

    if (FlagOn(IrpContext->Flags, IRP_CONTEXT_FLAG_WRITE_THROUGH)) {
        SetFlag(IrpSp->Flags, SL_WRITE_THROUGH);
    }

    SetFlag(IrpSp->Flags, SL_OVERRIDE_VERIFY_VOLUME);

    RC = IoCallDriver(DeviceObject, Irp);

    if (RC == STATUS_PENDING) {
        DbgWaitForSingleObject(&(Context->event), NULL);
        RC = Context->IosbToUse.Status;
        if (RC == STATUS_DATA_OVERRUN) {
            RC = STATUS_SUCCESS;
        }
    }

    if (NT_SUCCESS(RC)) {
        (*ReadBytes) = (ULONG)Context->IosbToUse.Information;
    }

    if (!(Flags & PH_TMP_BUFFER)) {
        RtlCopyMemory(Buffer, IoBuf, *ReadBytes);
    }

try_exit:
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
    KIRQL               CurIrql = KeGetCurrentIrql();
    PVOID               IoBuf = NULL;
    
    // Chunking variables
    ULONG               BytesRemaining = ByteCount;
    ULONG               CurrentChunkSize = 0;
    SIZE_T              TotalWritten = 0;
    SIZE_T              ChunkWritten = 0;
    const ULONG         MAX_UDF_CHUNK = 1048576; // 1MB limit for NonPagedPool safety

#ifdef DBG
    if (UDF_SIMULATE_WRITES) {
        *WrittenBytes = ByteCount;
        return STATUS_SUCCESS;
    }
#endif //DBG

    ROffset.QuadPart = Offset;
    (*WrittenBytes) = 0;

    // --- CHUNKING LOGIC START ---
    // If the request is large and we aren't already using a temporary buffer,
    // split the request into smaller chunks to avoid NonPagedPool exhaustion.
    if (!(Flags & PH_TMP_BUFFER) && ByteCount > MAX_UDF_CHUNK) {
        
        UDFPrint(("    UDF: Chunking large write: %lu bytes at Offset %I64x\n", ByteCount, Offset));
        
        // Allocate one small reusable buffer for the chunks
        IoBuf = DbgAllocatePool(NonPagedPool, MAX_UDF_CHUNK);
        if (!IoBuf) {
            UDFPrint(("    !IoBuf (Write Chunk buffer allocation failed)\n"));
            return STATUS_INSUFFICIENT_RESOURCES;
        }

        while (BytesRemaining > 0) {
            CurrentChunkSize = (BytesRemaining > MAX_UDF_CHUNK) ? MAX_UDF_CHUNK : BytesRemaining;
            ChunkWritten = 0;

            // Copy data from the caller's main buffer into our small chunk buffer
            RtlCopyMemory(IoBuf, (PVOID)((PUCHAR)Buffer + TotalWritten), CurrentChunkSize);

            // Perform a recursive synchronous write for this specific chunk.
            // Passing PH_TMP_BUFFER ensures the recursive call uses our IoBuf directly.
            RC = UDFPhWriteSynchronous(
                    DeviceObject, 
                    IoBuf, 
                    CurrentChunkSize, 
                    ROffset.QuadPart, 
                    &ChunkWritten, 
                    Flags | PH_TMP_BUFFER);
            
            if (!NT_SUCCESS(RC)) {
                UDFPrint(("    UDF: Chunked write failed at Offset %I64x, RC=%x\n", ROffset.QuadPart, RC));
                break;
            }

            TotalWritten += ChunkWritten;
            BytesRemaining -= (ULONG)ChunkWritten;
            ROffset.QuadPart += ChunkWritten;

            if (ChunkWritten < CurrentChunkSize) break; // Short write, stop here
        }
        
        *WrittenBytes = TotalWritten;
        if (IoBuf) DbgFreePool(IoBuf);
        return RC;
    }
    // --- CHUNKING LOGIC END ---

    // Utilizing a temporary buffer to circumvent the situation where the IO buffer contains TransitionPage pages.
    if (Flags & PH_TMP_BUFFER) {
        IoBuf = Buffer;
    } else {
        IoBuf = DbgAllocatePool(NonPagedPool, ByteCount);
        if (!IoBuf) {
            RC = STATUS_INSUFFICIENT_RESOURCES;
            goto try_exit;
        }
        RtlCopyMemory(IoBuf, Buffer, ByteCount);
    }

    Context = (PUDF_PH_CALL_CONTEXT)MyAllocatePool__( NonPagedPool, sizeof(UDF_PH_CALL_CONTEXT) );
    if (!Context) {
        RC = STATUS_INSUFFICIENT_RESOURCES;
        goto try_exit;
    }

    // Create notification event object to be used to signal the request completion.
    KeInitializeEvent(&(Context->event), NotificationEvent, FALSE);

    if (TRUE || CurIrql > PASSIVE_LEVEL) {
        irp = IoBuildAsynchronousFsdRequest(IRP_MJ_WRITE, DeviceObject, IoBuf,
                                            ByteCount, &ROffset, &(Context->IosbToUse) );
        if (!irp) {
            RC = STATUS_INSUFFICIENT_RESOURCES;
            goto try_exit;
        }
        MmPrint(("    Alloc async Irp MDL=%x, ctx=%x\n", irp->MdlAddress, Context));
        IoSetCompletionRoutine( irp, &UDFAsyncCompletionRoutine,
                                Context, TRUE, TRUE, TRUE );
    } else {
        irp = IoBuildSynchronousFsdRequest(IRP_MJ_WRITE, DeviceObject, IoBuf,
                                           ByteCount, &ROffset, &(Context->event), &(Context->IosbToUse) );
        if (!irp) {
            RC = STATUS_INSUFFICIENT_RESOURCES;
            goto try_exit;
        }
        MmPrint(("    Alloc Irp MDL=%x\n, ctx=%x", irp->MdlAddress, Context));
    }

    (IoGetNextIrpStackLocation(irp))->Flags |= SL_OVERRIDE_VERIFY_VOLUME;
    RC = IoCallDriver(DeviceObject, irp);

    if (RC == STATUS_PENDING) {
        DbgWaitForSingleObject(&(Context->event), NULL);
        RC = Context->IosbToUse.Status;
        if (RC == STATUS_DATA_OVERRUN) {
            RC = STATUS_SUCCESS;
        }
    }

    if (NT_SUCCESS(RC)) {
        (*WrittenBytes) = Context->IosbToUse.Information;
    }

try_exit:
    if (Context) MyFreePool__(Context);
    if (IoBuf && !(Flags & PH_TMP_BUFFER)) DbgFreePool(IoBuf);
    
    if (!NT_SUCCESS(RC)) {
        UDFPrint(("WriteError: %08x\n", RC));
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

