#include "udffs.h"

//  The Bug check file id for this module

#define UDF_BUG_CHECK_ID                   (UDFS_BUG_CHECK_STRUCSUP)

typedef struct _FCB_TABLE_ELEMENT {

    FILE_ID FileId;
    PFCB Fcb;

} FCB_TABLE_ELEMENT, *PFCB_TABLE_ELEMENT;

#define UDFInsertFcbTable(IC,F) {                                    \
     ASSERT_LOCKED_VCB( (F)->Vcb );                                  \
     FCB_TABLE_ELEMENT _Key;                                         \
     _Key.Fcb = (F);                                                 \
     _Key.FileId = (F)->FileId;                                      \
     RtlInsertElementGenericTable( &(F)->Vcb->FcbTable,              \
                                   &_Key,                            \
                                   sizeof( FCB_TABLE_ELEMENT ),      \
                                   NULL );                           \
}

#define UDFDeleteFcbTable(IC,F) {                                    \
     ASSERT_LOCKED_VCB( (F)->Vcb );                                  \
     FCB_TABLE_ELEMENT _Key;                                         \
     _Key.FileId = (F)->FileId;                                      \
     RtlDeleteElementGenericTable( &(F)->Vcb->FcbTable, &_Key );     \
}

inline
PFCB_NONPAGED
UDFAllocateFcbNonpaged(
)
{
    return (PFCB_NONPAGED)ExAllocateFromNPagedLookasideList(&UdfData.UDFNonPagedFcbLookasideList);
}

inline
PFCB
UDFAllocateFcbIndex(
)
{
    return (PFCB)ExAllocateFromPagedLookasideList(&UdfData.UDFFcbIndexLookasideList);
}

inline
PFCB
UDFAllocateFcbData(
)
{
    return (PFCB)ExAllocateFromPagedLookasideList(&UdfData.UDFFcbDataLookasideList);
}

inline
PFCB
UDFAllocateFcb(
)
{
    return (PFCB)ExAllocatePoolWithTag(NonPagedPool, sizeof(FCB), TAG_FCB);
}

inline
VOID
UDFDeallocateFcbNonpaged(
    PFCB_NONPAGED FcbNonpaged
    )
{
    ExFreeToNPagedLookasideList(&UdfData.UDFNonPagedFcbLookasideList, FcbNonpaged);
}

inline
VOID
UDFDeallocateFcbIndex(
    PFCB Fcb
    )
{
    ExFreeToPagedLookasideList(&UdfData.UDFFcbIndexLookasideList, Fcb);
}

inline
VOID
UDFDeallocateFcbData(
    PFCB Fcb
    )
{
    ExFreeToPagedLookasideList(&UdfData.UDFFcbDataLookasideList, Fcb);
}

PFCB_NONPAGED
UDFCreateFcbNonpaged(
    _In_ PIRP_CONTEXT IrpContext
    )

/*++

Routine Description:

    This routine is called to create and initialize the non-paged portion
    of an Fcb.

Arguments:

Return Value:

    PFCB_NONPAGED - Pointer to the created nonpaged Fcb.  NULL if not created.

--*/

{
    PFCB_NONPAGED FcbNonpaged;

    PAGED_CODE();
    
    UNREFERENCED_PARAMETER(IrpContext);
    
    //  Allocate the non-paged pool and initialize the various
    //  synchronization objects.

    FcbNonpaged = UDFAllocateFcbNonpaged();

    RtlZeroMemory(FcbNonpaged, sizeof(FCB_NONPAGED));

    FcbNonpaged->NodeTypeCode = UDF_NODE_TYPE_FCB_NONPAGED;
    FcbNonpaged->NodeByteSize = sizeof(FCB_NONPAGED);

    ExInitializeResourceLite(&FcbNonpaged->FcbPagingIoResource);
    ExInitializeResourceLite(&FcbNonpaged->FcbResource);
    ExInitializeFastMutex(&FcbNonpaged->FcbMutex);
    ExInitializeFastMutex(&FcbNonpaged->AdvancedFcbHeaderMutex);

    ExInitializeResourceLite(&FcbNonpaged->CcbListResource);

    return FcbNonpaged;
}

VOID
UDFDeleteFcbNonpaged (
    _In_ PIRP_CONTEXT IrpContext,
    _In_ PFCB_NONPAGED FcbNonpaged
    )

/*++

Routine Description:

    This routine is called to cleanup the non-paged portion of an Fcb.

Arguments:

    FcbNonpaged - Structure to clean up.

Return Value:

    None

--*/

{
    PAGED_CODE();
    
    UNREFERENCED_PARAMETER(IrpContext);
    
    ExDeleteResourceLite(&FcbNonpaged->FcbResource);
    ExDeleteResourceLite(&FcbNonpaged->FcbPagingIoResource);
    ExDeleteResourceLite(&FcbNonpaged->CcbListResource);

    UDFDeallocateFcbNonpaged(FcbNonpaged);

    return;
}

VOID
UDFDeleteFcb(
    _In_ PIRP_CONTEXT IrpContext,
    _In_ PFCB Fcb
    )

/*++

Routine Description:

    This routine is called to cleanup and deallocate an Fcb.  We know there
    are no references remaining.  We cleanup any auxilary structures and
    deallocate this Fcb.

Arguments:

    Fcb - This is the Fcb to deallcoate.

Return Value:

    None

--*/

{
    PVCB Vcb = NULL;
    PAGED_CODE();

    //  Sanity check the counts.

    NT_ASSERT( Fcb->FcbCleanup == 0 );
    NT_ASSERT( Fcb->FcbReference == 0 );

    //  Release any Filter Context structures associated with this FCB

   // FsRtlTeardownPerStreamContexts(&Fcb->Header);

    //  Start with the common structures.

   // CdUninitializeMcb( IrpContext, Fcb );

  //  CdDeleteFcbNonpaged( IrpContext, Fcb->FcbNonpaged );

    //
    //  Check if we need to deallocate the prefix name buffer.
    //

  //  if ((Fcb->FileNamePrefix.ExactCaseName.FileName.Buffer != (PWCHAR) Fcb->FileNamePrefix.FileNameBuffer) &&
  //      (Fcb->FileNamePrefix.ExactCaseName.FileName.Buffer != NULL)) {

 //       CdFreePool( &Fcb->FileNamePrefix.ExactCaseName.FileName.Buffer );
  //  }

    //
    //  Now look at the short name prefix.
    //

 //   if (Fcb->ShortNamePrefix != NULL) {

 //       CdFreePool( &Fcb->ShortNamePrefix );
 //   }

    //
    //  Now do the type specific structures.
    //

    switch (Fcb->Header.NodeTypeCode) {

    case UDF_NODE_TYPE_INDEX:

    //    NT_ASSERT( Fcb->FileObject == NULL );
    //    NT_ASSERT( IsListEmpty( &Fcb->FcbQueue ));

    //    if (Fcb == Fcb->Vcb->RootIndexFcb) {

    //        Vcb = Fcb->Vcb;
    //        Vcb->RootIndexFcb = NULL;

    //    } else if (Fcb == Fcb->Vcb->PathTableFcb) {

    //        Vcb = Fcb->Vcb;
    //        Vcb->PathTableFcb = NULL;
    //    }

        UDFDeallocateFcbIndex(Fcb);
        break;

    case UDF_NODE_TYPE_DATA:

    //    if (Fcb->FileLock != NULL) {

    //        FsRtlFreeFileLock( Fcb->FileLock );
    //    }

    //    FsRtlUninitializeOplock( CdGetFcbOplock(Fcb) );

          if (Fcb == Fcb->Vcb->VolumeDasdFcb) {

              __debugbreak();

              Vcb = Fcb->Vcb;
              Vcb->VolumeDasdFcb = NULL;
          }

        UDFDeallocateFcbData(Fcb);
    }

    //
    //  Decrement the Vcb reference count if this is a system
    //  Fcb.
    //

    if (Vcb != NULL) {

     //   InterlockedDecrement( (LONG*)&Vcb->VcbReference );
     //   InterlockedDecrement( (LONG*)&Vcb->VcbUserReference );
    }

    return;
}

PFCB
UDFLookupFcbTable (
    _In_ PIRP_CONTEXT IrpContext,
    _In_ PVCB Vcb,
    _In_ FILE_ID FileId
    )

/*++

Routine Description:

    This routine will look through the Fcb table looking for a matching
    entry.

Arguments:

    Vcb - Vcb for this volume.

    FileId - This is the key value to use for the search.

Return Value:

    PFCB - A pointer to the matching entry or NULL otherwise.

--*/

{
    FCB_TABLE_ELEMENT Key;
    PFCB_TABLE_ELEMENT Hit;
    PFCB ReturnFcb = NULL;

    PAGED_CODE();

    Key.FileId = FileId;

    Hit = (PFCB_TABLE_ELEMENT)RtlLookupElementGenericTable(&Vcb->FcbTable, &Key);

    if (Hit != NULL) {

        ReturnFcb = Hit->Fcb;
    }

    return ReturnFcb;

    UNREFERENCED_PARAMETER( IrpContext );
}

PFCB
CdGetNextFcb (
    _In_ PIRP_CONTEXT IrpContext,
    _In_ PVCB Vcb,
    _In_ PVOID *RestartKey
    )

/*++

Routine Description:

    This routine will enumerate through all of the Fcb's in the Fcb table.

Arguments:

    Vcb - Vcb for this volume.

    RestartKey - This value is used by the table package to maintain
        its position in the enumeration.  It is initialized to NULL
        for the first search.

Return Value:

    PFCB - A pointer to the next fcb or NULL if the enumeration is
        completed

--*/

{
    PFCB Fcb;

    PAGED_CODE();

    UNREFERENCED_PARAMETER( IrpContext );
    
    Fcb = (PFCB) RtlEnumerateGenericTableWithoutSplaying( &Vcb->FcbTable, RestartKey );

    if (Fcb != NULL) {

        Fcb = ((PFCB_TABLE_ELEMENT)(Fcb))->Fcb;
    }

    return Fcb;
}

/*************************************************************************
*
* Function: UDFCreateFcb()
*
* Description:
*   Allocate a new FCB structure to represent an open on-disk object.
*   Also initialize the FCB structure to NULL.
*
* Expected Interrupt Level (for execution) :
*
*  IRQL_PASSIVE_LEVEL
*
* Return Value: A pointer to the FCB structure OR NULL.
*
*************************************************************************/

PFCB
UDFCreateFcb(
    _In_ PIRP_CONTEXT IrpContext,
    _In_ FILE_ID FileId,
    _In_ NODE_TYPE_CODE NodeTypeCode,
    _Out_opt_ PBOOLEAN FcbExisted
    )

/*++

Routine Description:

    This routine is called to find the Fcb for the given FileId.  We will
    look this up first in the Fcb table and if not found we will create
    an Fcb.  We don't initialize it or insert it into the FcbTable in this
    routine.

    This routine is called while the Vcb is locked.

Arguments:

    FileId - This is the Id for the target Fcb.

    NodeTypeCode - Node type for this Fcb if we need to create.

    FcbExisted - If specified, we store whether the Fcb existed.

Return Value:

    PFCB - The Fcb found in the table or created if needed.

--*/

{
    PFCB NewFcb;
    BOOLEAN LocalFcbExisted;

    PAGED_CODE();

    _SEH2_TRY {

        // Use the local boolean if one was not passed in.

        if (!ARGUMENT_PRESENT(FcbExisted)) {

            FcbExisted = &LocalFcbExisted;
        }

        // Maybe this is already in the table.

        NewFcb = UDFLookupFcbTable(IrpContext, IrpContext->Vcb, FileId);

        // If not then create the Fcb is requested by our caller.

        if (NewFcb == NULL) {

            // Allocate and initialize the structure depending on the
            // type code.

            switch (NodeTypeCode) {

            case UDF_NODE_TYPE_INDEX:

                NewFcb = UDFAllocateFcbIndex();

                RtlZeroMemory(NewFcb, SIZEOF_FCB_INDEX);

                NewFcb->NodeIdentifier.NodeByteSize = SIZEOF_FCB_INDEX;

                break;

            case UDF_NODE_TYPE_DATA:

                NewFcb = UDFAllocateFcbData();

                RtlZeroMemory(NewFcb, SIZEOF_FCB_DATA);

                NewFcb->NodeIdentifier.NodeByteSize = SIZEOF_FCB_DATA;

                break;

            default:

#pragma prefast( suppress: __WARNING_USE_OTHER_FUNCTION, "This is a bug." )   
                UDFBugCheck(0, 0, 0);
            }

            // Now do the common initialization.

            NewFcb->NodeIdentifier.NodeTypeCode = NodeTypeCode;

            NewFcb->Vcb = IrpContext->Vcb;
            NewFcb->FileId = FileId;

            //  Now create the non-paged section object.

            NewFcb->FcbNonpaged = UDFCreateFcbNonpaged(IrpContext);

            *FcbExisted = FALSE;

        } else {

            *FcbExisted = TRUE;
        }

    } _SEH2_FINALLY{

        if (_SEH2_AbnormalTermination()) {

            if (NewFcb && NewFcb->FcbNonpaged) {

                UDFDeleteFcbNonpaged(IrpContext, NewFcb->FcbNonpaged);
            }

            UDFFreePool((PVOID*)&NewFcb);
        }

    } _SEH2_END;

    return NewFcb;
}

/*************************************************************************
*
* Function: UDFInitializeFCB()
*
* Description:
*   Initialize a new FCB structure and also the sent-in file object
*   (if supplied)
*
* Expected Interrupt Level (for execution) :
*
*  IRQL_PASSIVE_LEVEL
*
* Return Value: None
*
*************************************************************************/
NTSTATUS
UDFInitializeFCB(
    IN PFCB             Fcb,            // FCB structure to be initialized
    IN PVCB             Vcb,            // logical volume (VCB) pointer
    IN PtrUDFObjectName PtrObjectName,  // name of the object
    IN ULONG            Flags,          // is this a file/directory, etc.
    IN PFILE_OBJECT     FileObject)     // optional file object to be initialized
{
    ASSERT_LOCKED_VCB(Vcb);

    // Fill NT required Fcb part

    ASSERT(!Fcb->Header.Resource);
    Fcb->Header.Resource = &Fcb->FcbNonpaged->FcbResource;
    Fcb->Header.PagingIoResource = &Fcb->FcbNonpaged->FcbPagingIoResource;
    FsRtlSetupAdvancedHeader(&Fcb->Header, &Fcb->FcbNonpaged->AdvancedFcbHeaderMutex);
    Fcb->FileLock = NULL;

    Fcb->FcbState = Flags;

    UDFInsertFcbTable(IrpContext, Fcb);
    SetFlag(Fcb->FcbState, FCB_STATE_IN_FCB_TABLE);

    // initialize the various list heads
    InitializeListHead(&Fcb->NextCCB);

    Fcb->FcbReference = 0;
    Fcb->FcbCleanup = 0;

    Fcb->FCBName = PtrObjectName;

    Fcb->Vcb = Vcb;

    return STATUS_SUCCESS;
} // end UDFInitializeFCB()

RTL_GENERIC_COMPARE_RESULTS
NTAPI /* ReactOS Change: GCC Does not support STDCALL by default */
UDFFcbTableCompare (
    _In_ PRTL_GENERIC_TABLE FcbTable,
    _In_ PVOID Fid1,
    _In_ PVOID Fid2
    )

/*++

Routine Description:

    This routine is the Cdfs compare routine called by the generic table package.
    If will compare the two File Id values and return a comparison result.

Arguments:

    FcbTable - This is the table being searched.

    Fid1 - First key value.

    Fid2 - Second key value.

Return Value:

    RTL_GENERIC_COMPARE_RESULTS - The results of comparing the two
        input structures

--*/

{
    FILE_ID Id1, Id2;
    PAGED_CODE();

    Id1 = *((FILE_ID UNALIGNED *) Fid1);
    Id2 = *((FILE_ID UNALIGNED *) Fid2);

    if (Id1.QuadPart < Id2.QuadPart) {

        return GenericLessThan;

    } else if (Id1.QuadPart > Id2.QuadPart) {

        return GenericGreaterThan;

    } else {

        return GenericEqual;
    }

    UNREFERENCED_PARAMETER( FcbTable );
}

PVOID
NTAPI /* ReactOS Change: GCC Does not support STDCALL by default */
UDFAllocateFcbTable (
    _In_ PRTL_GENERIC_TABLE FcbTable,
    _In_ CLONG ByteSize
    )

/*++

Routine Description:

    This is a generic table support routine to allocate memory

Arguments:

    FcbTable - Supplies the generic table being used

    ByteSize - Supplies the number of bytes to allocate

Return Value:

    PVOID - Returns a pointer to the allocated data

--*/

{
    PAGED_CODE();
    
    UNREFERENCED_PARAMETER(FcbTable);

    return FsRtlAllocatePoolWithTag(PagedPool, ByteSize, TAG_FCB_TABLE);
}

VOID
NTAPI /* ReactOS Change: GCC Does not support STDCALL by default */
UDFDeallocateFcbTable (
    _In_ PRTL_GENERIC_TABLE FcbTable,
    _In_ __drv_freesMem(Mem) _Post_invalid_ PVOID Buffer
    )
/*++

Routine Description:

    This is a generic table support routine that deallocates memory

Arguments:

    FcbTable - Supplies the generic table being used

    Buffer - Supplies the buffer being deallocated

Return Value:

    None.

--*/

{
    PAGED_CODE();

    UDFFreePool(&Buffer);

    UNREFERENCED_PARAMETER( FcbTable );
}

/*************************************************************************
*
* Function: UDFInitializeVCB()
*
* Description:
*   Perform the initialization for a VCB structure.
*
* Expected Interrupt Level (for execution) :
*
*   IRQL PASSIVE_LEVEL
*
* Return Value: status
*
*************************************************************************/
VOID
UDFInitializeVCB(
    _In_ PIRP_CONTEXT IrpContext,
    _Inout_ PVCB Vcb,
    _In_ PDEVICE_OBJECT TargetDeviceObject,
    _In_ PVPB Vpb,
    _In_ PDISK_GEOMETRY DiskGeometry,
    _In_ ULONG MediaChangeCount
    )
{
    PAGED_CODE();

    UNREFERENCED_PARAMETER(IrpContext);

    // We start by first zeroing out all of the VCB, this will guarantee
    // that any stale data is wiped clean.

    RtlZeroMemory(Vcb, sizeof(VCB));

    // Set the proper node type code and node byte size.

    Vcb->NodeIdentifier.NodeTypeCode = UDF_NODE_TYPE_VCB;
    Vcb->NodeIdentifier.NodeByteSize = sizeof(VCB);

    // Initialize the notify sync mutex. FsRtlNotifyInitializeSync can raise.

    FsRtlNotifyInitializeSync(&Vcb->NotifySync);

    _SEH2_TRY {

        ExInitializeResourceLite(&Vcb->VcbResource);
        ExInitializeResourceLite(&Vcb->BitMapResource1);
        ExInitializeResourceLite(&Vcb->FileIdResource);
        ExInitializeResourceLite(&Vcb->DlocResource);
        ExInitializeResourceLite(&Vcb->DlocResource2);
        ExInitializeResourceLite(&Vcb->FlushResource);
        ExInitializeResourceLite(&Vcb->PreallocResource);
        ExInitializeResourceLite(&Vcb->IoResource);

        ExInitializeFastMutex(&Vcb->VcbMutex);

        // Initialize the generic Fcb Table.

        RtlInitializeGenericTable(&Vcb->FcbTable,
                                  (PRTL_GENERIC_COMPARE_ROUTINE)UDFFcbTableCompare,
                                  (PRTL_GENERIC_ALLOCATE_ROUTINE)UDFAllocateFcbTable,
                                  (PRTL_GENERIC_FREE_ROUTINE)UDFDeallocateFcbTable,
                                  NULL);

        // Pick up a VPB right now so we know we can pull this filesystem stack
        // off of the storage stack on demand.  This can raise - if it does,  
        // uninitialize the notify structures before returning.

        Vcb->SwapVpb = (PVPB)FsRtlAllocatePoolWithTag(NonPagedPoolNx, sizeof(VPB), TAG_VPB);

        RtlZeroMemory(Vcb->SwapVpb, sizeof(VPB));

        // We know the target device object.
        // Note that this is not neccessarily a pointer to the actual
        // physical/virtual device on which the logical volume should
        // be mounted. This is actually a pointer to either the actual
        // (real) device or to any device object that may have been
        // attached to it. Any IRPs that we send down should be sent to this
        // device object. However, the "real" physical/virtual device object
        // on which we perform our mount operation can be determined from the
        // RealDevice field in the VPB sent to us.
        Vcb->TargetDeviceObject = TargetDeviceObject;

        // We also have the VPB pointer. This was obtained from the
        // Parameters.MountVolume.Vpb field in the current I/O stack location
        // for the mount IRP.
        Vcb->Vpb = Vpb;

        //  Set the removable media flag based on the real device's
        //  characteristics
        if (Vpb->RealDevice->Characteristics & FILE_REMOVABLE_MEDIA) {

            Vcb->VcbState |= VCB_STATE_REMOVABLE_MEDIA;
        }

        // Initialize the list anchor (head) for some lists in this VCB.
        InitializeListHead(&Vcb->NextNotifyIRP);

        // Intilize FCB for this VCB

        // Refererence the Vcb for two reasons.  The first is a reference
        // that prevents the Vcb from going away on the last close unless
        // dismount has already occurred.  The second is to make sure
        // we don't go into the dismount path on any error during mount
        // until we get to the Mount cleanup.

        Vcb->VcbResidualReference = UDFS_BASE_RESIDUAL_REFERENCE;
        Vcb->VcbResidualUserReference = UDFS_BASE_RESIDUAL_USER_REFERENCE;

        Vcb->VcbReference = 1 + Vcb->VcbResidualReference;

        Vcb->WCacheMaxBlocks        = UdfData.WCacheMaxBlocks;
        Vcb->WCacheMaxFrames        = UdfData.WCacheMaxFrames;
        Vcb->WCacheBlocksPerFrameSh = UdfData.WCacheBlocksPerFrameSh;
        Vcb->WCacheFramesToKeepFree = UdfData.WCacheFramesToKeepFree;

        // Create a stream file object for this volume.
        //Vcb->PtrStreamFileObject = IoCreateStreamFileObject(NULL,
        //                                            Vcb->Vpb->RealDevice);
        //ASSERT(Vcb->PtrStreamFileObject);

        // Initialize some important fields in the newly created file object.
        //Vcb->PtrStreamFileObject->FsContext = (PVOID)Vcb;
        //Vcb->PtrStreamFileObject->FsContext2 = NULL;
        //Vcb->PtrStreamFileObject->SectionObjectPointer = &(Vcb->SectionObject);

        //Vcb->PtrStreamFileObject->Vpb = PtrVPB;

        // Insert this Vcb record on the CdData.VcbQueue.

        ASSERT_EXCLUSIVE_CDDATA;
        InsertTailList(&(UdfData.VcbQueue), &(Vcb->NextVCB));

        // Initialize caching for the stream file object.
        //CcInitializeCacheMap(Vcb->PtrStreamFileObject, (PCC_FILE_SIZES)(&(Vcb->AllocationSize)),
        //                            TRUE,       // We will use pinned access.
        //                            &(UDFGlobalData.CacheMgrCallBacks), Vcb);

        Vcb->SectorSize = DiskGeometry->BytesPerSector;
        Vcb->SectorShift = UDFHighBit(DiskGeometry->BytesPerSector);
        Vcb->MediaChangeCount = MediaChangeCount;

    } _SEH2_FINALLY {

        if (_SEH2_AbnormalTermination()) {

            FsRtlNotifyUninitializeSync(&Vcb->NotifySync);
        }
    } _SEH2_END;
} // end UDFInitializeVCB()

VOID
UDFCleanUpFCB(
    PFCB Fcb
    )
{
    UDFPrint(("UDFCleanUpFCB: %x\n", Fcb));
    if (!Fcb) return;

    ASSERT_FCB(Fcb);

    _SEH2_TRY {
        // Deinitialize FCBName field
        if (Fcb->FCBName) {
            if (Fcb->FCBName->ObjectName.Buffer) {
                MyFreePool__(Fcb->FCBName->ObjectName.Buffer);
                Fcb->FCBName->ObjectName.Buffer = NULL;
#ifdef UDF_DBG
                Fcb->FCBName->ObjectName.Length =
                Fcb->FCBName->ObjectName.MaximumLength = 0;
#endif
            }
#ifdef UDF_DBG
            else {
                UDFPrint(("UDF: Fcb has invalid FCBName Buffer\n"));
                BrutePoint();
            }
#endif
            UDFReleaseObjectName(Fcb->FCBName);
            Fcb->FCBName = NULL;
        }
#ifdef UDF_DBG
        else {
            UDFPrint(("UDF: Fcb has invalid FCBName field\n"));
            BrutePoint();
        }
#endif


        // begin transaction {

        UDFLockVcb(IrpContext, Fcb->Vcb);

        if (FlagOn(Fcb->FcbState, FCB_STATE_IN_FCB_TABLE)) {

            UDFDeleteFcbTable(IrpContext, Fcb);
            ClearFlag(Fcb->FcbState, FCB_STATE_IN_FCB_TABLE);
        }

        UDFUnlockVcb(IrpContext, Fcb->Vcb);

        // } end transaction

        // Free memory
        UDFDeleteFcb(0, Fcb);
    } _SEH2_EXCEPT(EXCEPTION_EXECUTE_HANDLER) {
        BrutePoint();
    } _SEH2_END;
} // end UDFCleanUpFCB()

NTSTATUS
UDFCompleteMount(
    IN PIRP_CONTEXT IrpContext,
    IN PVCB Vcb
    )
{
    NTSTATUS Status;
    UNICODE_STRING LocalPath;
    PtrUDFObjectName RootName;
    ULONG LastSector = 0;
    BOOLEAN UnlockVcb = FALSE;
    FILE_ID FileId{};

    PAGED_CODE();

    UDFPrint(("UDFCompleteMount:\n"));

    // Use a try-finally to facilitate cleanup.

    _SEH2_TRY {

        Vcb->ZBuffer = (PCHAR)DbgAllocatePoolWithTag(NonPagedPool, max(Vcb->SectorSize, PAGE_SIZE), 'zNWD');

        if (!Vcb->ZBuffer) {

            try_return(Status = STATUS_INSUFFICIENT_RESOURCES);
        }

        RtlZeroMemory(Vcb->ZBuffer, Vcb->SectorSize);

        // Create the root index and reference it in the Vcb.

        Vcb->RootIndexFcb = UDFCreateFcb(IrpContext, FileId, UDF_NODE_TYPE_INDEX, NULL);

        if (!Vcb->RootIndexFcb) {

            try_return(Status = STATUS_INSUFFICIENT_RESOURCES);
        }

        //  Create the File id by hand for this Fcb.

        Vcb->RootIndexFcb->FileId = UdfGetFidFromLbAddr(Vcb->RootLbAddr);
        SetFlag(Vcb->RootIndexFcb->FileId.HighPart, FID_DIR_MASK);

        // Allocate and set root FCB unique name
        RootName = UDFAllocateObjectName();

        if (!RootName) {

            UDFCleanUpFCB(Vcb->RootIndexFcb);
            Vcb->RootIndexFcb = NULL;
            try_return(Status = STATUS_INSUFFICIENT_RESOURCES);
        }

        Status = MyInitUnicodeString(&RootName->ObjectName, UDF_ROOTDIR_NAME);
        if (!NT_SUCCESS(Status))
            goto insuf_res_1;

        Vcb->RootIndexFcb->FileInfo = (PUDF_FILE_INFO)MyAllocatePool__(NonPagedPool,sizeof(UDF_FILE_INFO));

        if (!Vcb->RootIndexFcb->FileInfo) {
            Status = STATUS_INSUFFICIENT_RESOURCES;
    insuf_res_1:
            MyFreePool__(RootName->ObjectName.Buffer);
            UDFReleaseObjectName(RootName);
            UDFCleanUpFCB(Vcb->RootIndexFcb);
            Vcb->RootIndexFcb = NULL;
            try_return(Status);
        }

        UDFPrint(("UDFCompleteMount: open Root Dir\n"));
        // Open Root Directory
        Status = UDFOpenRootFile__(IrpContext, Vcb, &Vcb->RootLbAddr, Vcb->RootIndexFcb->FileInfo);

        if (!NT_SUCCESS(Status)) {

            UDFCleanUpFile__(Vcb, Vcb->RootIndexFcb->FileInfo);
            MyFreePool__(Vcb->RootIndexFcb->FileInfo);
            goto insuf_res_1;
        }

        Vcb->RootIndexFcb->FileInfo->Fcb = Vcb->RootIndexFcb;

        if (!Vcb->RootIndexFcb->FileInfo->Dloc->CommonFcb) {
            Vcb->RootIndexFcb->FileInfo->Dloc->CommonFcb = Vcb->RootIndexFcb;
        }

        UDFLockVcb(IrpContext, Vcb);
        UnlockVcb = TRUE;

        Status = UDFInitializeFCB(Vcb->RootIndexFcb, Vcb, RootName, UDF_FCB_ROOT_DIRECTORY | UDF_FCB_DIRECTORY, NULL);

        if (!NT_SUCCESS(Status)) {

            // if we get here, no resources are inited
            Vcb->RootIndexFcb->FcbCleanup = 0;
            Vcb->RootIndexFcb->FcbReference = 0;

            UDFCleanUpFile__(Vcb, Vcb->RootIndexFcb->FileInfo);
            MyFreePool__(Vcb->RootIndexFcb->FileInfo);
            UDFCleanUpFCB(Vcb->RootIndexFcb);
            Vcb->RootIndexFcb = NULL;
            try_return(Status);
        }

        // this is a part of UDF_RESIDUAL_REFERENCE
        InterlockedIncrement((PLONG)&Vcb->VcbReference);
        Vcb->RootIndexFcb->FcbCleanup = 1;
        Vcb->RootIndexFcb->FcbReference = 1;

        UDFGetFileXTime(Vcb->RootIndexFcb->FileInfo,
                      &(Vcb->RootIndexFcb->CreationTime.QuadPart),
                      &(Vcb->RootIndexFcb->LastAccessTime.QuadPart),
                      &(Vcb->RootIndexFcb->ChangeTime.QuadPart),
                      &(Vcb->RootIndexFcb->LastWriteTime.QuadPart) );

        if (Vcb->SysStreamLbAddr.logicalBlockNum) {
            Vcb->SysSDirFileInfo = (PUDF_FILE_INFO)MyAllocatePool__(NonPagedPool,sizeof(UDF_FILE_INFO));
            if (!Vcb->SysSDirFileInfo) {
                Status = STATUS_INSUFFICIENT_RESOURCES;
                goto unwind_1;
            }
            // Open System SDir Directory
            Status = UDFOpenRootFile__(IrpContext, Vcb, &Vcb->SysStreamLbAddr, Vcb->SysSDirFileInfo);
            if (!NT_SUCCESS(Status)) {
                UDFCleanUpFile__(Vcb, Vcb->SysSDirFileInfo);
                MyFreePool__(Vcb->SysSDirFileInfo);
                Vcb->SysSDirFileInfo = NULL;
                goto unwind_1;
            } else {
                Vcb->SysSDirFileInfo->Dloc->DataLoc.Flags |= EXTENT_FLAG_VERIFY;
            }
        }

        // Open Unallocatable space stream
        // Generally, it should be placed in SystemStreamDirectory, but some
        // stupid apps think that RootDirectory is much better place.... :((
        Status = MyInitUnicodeString(&LocalPath, UDF_FN_NON_ALLOCATABLE);
        if (NT_SUCCESS(Status)) {
            Status = UDFOpenFile__(IrpContext, Vcb, FALSE, TRUE, &LocalPath, Vcb->RootIndexFcb->FileInfo, &Vcb->NonAllocFileInfo, NULL);
            MyFreePool__(LocalPath.Buffer);
        }

        if (!NT_SUCCESS(Status) && (Status != STATUS_OBJECT_NAME_NOT_FOUND)) {

    //unwind_2:
            UDFCleanUpFile__(Vcb, Vcb->NonAllocFileInfo);
            Vcb->NonAllocFileInfo = NULL;
            // this was a part of UDF_RESIDUAL_REFERENCE
            InterlockedDecrement((PLONG)&Vcb->VcbReference);
    unwind_1:

            // UDFCloseResidual() will clean up everything

            try_return(Status);
        }

        /* process Non-allocatable */
        if (NT_SUCCESS(Status)) {
            UDFMarkSpaceAsXXX(Vcb, Vcb->NonAllocFileInfo->Dloc, Vcb->NonAllocFileInfo->Dloc->DataLoc.Mapping, AS_USED); // used
            UDFDirIndex(UDFGetDirIndexByFileInfo(Vcb->NonAllocFileInfo), Vcb->NonAllocFileInfo->Index)->FI_Flags |= UDF_FI_FLAG_FI_INTERNAL;
        } else {
            /* try to read Non-allocatable from alternate locations */
            Status = MyInitUnicodeString(&LocalPath, UDF_FN_NON_ALLOCATABLE_2);
            if (!NT_SUCCESS(Status)) {
                goto unwind_1;
            }
            Status = UDFOpenFile__(IrpContext, Vcb, FALSE, TRUE, &LocalPath, Vcb->RootIndexFcb->FileInfo, &(Vcb->NonAllocFileInfo), NULL);
            MyFreePool__(LocalPath.Buffer);
            if (!NT_SUCCESS(Status) && (Status != STATUS_OBJECT_NAME_NOT_FOUND)) {
                goto unwind_1;
            }
            if (NT_SUCCESS(Status)) {
                UDFMarkSpaceAsXXX(Vcb, Vcb->NonAllocFileInfo->Dloc, Vcb->NonAllocFileInfo->Dloc->DataLoc.Mapping, AS_USED); // used
                UDFDirIndex(UDFGetDirIndexByFileInfo(Vcb->NonAllocFileInfo), Vcb->NonAllocFileInfo->Index)->FI_Flags |= UDF_FI_FLAG_FI_INTERNAL;
            } else
            if (Vcb->SysSDirFileInfo) {
                Status = MyInitUnicodeString(&LocalPath, UDF_SN_NON_ALLOCATABLE);
                if (!NT_SUCCESS(Status)) {
                    goto unwind_1;
                }
                Status = UDFOpenFile__(IrpContext, Vcb, FALSE, TRUE, &LocalPath, Vcb->SysSDirFileInfo , &(Vcb->NonAllocFileInfo), NULL);
                MyFreePool__(LocalPath.Buffer);
                if (!NT_SUCCESS(Status) && (Status != STATUS_OBJECT_NAME_NOT_FOUND)) {
                    goto unwind_1;
                }
                if (NT_SUCCESS(Status)) {
                    UDFMarkSpaceAsXXX(Vcb, Vcb->NonAllocFileInfo->Dloc, Vcb->NonAllocFileInfo->Dloc->DataLoc.Mapping, AS_USED); // used
    //                    UDFDirIndex(UDFGetDirIndexByFileInfo(Vcb->NonAllocFileInfo), Vcb->NonAllocFileInfo->Index)->FI_Flags |= UDF_FI_FLAG_FI_INTERNAL;
                } else {
                    Status = STATUS_SUCCESS;
                }
            } else {
                Status = STATUS_SUCCESS;
            }
        }

        /* Read SN UID mapping */
        if (Vcb->SysSDirFileInfo) {

            LocalPath = RTL_CONSTANT_STRING(UDF_SN_UID_MAPPING);

            Status = UDFOpenFile__(IrpContext, Vcb, FALSE, TRUE, &LocalPath, Vcb->SysSDirFileInfo , &Vcb->UniqueIDMapFileInfo, NULL);

            if (NT_SUCCESS(Status)) {

                Vcb->UniqueIDMapFileInfo->Dloc->DataLoc.Flags |= EXTENT_FLAG_VERIFY;

            } else if  (Status == STATUS_OBJECT_NAME_NOT_FOUND) {

                Vcb->UniqueIDMapFileInfo = NULL;

            } else {

                goto unwind_1;
            }
            Status = STATUS_SUCCESS;
        }

        Status = STATUS_SUCCESS;

        // clear Modified flags. It was not real modify, just
        // bitmap construction
        Vcb->BitmapModified = FALSE;
        //Vcb->Modified = FALSE;
        UDFPreClrModified(Vcb);
        UDFClrModified(Vcb);
        // this is a part of UDF_RESIDUAL_REFERENCE
        InterlockedIncrement((PLONG)&Vcb->VcbReference);

        // Start initializing the fields contained in the Header.

        // DisAllow fast-IO for now.
    //    RootFcb->Header->IsFastIoPossible = FastIoIsNotPossible;
        Vcb->RootIndexFcb->Header.IsFastIoPossible = FastIoIsPossible;

        // Initialize the MainResource and PagingIoResource pointers in
        // the CommonFCBHeader structure to point to the ERESOURCE structures we
        // have allocated and already initialized above.
    //    RootFcb->Header.Resource = &RootFcb->MainResource;
    //    RootFcb->Header.PagingIoResource = &RootFcb->PagingIoResource;

        // Initialize the file size values here.
        Vcb->RootIndexFcb->Header.AllocationSize.QuadPart = 0;
        Vcb->RootIndexFcb->Header.FileSize.QuadPart = 0;

        // The following will disable ValidDataLength support.
    //    RootFcb->Header.ValidDataLength.QuadPart = 0x7FFFFFFFFFFFFFFFI64;
        Vcb->RootIndexFcb->Header.ValidDataLength.QuadPart = 0;

        if (!NT_SUCCESS(Status))
            try_return(Status);

        ASSERT(!Vcb->Modified);

        UDFUnlockVcb(IrpContext, Vcb);
        UnlockVcb = FALSE;

        //  Now do the volume dasd Fcb.  Create this and reference it in the Vcb.

        UDFLockVcb(IrpContext, Vcb);
        UnlockVcb = TRUE;

        Vcb->VolumeDasdFcb = UDFCreateFcb(IrpContext, FileId, UDF_NODE_TYPE_DATA, NULL);

        InitializeListHead(&Vcb->VolumeDasdFcb->NextCCB);

        UDFIncrementReferenceCounts(IrpContext, Vcb->VolumeDasdFcb, 1, 1);
        UDFUnlockVcb(IrpContext, Vcb);
        UnlockVcb = FALSE;

        // Iterate through all partitions in the Pcb structure to find the highest sector number (LastSector)
        // occupied by any physical partition. If the end of the current partition exceeds the current LastSector,
        // update LastSector. This determines the upper boundary of space used on the device.

        for (USHORT RefPartNum = 0; RefPartNum < Vcb->PartitionMaps; RefPartNum++) {

            if ((Vcb->Partitions[RefPartNum].PartitionType == UDF_TYPE1_MAP15 ||
                Vcb->Partitions[RefPartNum].PartitionType == UDF_SPARABLE_MAP15) &&
                Vcb->Partitions[RefPartNum].PartitionRoot +
                Vcb->Partitions[RefPartNum].PartitionLen > LastSector) {

                LastSector = Vcb->Partitions[RefPartNum].PartitionRoot +
                             Vcb->Partitions[RefPartNum].PartitionLen;
            }
        }

        Vcb->VolumeDasdFcb->Header.FileSize.QuadPart = LlBytesFromSectors(Vcb, LastSector);

        Vcb->VolumeDasdFcb->Header.AllocationSize.QuadPart =
        Vcb->VolumeDasdFcb->Header.ValidDataLength.QuadPart = Vcb->VolumeDasdFcb->Header.FileSize.QuadPart;

        // Point to the resource.

        Vcb->VolumeDasdFcb->Header.Resource = &Vcb->VolumeDasdFcb->FcbNonpaged->FcbResource;
        Vcb->VolumeDasdFcb->Header.PagingIoResource = &Vcb->VolumeDasdFcb->FcbNonpaged->FcbPagingIoResource;

        // TODO: use VolumeDasdFcb ?????

        FsRtlSetupAdvancedHeader(&Vcb->VolumeDasdFcb->Header, &Vcb->VolumeDasdFcb->FcbNonpaged->AdvancedFcbHeaderMutex);

        // Mark the Fcb as initialized.

        SetFlag(Vcb->VolumeDasdFcb->FcbState, FCB_STATE_INITIALIZED);

    try_exit:  NOTHING;
    } _SEH2_FINALLY {

        if (_SEH2_AbnormalTermination()) {

            UDFFreePool((PVOID*)&Vcb->ZBuffer);

            // Vcb->VolumeDasdFcb
        }

        if (UnlockVcb) {
            UDFUnlockVcb(IrpContext, Vcb);
        }
    } _SEH2_END;

    return Status;
} // end UDFCompleteMount()
