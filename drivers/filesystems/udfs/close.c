////////////////////////////////////////////////////////////////////
// Copyright (C) Alexander Telyatnikov, Ivan Keliukh, Yegor Anchishkin, SKIF Software, 1999-2013. Kiev, Ukraine
// All rights reserved
// This file was released under the GPLv2 on June 2015.
////////////////////////////////////////////////////////////////////
/*************************************************************************
*
* File: Close.c
*
* Module: UDF File System Driver (Kernel mode execution only)
*
* Description:
*   Contains code to handle the "Close" dispatch entry point.
*
*************************************************************************/

#include            "udffs.h"

// define the file specific bug-check id
#define         UDF_BUG_CHECK_ID                UDF_FILE_CLOSE

typedef BOOLEAN      (*PCHECK_TREE_ITEM) (IN PUDF_FILE_INFO   FileInfo);
#define TREE_ITEM_LIST_GRAN 32

_Requires_lock_held_(_Global_critical_region_)
BOOLEAN
UDFCommonClosePrivate(
    _In_ PIRP_CONTEXT IrpContext,
    _In_ PVCB Vcb,
    _In_ PFCB Fcb,
    _In_ ULONG UserReference,
    _In_ BOOLEAN FromFsd
    );

VOID
UDFQueueClose(
    _In_ PIRP_CONTEXT IrpContext,
    _In_ PFCB Fcb,
    _In_ ULONG UserReference,
    _In_ BOOLEAN DelayedClose
    );

#define UDFCreateIrpContextLite(IC)  \
    ExAllocatePoolWithTag(NonPagedPool, sizeof(IRP_CONTEXT_LITE), TAG_IRP_CONTEXT_LITE)

#define UDFFreeIrpContextLite(ICL)  \
    {                               \
        PVOID Pool = (PVOID)ICL;    \
        UDFFreePool(&Pool);         \
    }

/*************************************************************************
*
* Function: UDFCommonClose()
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
* Return Value: must be STATUS_SUCCESS
*
*************************************************************************/
_Requires_lock_held_(_Global_critical_region_)
NTSTATUS
UDFCommonClose(
    PIRP_CONTEXT IrpContext,
    PIRP Irp
    )
{
    NTSTATUS                RC = STATUS_SUCCESS;
    PFILE_OBJECT            FileObject = IoGetCurrentIrpStackLocation(Irp)->FileObject;
    PFCB                    Fcb = NULL;
    PCCB                    Ccb = NULL;
    PVCB                    Vcb = NULL;
    TYPE_OF_OPEN            TypeOfOpen;
    ULONG UserReference = 0;
    BOOLEAN PotentialVcbTeardown = FALSE;

    PAGED_CODE();

    ASSERT_IRP_CONTEXT(IrpContext);
    ASSERT_IRP(Irp);

    // If we were called with our file system device object instead of a
    // volume device object, just complete this request with STATUS_SUCCESS.

    if (IrpContext->Vcb == NULL) {

        UDFCompleteRequest( IrpContext, Irp, STATUS_SUCCESS );
        return STATUS_SUCCESS;
    }

    // Decode the file object to get the type of open and Fcb/Ccb.

    TypeOfOpen = UDFDecodeFileObject(IoGetCurrentIrpStackLocation(Irp)->FileObject,
                                     &Fcb,
                                     &Ccb);

    // No work to do for unopened file objects.

    if (TypeOfOpen == UnopenedFileObject) {

        UDFCompleteRequest(IrpContext, Irp, STATUS_SUCCESS);

        return STATUS_SUCCESS;
    }

    Vcb = Fcb->Vcb;

    ASSERT_FCB(Fcb);
    ASSERT_CCB(Ccb);
    ASSERT_VCB(Vcb);

    if (Irp) {

        UserReference = 1;
        // we can release CCB in any case
        UDFDeleteCcb(Ccb);
        FileObject->FsContext2 = NULL;
    }

    _SEH2_TRY {

        // check if this is the last Close (no more Handles)
        // and try to Delay it....
        if ((Fcb->FcbState & UDF_FCB_DELAY_CLOSE) &&
            (Vcb->VcbCondition == VcbMounted) &&
            (Fcb->FcbState & UDF_FCB_DELETED) == 0 &&
            //(Fcb->FcbCondition == FcbGood) &&
            (Fcb->FcbReference == 1) &&
            ((TypeOfOpen == UserFileOpen) ||
             (TypeOfOpen == UserDirectoryOpen))) {

            UDFQueueClose(IrpContext, Fcb, UserReference, TRUE);

            IrpContext = NULL;

        // Otherwise try to process this close.  Post to the async close queue
        // if we can't acquire all of the resources.

        } else {

            // Decrement reference counts
            // These were incremented in UDFCompleteFcbOpen.

            UDFLockVcb(IrpContext, Vcb);

            Fcb->FcbReference--;
            Fcb->FcbUserReference--;
            Vcb->VcbReference--;
            Vcb->VcbUserReference--;

            if (Fcb == Vcb->VolumeDasdFcb) {

                AdPrint(("UDF: Closing volume\n"));
                AdPrint(("UDF: ReferenceCount:  %x\n",Fcb->FcbReference));

                ASSERT(Fcb == Fcb->Vcb->VolumeDasdFcb);
                UDFUnlockVcb(IrpContext, Vcb);

                if (Vcb->VcbCleanup > 0) {
                    try_return(RC = STATUS_SUCCESS);
                }

                if ((Vcb->VcbCleanup == 0) &&
                    (Vcb->VcbCondition != VcbMounted))  {

                    // Possible dismount.  Acquire CdData to synchronise with the remount path
                    // before looking at the vcb condition again.

                    UDFAcquireUdfData(IrpContext);

                    if ((Vcb->VcbCleanup == 0) &&
                        (Vcb->VcbCondition != VcbMounted) &&
                        (Vcb->VcbCondition != VcbMountInProgress) &&
                        FlagOn(IrpContext->Flags, IRP_CONTEXT_FLAG_TOP_LEVEL_UDFS))  {

                        PotentialVcbTeardown = TRUE;
                    }
                    else {

                        // We can't dismount this volume now,  there are other references or
                        // it's just been remounted.
                    }

                    //  Drop the global lock if we don't need it anymore.

                    if (!PotentialVcbTeardown) {

                        UDFReleaseUdfData(IrpContext);
                    }
                }

                try_return(RC = STATUS_SUCCESS);
            }

            // Release VcbMutex BEFORE TeardownStructures
            UDFUnlockVcb(IrpContext, Vcb);

            // try to clean up as long chain as it is possible
            // TODO: refactor to use UDFCommonClosePrivate
            {
                BOOLEAN RemovedFcb = FALSE;
                UDFAcquireFcbExclusive(IrpContext, Fcb, FALSE);
                // LCB-based teardown: walks ParentLcbQueue to find and remove LCBs
                UDFTeardownStructures(IrpContext, Fcb, FALSE, &RemovedFcb);
                if (!RemovedFcb) {
                    UDFReleaseFcb(IrpContext, Fcb);
                }
            }
        }

try_exit: NOTHING;

    } _SEH2_FINALLY {

    } _SEH2_END; // end of "__finally" processing

    // Always complete this request with STATUS_SUCCESS.

    UDFCompleteRequest(IrpContext, Irp, STATUS_SUCCESS);

    if (PotentialVcbTeardown) {

        UDFReleaseUdfData(IrpContext);
    }

    // Always return STATUS_SUCCESS for closes.

    return STATUS_SUCCESS;
} // end UDFCommonClose()

PIRP_CONTEXT
UDFRemoveClose(
    _In_opt_ PVCB Vcb
    )

/*++

Routine Description:

Arguments:

    This routine is called to scan the async and delayed close queues looking
    for a suitable entry.  If the Vcb is specified then we scan both queues
    looking for an entry with the same Vcb.  Otherwise we will look in the
    async queue first for any close item.  If none found there then we look
    in the delayed close queue provided that we have triggered the delayed
    close operation.

Return Value:

    PIRP_CONTEXT - NULL if no work item found.  Otherwise it is the pointer to
        either the IrpContext or IrpContextLite for this request.

--*/

{
    PIRP_CONTEXT IrpContext = NULL;
    PIRP_CONTEXT NextIrpContext;
    PIRP_CONTEXT_LITE NextIrpContextLite;

    PLIST_ENTRY Entry;

    PAGED_CODE();

    ASSERT_OPTIONAL_VCB(Vcb);

    // Lock the UdfData to perform the scan.

    UDFLockUdfData();

    //  First check the list of async closes.

    Entry = UdfData.AsyncCloseQueue.Flink;

    while (Entry != &UdfData.AsyncCloseQueue) {

        // Extract the IrpContext.

        NextIrpContext = CONTAINING_RECORD(Entry,
                                           IRP_CONTEXT,
                                           WorkQueueItem.List);

        // If no Vcb was specified or this Vcb is for our volume
        // then perform the close.

        if (!ARGUMENT_PRESENT(Vcb) || (NextIrpContext->Vcb == Vcb)) {

            RemoveEntryList(Entry);
            UdfData.AsyncCloseCount -= 1;

            IrpContext = NextIrpContext;
            break;
        }

        // Move to the next entry.

        Entry = Entry->Flink;
    }

    //  If we didn't find anything look through the delayed close
    //  queue.
    //
    //  We will only check the delayed close queue if we were given
    //  a Vcb or the delayed close operation is active.

    if ((IrpContext == NULL) &&
        (ARGUMENT_PRESENT( Vcb ) ||
        (UdfData.ReduceDelayedClose &&
        (UdfData.DelayedCloseCount > UdfData.MinDelayedCloseCount)))) {

        Entry = UdfData.DelayedCloseQueue.Flink;

        while (Entry != &UdfData.DelayedCloseQueue) {

            // Extract the IrpContext.

            NextIrpContextLite = CONTAINING_RECORD( Entry,
                                                    IRP_CONTEXT_LITE,
                                                    DelayedCloseLinks );

            //  If no Vcb was specified or this Vcb is for our volume
            //  then perform the close.

            if (!ARGUMENT_PRESENT(Vcb) || (NextIrpContextLite->Fcb->Vcb == Vcb)) {

                RemoveEntryList(Entry);
                UdfData.DelayedCloseCount -= 1;

                IrpContext = (PIRP_CONTEXT)NextIrpContextLite;
                break;
            }

            //
            //  Move to the next entry.
            //

            Entry = Entry->Flink;
        }
    }

    // If the Vcb wasn't specified and we couldn't find an entry
    // then turn off the Fsp thread.

    if (!ARGUMENT_PRESENT( Vcb ) && (IrpContext == NULL)) {

        UdfData.FspCloseActive = FALSE;
        UdfData.ReduceDelayedClose = FALSE;
    }

    // Unlock the UdfData.

    UDFUnlockUdfData();

    return IrpContext;
}

VOID
UDFInitializeStackIrpContext(
    _Out_ PIRP_CONTEXT IrpContext,
    _In_ PIRP_CONTEXT_LITE IrpContextLite
    )

/*++

Routine Description:

    This routine is called to initialize an IrpContext for the current
    CDFS request.  The IrpContext is on the stack and we need to initialize
    it for the current request.  The request is a close operation.

Arguments:

    IrpContext - IrpContext to initialize.

    IrpContextLite - Structure containing the details of this request.

Return Value:

    None

--*/

{
    PAGED_CODE();

    // Zero and then initialize the structure.

    RtlZeroMemory( IrpContext, sizeof( IRP_CONTEXT ));

    // Set the proper node type code and node byte size

    IrpContext->NodeIdentifier.NodeTypeCode = UDF_NODE_TYPE_IRP_CONTEXT;
    IrpContext->NodeIdentifier.NodeByteSize = sizeof(IRP_CONTEXT);

    // Note that this is from the stack.

    SetFlag( IrpContext->Flags, IRP_CONTEXT_FLAG_ON_STACK );

    // Copy RealDevice for workque algorithms.

    IrpContext->RealDevice = IrpContextLite->RealDevice;

    // The Vcb is found in the Fcb.

    IrpContext->Vcb = IrpContextLite->Fcb->Vcb;

    // Major/Minor Function codes

    IrpContext->MajorFunction = IRP_MJ_CLOSE;

    // Set the wait parameter

    SetFlag(IrpContext->Flags, IRP_CONTEXT_FLAG_WAIT);

    return;
}

_Requires_lock_held_(_Global_critical_region_)
BOOLEAN
UDFCommonClosePrivate(
    _In_ PIRP_CONTEXT IrpContext,
    _In_ PVCB Vcb,
    _In_ PFCB Fcb,
    _In_ ULONG UserReference,
    _In_ BOOLEAN FromFsd
)

/*++

Routine Description:

    This is the worker routine for the close operation.  We can be called in
    an Fsd thread or from a worker Fsp thread.  If called from the Fsd thread
    then we acquire the resources without waiting.  Otherwise we know it is
    safe to wait.

    We check to see whether we should post this request to the delayed close
    queue.  If we are to process the close here then we acquire the Vcb and
    Fcb.  We will adjust the counts and call our teardown routine to see
    if any of the structures should go away.

Arguments:

    Vcb - Vcb for this volume.

    Fcb - Fcb for this request.

    UserReference - Number of user references for this file object.  This is
        zero for an internal stream.

    FromFsd - This request was called from an Fsd thread.  Indicates whether
        we should wait to acquire resources.

    DelayedClose - Address to store whether we should try to put this on
        the delayed close queue.  Ignored if this routine can process this
        close.

Return Value:

    BOOLEAN - TRUE if this thread processed the close, FALSE otherwise.

--*/

{
    BOOLEAN RemovedFcb;

    PAGED_CODE();

    ASSERT_IRP_CONTEXT(IrpContext);
    ASSERT_FCB(Fcb);

    //  Try to acquire the Vcb and Fcb.  If we can't acquire them then return
    // and let our caller know he should post the request to the async
    // queue.

    if (UDFAcquireVcbShared(IrpContext, Vcb, FromFsd)) {

        if (!UDFAcquireFcbExclusive(IrpContext, Fcb, FromFsd)) {

            // We couldn't get the Fcb.  Release the Vcb and let our caller
            // know to post this request.

            UDFReleaseVcb(IrpContext, Vcb);
            return FALSE;
        }

    // We didn't get the Vcb.  Let our caller know to post this request.

    } else {

        return FALSE;
    }

    // Lock the Vcb and decrement the reference counts.

    UDFLockVcb(IrpContext, Vcb);
    UDFDecrementReferenceCounts(IrpContext, Fcb, 1, UserReference);
    UDFUnlockVcb(IrpContext, Vcb);

    //  Call our teardown routine to see if this object can go away.
    //  If we don't remove the Fcb then release it.
    // LCB-based teardown: walks ParentLcbQueue to find and remove LCBs

    UDFTeardownStructures(IrpContext, Fcb, FALSE, &RemovedFcb);

    if (!RemovedFcb) {

        UDFReleaseFcb(IrpContext, Fcb);
    }
    else {
        _Analysis_assume_lock_not_held_(Fcb->FcbNonpaged->FcbResource);
    }

    //  Release the Vcb and return to our caller.  Let him know we completed
    //  this request.

    UDFReleaseVcb(IrpContext, Vcb);

    return TRUE;
}


VOID
NTAPI
UDFFspClose(
    _In_opt_ PVCB Vcb
    )

/*++

Routine Description:

    This routine is called to process the close queues in the UdfData.  If the
    Vcb is passed then we want to remove all of the closes for this Vcb.
    Otherwise we will do as many of the delayed closes as we need to do.

Arguments:

    Vcb - If specified then we are looking for all of the closes for the
        given Vcb.

Return Value:

    None

--*/

{
    PIRP_CONTEXT IrpContext;
    IRP_CONTEXT StackIrpContext = {0};

    THREAD_CONTEXT ThreadContext = {0};

    PFCB Fcb;
    ULONG UserReference;

    ULONG VcbHoldCount = 0;
    PVCB CurrentVcb = NULL;

    BOOLEAN PotentialVcbTeardown = FALSE;

    PAGED_CODE();

    ASSERT_OPTIONAL_VCB(Vcb);

    FsRtlEnterFileSystem();

    // Continue processing until there are no more closes to process.

    while ((IrpContext = UDFRemoveClose(Vcb)) != NULL) {

        // If we don't have an IrpContext then use the one on the stack.
        // Initialize it for this request.

        if (SafeNodeType(IrpContext) != UDF_NODE_TYPE_IRP_CONTEXT) {

            // Update the local values from the IrpContextLite.

            Fcb = ((PIRP_CONTEXT_LITE)IrpContext)->Fcb;
            UserReference = ((PIRP_CONTEXT_LITE)IrpContext)->UserReference;

            // Update the stack irp context with the values from the
            // IrpContextLite.

            UDFInitializeStackIrpContext(&StackIrpContext,
                                         (PIRP_CONTEXT_LITE)IrpContext);

            // Free the IrpContextLite.

            UDFFreeIrpContextLite((PIRP_CONTEXT_LITE)IrpContext);

            //  Remember we have the IrpContext from the stack.

            IrpContext = &StackIrpContext;

        //  Otherwise cleanup the existing IrpContext.

        } else {

            //  Remember the Fcb and user reference count.

            Fcb = (PFCB) IrpContext->Irp;
            IrpContext->Irp = NULL;

            UserReference = (ULONG) IrpContext->ExceptionStatus;
            IrpContext->ExceptionStatus = STATUS_SUCCESS;
        }

        _Analysis_assume_(Fcb != NULL && Fcb->Vcb != NULL);

        // We have an IrpContext.  Now we need to set the top level thread
        // context.

        SetFlag(IrpContext->Flags, IRP_CONTEXT_FSP_FLAGS);

        //  If we were given a Vcb then there is a request on top of this.

        if (ARGUMENT_PRESENT(Vcb)) {

            ClearFlag(IrpContext->Flags,
                      IRP_CONTEXT_FLAG_TOP_LEVEL | IRP_CONTEXT_FLAG_TOP_LEVEL_UDFS);
        }

        UDFSetThreadContext(IrpContext, &ThreadContext);

        //  If we have hit the maximum number of requests to process without
        //  releasing the Vcb then release the Vcb now.  If we are holding
        //  a different Vcb to this one then release the previous Vcb.
        //
        //  In either case acquire the current Vcb.
        //
        //  We use the MinDelayedCloseCount from the CdData since it is
        //  a convenient value based on the system size.  Only thing we are trying
        //  to do here is prevent this routine starving other threads which
        //  may need this Vcb exclusively.
        //
        //  Note that the check for potential teardown below is unsafe.  We'll 
        //  repeat later within the cddata lock.

        PotentialVcbTeardown = !ARGUMENT_PRESENT( Vcb ) &&
                               (Fcb->Vcb->VcbCondition != VcbMounted) &&
                               (Fcb->Vcb->VcbCondition != VcbMountInProgress) &&
                               (Fcb->Vcb->VcbCleanup == 0);

        if (PotentialVcbTeardown ||
            (VcbHoldCount > UdfData.MinDelayedCloseCount) ||
            (Fcb->Vcb != CurrentVcb)) {

            if (CurrentVcb != NULL) {

                UDFReleaseVcb(IrpContext, CurrentVcb);
            }

            if (PotentialVcbTeardown) {

                UDFAcquireUdfData(IrpContext);

                //  Repeat the checks with global lock held.  The volume could have
                //  been remounted while we didn't hold the lock.

                PotentialVcbTeardown = !ARGUMENT_PRESENT( Vcb ) &&
                                       (Fcb->Vcb->VcbCondition != VcbMounted) &&
                                       (Fcb->Vcb->VcbCondition != VcbMountInProgress) &&
                                       (Fcb->Vcb->VcbCleanup == 0);
                                
                if (!PotentialVcbTeardown)  {

                    UDFReleaseUdfData(IrpContext);
                }
            }

            CurrentVcb = Fcb->Vcb;

            _Analysis_assume_(CurrentVcb != NULL);
            
            UDFAcquireVcbShared(IrpContext, CurrentVcb, FALSE);

            VcbHoldCount = 0;

        } else {

            VcbHoldCount += 1;
        }

        // Call our worker routine to perform the close operation.

        UDFCommonClosePrivate(IrpContext, CurrentVcb, Fcb, UserReference, FALSE);

        //  If the reference count on this Vcb is below our residual reference
        //  then check if we should dismount the volume.

        if (PotentialVcbTeardown) {

            UDFReleaseVcb( IrpContext, CurrentVcb );
            UDFCheckForDismount(IrpContext, CurrentVcb, FALSE);

            CurrentVcb = NULL;

            UDFReleaseUdfData(IrpContext);
            PotentialVcbTeardown = FALSE;
        }

        //  Complete the current request to cleanup the IrpContext.

        UDFCompleteRequest(IrpContext, NULL, STATUS_SUCCESS);
    }

    //  Release any Vcb we may still hold.

    if (CurrentVcb != NULL) {

        UDFReleaseVcb(IrpContext, CurrentVcb);

    }

#pragma prefast(suppress:26165, "Esp:1153")
    FsRtlExitFileSystem();
}


/*
    This routine adds request to Delayed Close queue.
    If number of queued requests exceeds higher threshold it fires
    UDFDelayedClose()
 */
VOID
UDFQueueClose(
    _In_ PIRP_CONTEXT IrpContext,
    _In_ PFCB Fcb,
    _In_ ULONG UserReference,
    _In_ BOOLEAN DelayedClose
    )
{
    PIRP_CONTEXT_LITE IrpContextLite = NULL;
    BOOLEAN StartWorker = FALSE;

    PAGED_CODE();

    ASSERT_IRP_CONTEXT(IrpContext);
    ASSERT_FCB(Fcb);

    // Start with the delayed queue request.  We can move this to the async
    // queue if there is an allocation failure.

    if (DelayedClose) {

        // Try to allocate non-paged pool for the IRP_CONTEXT_LITE.

        IrpContextLite = (PIRP_CONTEXT_LITE)UDFCreateIrpContextLite(IrpContext);
    }

    // We want to clear the top level context in this thread if
    // necessary.  Call our cleanup routine to do the work.

    SetFlag(IrpContext->Flags, IRP_CONTEXT_FLAG_MORE_PROCESSING);
    UDFCleanupIrpContext(IrpContext, TRUE);

    // Synchronize with the UdfData lock.

    UDFLockUdfData();

    // If we have an IrpContext then put the request on the delayed close queue.

    if (IrpContextLite != NULL) {

        // Initialize the IrpContextLite.

        IrpContextLite->NodeIdentifier.NodeTypeCode = UDF_NODE_TYPE_IRP_CONTEXT_LITE;
        IrpContextLite->NodeIdentifier.NodeByteSize = sizeof(IRP_CONTEXT_LITE);
        IrpContextLite->Fcb = Fcb;
        IrpContextLite->UserReference = UserReference;
        IrpContextLite->RealDevice = IrpContext->RealDevice;

        // Add this to the delayed close list and increment
        // the count.

        InsertTailList(&UdfData.DelayedCloseQueue,
                       &IrpContextLite->DelayedCloseLinks);

        UdfData.DelayedCloseCount += 1;

        // If we are above our threshold then start the delayed
        // close operation.

        if (UdfData.DelayedCloseCount > UdfData.MaxDelayedCloseCount) {

            UdfData.ReduceDelayedClose = TRUE;

            if (!UdfData.FspCloseActive) {

                UdfData.FspCloseActive = TRUE;
                StartWorker = TRUE;
            }
        }

        // Unlock the UdfData.

        UDFUnlockUdfData();

        // Cleanup the IrpContext.

        UDFCompleteRequest(IrpContext, NULL, STATUS_SUCCESS);

    // Otherwise drop into the async case below.

    } else {

        // Store the information about the file object into the IrpContext.

        IrpContext->Irp = (PIRP)Fcb;
        IrpContext->ExceptionStatus = (NTSTATUS)UserReference;

        // Add this to the async close list and increment the count.

        InsertTailList(&UdfData.AsyncCloseQueue,
                       &IrpContext->WorkQueueItem.List);

        UdfData.AsyncCloseCount += 1;

        // Remember to start the Fsp close thread if not currently started.

        if (!UdfData.FspCloseActive) {

            UdfData.FspCloseActive = TRUE;

            StartWorker = TRUE;
        }

        //  Unlock the CdData.

        UDFUnlockUdfData();

    }

    // Start the FspClose thread if we need to.

    if (StartWorker) {

        ExQueueWorkItem(&UdfData.CloseItem, CriticalWorkQueue);
    }

    return;
} // end UDFQueueDelayedClose()

