////////////////////////////////////////////////////////////////////
// Copyright (C) Alexander Telyatnikov, Ivan Keliukh, Yegor Anchishkin, SKIF Software, 1999-2013. Kiev, Ukraine
// All rights reserved
// This file was released under the GPLv2 on June 2015.
////////////////////////////////////////////////////////////////////
/*

 File: PrefxSup.cpp

 Module: UDF File System Driver (Kernel mode execution only)

 Description:
   This module implements the UDF Prefix support routines.
   Based on Microsoft UDF driver pattern.

*/

#include "udffs.h"

// define the file specific bug-check id
#define UDF_BUG_CHECK_ID                UDF_FILE_MISC

/*
    This routine creates and inserts an LCB linking the parent FCB to child FCB.

    Similar to MS UdfInsertPrefix but simplified - we don't use splay trees
    for prefix lookup since we use FileInfo-based navigation.

    The LCB is linked into:
    - ParentFcb->ChildLcbQueue (via Lcb->ParentFcbLinks)
    - ChildFcb->ParentLcbQueue (via Lcb->ChildFcbLinks)
*/
PLCB
UDFInsertPrefix(
    IN PIRP_CONTEXT IrpContext,
    IN PFCB ParentFcb,
    IN PFCB ChildFcb,
    IN ULONG Index
    )
{
    PLCB Lcb;
    ULONG Flags = 0;

    UNREFERENCED_PARAMETER(IrpContext);

    ASSERT(ParentFcb);
    ASSERT(ChildFcb);

    //
    // Allocate LCB - use lookaside for small allocations
    //
    if (sizeof(LCB) > SIZEOF_LOOKASIDE_LCB) {

        Lcb = (PLCB)FsRtlAllocatePoolWithTag(PagedPool,
                                              sizeof(LCB),
                                              TAG_LCB);
        if (!Lcb) {
            return NULL;
        }

        SetFlag(Flags, UDF_LCB_FLAG_POOL_ALLOCATED);

    } else {

        Lcb = (PLCB)ExAllocateFromPagedLookasideList(&UdfData.LcbLookasideList);
        if (!Lcb) {
            return NULL;
        }
    }

    //
    // Initialize LCB
    //
    RtlZeroMemory(Lcb, sizeof(LCB));

    Lcb->NodeIdentifier.NodeTypeCode = UDF_NODE_TYPE_LCB;
    Lcb->NodeIdentifier.NodeByteSize = sizeof(LCB);

    // Set up FCB pointers
    Lcb->ParentFcb = ParentFcb;
    Lcb->ChildFcb = ChildFcb;
    Lcb->Index = Index;

    // Set flags
    Lcb->Flags = Flags;

    // Initial reference count
    Lcb->Reference = 0;

    //
    // Link LCB into the FCB queues
    //
    // Insert into parent's child queue
    InsertHeadList(&ParentFcb->ChildLcbQueue, &Lcb->ParentFcbLinks);
    // Insert into child's parent queue
    InsertHeadList(&ChildFcb->ParentLcbQueue, &Lcb->ChildFcbLinks);

    return Lcb;
} // end UDFInsertPrefix()


/*
    This routine removes an LCB from FCB queues and frees it.
*/
VOID
UDFRemovePrefix(
    IN PIRP_CONTEXT IrpContext,
    IN PLCB Lcb
    )
{
    UNREFERENCED_PARAMETER(IrpContext);

    if (!Lcb) {
        return;
    }

    ASSERT(Lcb->NodeIdentifier.NodeTypeCode == UDF_NODE_TYPE_LCB);

    //
    // Remove from parent FCB's ChildLcbQueue
    //
    RemoveEntryList(&Lcb->ParentFcbLinks);
    InitializeListHead(&Lcb->ParentFcbLinks);

    //
    // Remove from child FCB's ParentLcbQueue
    //
    RemoveEntryList(&Lcb->ChildFcbLinks);
    InitializeListHead(&Lcb->ChildFcbLinks);

    //
    // Free the LCB based on how it was allocated
    //
    if (FlagOn(Lcb->Flags, UDF_LCB_FLAG_POOL_ALLOCATED)) {

        ExFreePool(Lcb);

    } else {

        ExFreeToPagedLookasideList(&UdfData.LcbLookasideList, Lcb);
    }
} // end UDFRemovePrefix()


/*
    This routine finds an existing LCB linking the parent FCB to child FCB.
    Returns NULL if no such LCB exists.
*/
PLCB
UDFFindPrefix(
    IN PIRP_CONTEXT IrpContext,
    IN PFCB ParentFcb,
    IN PFCB ChildFcb
    )
{
    PLIST_ENTRY ListEntry;
    PLCB Lcb;

    UNREFERENCED_PARAMETER(IrpContext);

    ASSERT(ParentFcb);
    ASSERT(ChildFcb);

    //
    // Walk the child FCB's ParentLcbQueue to find LCB pointing to this parent
    //
    for (ListEntry = ChildFcb->ParentLcbQueue.Flink;
         ListEntry != &ChildFcb->ParentLcbQueue;
         ListEntry = ListEntry->Flink) {

        Lcb = CONTAINING_RECORD(ListEntry, LCB, ChildFcbLinks);

        ASSERT(Lcb->NodeIdentifier.NodeTypeCode == UDF_NODE_TYPE_LCB);

        if (Lcb->ParentFcb == ParentFcb) {
            return Lcb;
        }
    }

    return NULL;
} // end UDFFindPrefix()


/*
    This routine acquires or creates an LCB linking the parent FCB to child FCB.
    If LCB already exists, increments its Reference count.
    If new LCB is created:
    - Increments parent FCB's FcbReference
    - Increments parent's FileInfo->RefCount (if available)

    This is the primary function to call when establishing a parent-child
    relationship during file open.

    Returns the LCB (existing or newly created), or NULL on allocation failure.
*/
PLCB
UDFAcquirePrefix(
    IN PIRP_CONTEXT IrpContext,
    IN PFCB ParentFcb,
    IN PFCB ChildFcb,
    IN ULONG Index
    )
{
    PLCB Lcb;

    UNREFERENCED_PARAMETER(IrpContext);

    ASSERT(ParentFcb);
    ASSERT(ChildFcb);

    //
    // First try to find existing LCB
    //
    Lcb = UDFFindPrefix(IrpContext, ParentFcb, ChildFcb);

    if (Lcb) {
        //
        // Found existing LCB - just increment reference
        //
        InterlockedIncrement((PLONG)&Lcb->Reference);
        return Lcb;
    }

    //
    // Need to create new LCB
    //
    Lcb = UDFInsertPrefix(IrpContext, ParentFcb, ChildFcb, Index);

    if (Lcb) {
        //
        // New LCB created - set initial reference and increment parent's references
        // The parent's FcbReference represents this LCB's existence
        //
        Lcb->Reference = 1;
        InterlockedIncrement((PLONG)&ParentFcb->FcbReference);

        //
        // Also reference parent's FileInfo if available
        // This mirrors how TreeLength used to work - parent is referenced
        // for each child link. When LCB is removed, we'll call UDFCloseFile__.
        //
        if (ParentFcb->FileInfo) {
            UDFReferenceFile__(ParentFcb->FileInfo);
        }
    }

    return Lcb;
} // end UDFAcquirePrefix()


/*
    This routine releases a reference to an LCB.
    If Reference goes to zero, the LCB is eligible for removal during teardown.

    Note: This does NOT remove the LCB or decrement parent's FcbReference.
    That is done during teardown when walking the ParentLcbQueue.
*/
VOID
UDFReleasePrefix(
    IN PIRP_CONTEXT IrpContext,
    IN PLCB Lcb
    )
{
    UNREFERENCED_PARAMETER(IrpContext);

    if (!Lcb) {
        return;
    }

    ASSERT(Lcb->NodeIdentifier.NodeTypeCode == UDF_NODE_TYPE_LCB);
    ASSERT(Lcb->Reference > 0);

    InterlockedDecrement((PLONG)&Lcb->Reference);
} // end UDFReleasePrefix()


/*
    This routine releases a reference to an LCB and, if Reference becomes 0,
    immediately removes the LCB and decrements parent references.

    This is used during rename operations where we need immediate cleanup
    of the old parent link rather than waiting for teardown.

    Parameters:
        IrpContext - The IRP context
        Lcb - The LCB to release
        CloseParentFileInfo - If TRUE and LCB is removed, call UDFCloseFile__
                              on parent's FileInfo

    Returns:
        TRUE if the LCB was removed, FALSE if it still has references
*/
BOOLEAN
UDFReleasePrefixImmediate(
    IN PIRP_CONTEXT IrpContext,
    IN PLCB Lcb,
    IN BOOLEAN CloseParentFileInfo
    )
{
    PFCB ParentFcb;
    PUDF_FILE_INFO ParentFileInfo;
    PVCB Vcb;
    LONG NewRef;

    if (!Lcb) {
        return FALSE;
    }

    ASSERT(Lcb->NodeIdentifier.NodeTypeCode == UDF_NODE_TYPE_LCB);
    ASSERT(Lcb->Reference > 0);

    //
    // Decrement reference
    //
    NewRef = InterlockedDecrement((PLONG)&Lcb->Reference);

    if (NewRef > 0) {
        //
        // Still has references, leave it in place
        //
        return FALSE;
    }

    //
    // Reference is now 0 - remove LCB and decrement parent refs
    //
    ParentFcb = Lcb->ParentFcb;
    ParentFileInfo = ParentFcb ? ParentFcb->FileInfo : NULL;
    Vcb = ParentFcb ? ParentFcb->Vcb : NULL;

    //
    // Remove LCB from queues and free it
    //
    UDFRemovePrefix(IrpContext, Lcb);

    //
    // Decrement parent's FcbReference
    //
    if (ParentFcb) {
        ASSERT(ParentFcb->FcbReference > 0);
        InterlockedDecrement((PLONG)&ParentFcb->FcbReference);

        //
        // Close parent's FileInfo if requested
        //
        if (CloseParentFileInfo && ParentFileInfo && Vcb) {
            UDFCloseFile__(IrpContext, Vcb, ParentFileInfo);
        }
    }

    return TRUE;
} // end UDFReleasePrefixImmediate()
