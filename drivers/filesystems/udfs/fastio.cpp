////////////////////////////////////////////////////////////////////
// Copyright (C) Alexander Telyatnikov, Ivan Keliukh, Yegor Anchishkin, SKIF Software, 1999-2013. Kiev, Ukraine
// All rights reserved
// This file was released under the GPLv2 on June 2015.
////////////////////////////////////////////////////////////////////
/*************************************************************************
*
* File: Fastio.cpp
*
* Module: UDF File System Driver (Kernel mode execution only)
*
* Description:
*   Contains code to handle the various "fast-io" calls.
*
*************************************************************************/

#include            "udffs.h"

// define the file specific bug-check id
#define         UDF_BUG_CHECK_ID                UDF_FILE_FAST_IO



/*************************************************************************
*
* Function: UDFFastIoCheckIfPossible()
*
* Description:
*   To fast-io or not to fast-io, that is the question ...
*   This routine helps the I/O Manager determine whether the FSD wishes
*   to permit fast-io on a specific file stream.
*
* Expected Interrupt Level (for execution) :
*
*  IRQL_PASSIVE_LEVEL
*
* Return Value: TRUE/FALSE
*
*************************************************************************/
BOOLEAN
NTAPI
UDFFastIoCheckIfPossible(
    IN PFILE_OBJECT             FileObject,
    IN PLARGE_INTEGER           FileOffset,
    IN ULONG                    Length,
    IN BOOLEAN                  Wait,
    IN ULONG                    LockKey,
    IN BOOLEAN                  CheckForReadOperation,
    OUT PIO_STATUS_BLOCK        IoStatus,
    IN PDEVICE_OBJECT           DeviceObject
    )
{
    BOOLEAN ReturnedStatus = FALSE;
    PFCB Fcb;
    PCCB Ccb;
    LARGE_INTEGER IoLength;

    UNREFERENCED_PARAMETER(Wait);
    UNREFERENCED_PARAMETER(IoStatus);
    UNREFERENCED_PARAMETER(DeviceObject);

    // Decode the type of file object we're being asked to process and
    // make sure that is is only a user file open.

    if (UDFDecodeFileObject(FileObject, &Fcb, &Ccb) != UserFileOpen) {

        return FALSE;
    }

    ASSERT_FCB(Fcb);

    IoLength.QuadPart = Length;

    // Based on whether this is a read or write operation we call
    // fsrtl check for read/write

    if (CheckForReadOperation) {

        if (Fcb->FileLock == NULL ||
            FsRtlFastCheckLockForRead(Fcb->FileLock,
                                      FileOffset,
                                      &IoLength,
                                      LockKey,
                                      FileObject,
                                      PsGetCurrentProcess())) {

            ReturnedStatus = TRUE;
        }
    } else {

        if (Fcb->FileLock == NULL ||
            (!FlagOn(Fcb->Vcb->VcbState, VCB_STATE_MEDIA_WRITE_PROTECT | VCB_STATE_VOLUME_READ_ONLY) &&
            FsRtlFastCheckLockForWrite(Fcb->FileLock,
                                       FileOffset,
                                       &IoLength,
                                       LockKey,
                                       FileObject,
                                       PsGetCurrentProcess()))) {

            ReturnedStatus = TRUE;
        }
    }

    return ReturnedStatus;

} // end UDFFastIoCheckIfPossible()

/*
 */
FAST_IO_POSSIBLE
NTAPI
UDFIsFastIoPossible(
    IN PFCB Fcb
    )
{
    if (Fcb->Vcb->VcbCondition != VcbMounted /*||
        !FsRtlOplockIsFastIoPossible(&(Fcb->Oplock))*/ ) {

        return FastIoIsNotPossible;
    }

    if ((Fcb->FileLock != NULL) &&
        FsRtlAreThereCurrentFileLocks(Fcb->FileLock)) {

        return FastIoIsQuestionable;
    }

    return FastIoIsPossible;
} // end UDFIsFastIoPossible()

/*************************************************************************
*
* Function: UDFFastIoQueryBasicInfo()
*
* Description:
*   Bypass the traditional IRP method to perform a query basic
*   information operation.
*
* Expected Interrupt Level (for execution) :
*
*  IRQL_PASSIVE_LEVEL
*
* Return Value: TRUE/FALSE
*
*************************************************************************/
BOOLEAN
NTAPI
UDFFastIoQueryBasicInfo(
    IN PFILE_OBJECT             FileObject,
    IN BOOLEAN                  Wait,
    OUT PFILE_BASIC_INFORMATION Buffer,
    OUT PIO_STATUS_BLOCK        IoStatus,
    IN PDEVICE_OBJECT           DeviceObject
    )
{
    BOOLEAN ReturnedStatus = FALSE;     // fast i/o failed/not allowed
    TYPE_OF_OPEN TypeOfOpen;
    NTSTATUS Status = STATUS_SUCCESS;
    LONG Length = sizeof(FILE_BASIC_INFORMATION);
    PFCB Fcb;
    PCCB Ccb;
    BOOLEAN FcbAcquired = FALSE;

    // Decode the file object to find the type of open and the data
    // structures.

    TypeOfOpen = UDFDecodeFileObject(FileObject, &Fcb, &Ccb);

    // We only support this request on user file or directory objects.

    if ((TypeOfOpen != UserFileOpen) &&
        (TypeOfOpen != UserDirectoryOpen)) {

        return FALSE;
    }

    ASSERT_FCB(Fcb);
    ASSERT_CCB(Ccb);

    FsRtlEnterFileSystem();

    // if the file is already opended we can satisfy this request
    // immediately 'cause all the data we need must be cached
    _SEH2_TRY {

        FcbAcquired = UDFAcquireFcbShared(NULL, Fcb, TRUE);

        if (FcbAcquired) {

            // Verify FCB operation
            if (UDFVerifyFcbOperation(NULL, Fcb, Ccb)) {

                ReturnedStatus =
                    ((Status = UDFGetBasicInformation(FileObject, Fcb, Buffer, &Length)) == STATUS_SUCCESS);

                IoStatus->Status = Status;

                if (ReturnedStatus) {
                    IoStatus->Information = sizeof(FILE_BASIC_INFORMATION);
                } else {
                    IoStatus->Information = 0;
                }
            }
        }
    } _SEH2_FINALLY {

        if (FcbAcquired) {
            UDFReleaseFcb(NULL, Fcb);
            FcbAcquired = FALSE;
        }

        FsRtlExitFileSystem();

    } _SEH2_END;

    return ReturnedStatus;
} // end UDFFastIoQueryBasicInfo()


/*************************************************************************
*
* Function: UDFFastIoQueryStdInfo()
*
* Description:
*   Bypass the traditional IRP method to perform a query standard
*   information operation.
*
* Expected Interrupt Level (for execution) :
*
*  IRQL_PASSIVE_LEVEL
*
* Return Value: TRUE/FALSE
*
*************************************************************************/
BOOLEAN
NTAPI
UDFFastIoQueryStdInfo(
    IN PFILE_OBJECT FileObject,
    IN BOOLEAN Wait,
    OUT PFILE_STANDARD_INFORMATION Buffer,
    OUT PIO_STATUS_BLOCK IoStatus,
    IN PDEVICE_OBJECT DeviceObject)
{
    BOOLEAN ReturnedStatus = FALSE;     // fast i/o failed/not allowed
    TYPE_OF_OPEN TypeOfOpen;
    NTSTATUS Status = STATUS_SUCCESS;
    LONG Length = sizeof(FILE_STANDARD_INFORMATION);
    PFCB Fcb;
    PCCB Ccb;
    BOOLEAN FcbAcquired = FALSE;

    // Decode the file object to find the type of open and the data
    // structures.

    TypeOfOpen = UDFDecodeFileObject(FileObject, &Fcb, &Ccb);

    // We only support this request on user file or directory objects.

    if ((TypeOfOpen != UserFileOpen) &&
        (TypeOfOpen != UserDirectoryOpen)) {

        return FALSE;
    }

    ASSERT_FCB(Fcb);
    ASSERT_CCB(Ccb);

    FsRtlEnterFileSystem();

    _SEH2_TRY{

        FcbAcquired = UDFAcquireFcbShared(NULL, Fcb, TRUE);

        if (FcbAcquired) {

            // Verify FCB operation
            if (UDFVerifyFcbOperation(NULL, Fcb, Ccb)) {

                ReturnedStatus =
                    ((Status = UDFGetStandardInformation(Fcb, Buffer, &Length)) == STATUS_SUCCESS);

                IoStatus->Status = Status;

                if (ReturnedStatus) {
                    IoStatus->Information = sizeof(FILE_STANDARD_INFORMATION);
                }
                else {
                    IoStatus->Information = 0;
                }
            }
        }
    } _SEH2_FINALLY{

        if (FcbAcquired) {
            UDFReleaseFcb(NULL, Fcb);
            FcbAcquired = FALSE;
        }

        FsRtlExitFileSystem();

    } _SEH2_END;

    return ReturnedStatus;
} // end UDFFastIoQueryStdInfo()

NTSTATUS
NTAPI
UDFFilterCallbackAcquireForCreateSection(
    IN PFS_FILTER_CALLBACK_DATA CallbackData,
    IN PVOID *CompletionContext
    )
{
    UNREFERENCED_PARAMETER(CompletionContext);	

    NT_ASSERT(CallbackData->Operation == FS_FILTER_ACQUIRE_FOR_SECTION_SYNCHRONIZATION);
    NT_ASSERT(CallbackData->SizeOfFsFilterCallbackData == sizeof(FS_FILTER_CALLBACK_DATA));

    MmPrint(("  AcqForCreateSection()\n"));

    PFCB Fcb = (PFCB)CallbackData->FileObject->FsContext;

    // Acquire the MainResource exclusively for the file stream
    UDFAcquireResourceExclusive(&Fcb->FcbNonpaged->FcbResource, TRUE);

    // Return the appropriate status based on the type of synchronization and whether anyone
    // has write access to this file.

    if (CallbackData->Parameters.AcquireForSectionSynchronization.SyncType != SyncTypeCreateSection) {

        return STATUS_FSFILTER_OP_COMPLETED_SUCCESSFULLY;

    } else if (Fcb->ShareAccess.Writers == 0) {

        return STATUS_FILE_LOCKED_WITH_ONLY_READERS;

    } else {

        return STATUS_FILE_LOCKED_WITH_WRITERS;
    }
}

/*************************************************************************
*
* Function: UDFFastIoRelCreateSec()
*
* Description:
*   Not really a fast-io operation. Used by the VMM to release FSD resources
*   after processing a file map (create section object) request.
*
* Expected Interrupt Level (for execution) :
*
*  IRQL_PASSIVE_LEVEL
*
* Return Value: None
*
*************************************************************************/
VOID
NTAPI
UDFFastIoRelCreateSec(
    IN PFILE_OBJECT FileObject)
{
    PFCB Fcb = (PFCB)(FileObject->FsContext);

    MmPrint(("  RelFromCreateSection()\n"));

    // Release the MainResource for the file stream
    UDFReleaseResource(&Fcb->FcbNonpaged->FcbResource);

    return;
} // end UDFFastIoRelCreateSec()


/*************************************************************************
*
* Function: UDFAcqLazyWrite()
*
* Description:
*   Not really a fast-io operation. Used by the NT Cache Mgr to acquire FSD
*   resources before performing a delayed write (write behind/lazy write)
*   operation.
*   NOTE: this function really must succeed since the Cache Manager will
*           typically ignore failure and continue on ...
*
* Expected Interrupt Level (for execution) :
*
*  IRQL_PASSIVE_LEVEL
*
* Return Value: TRUE/FALSE (Cache Manager does not tolerate FALSE well)
*
*************************************************************************/
BOOLEAN NTAPI UDFAcqLazyWrite(
    IN PVOID   Context,
    IN BOOLEAN Wait)
{
    // The context is whatever we passed to the Cache Manager when invoking
    // the CcInitializeCacheMaps() function. In the case of the UDF FSD
    // implementation, this context is a pointer to the NT_REQ_FCB structure.
    PFCB Fcb = (PFCB)Context;

    MmPrint(("  UDFAcqLazyWrite()\n"));

    // Acquire the MainResource in the NT_REQ_FCB. For embedded data files
    // (data stored in ICB), acquire exclusively since data shares sector with
    // metadata. For normal files, acquire shared to allow concurrent access.
    // Note: The lazy-writer typically always supplies WAIT set to TRUE.
    if (Fcb->FcbState & UDF_FCB_EMBEDDED_DATA) {
        if (!UDFAcquireResourceExclusive(&Fcb->FcbNonpaged->FcbResource, Wait))
            return FALSE;
    } else {
        if (!UDFAcquireResourceShared(&Fcb->FcbNonpaged->FcbResource, Wait))
            return FALSE;
    }

    // Now, set the lazy-writer thread id.
    ASSERT(!(Fcb->LazyWriteThread));
    Fcb->LazyWriteThread = PsGetCurrentThread();

    ASSERT(IoGetTopLevelIrp() == NULL);
    IoSetTopLevelIrp((PIRP)FSRTL_CACHE_TOP_LEVEL_IRP);

    // If our FSD needs to perform some special preparations in anticipation
    // of receving a lazy-writer request, do so now.
    return TRUE;
} // end UDFAcqLazyWrite()


/*************************************************************************
*
* Function: UDFRelLazyWrite()
*
* Description:
*   Not really a fast-io operation. Used by the NT Cache Mgr to release FSD
*   resources after performing a delayed write (write behind/lazy write)
*   operation.
*
* Expected Interrupt Level (for execution) :
*
*  IRQL_PASSIVE_LEVEL
*
* Return Value: None
*
*************************************************************************/
VOID
NTAPI
UDFRelLazyWrite(
    IN PVOID   Context)
{
    // The context is whatever we passed to the Cache Manager when invoking
    // the CcInitializeCacheMaps() function. In the case of the UDF FSD
    // implementation, this context is a pointer to the NT_REQ_FCB structure.
    PFCB Fcb = (PFCB)Context;

    MmPrint(("  UDFRelLazyWrite()\n"));

    // Remove the current thread-id from the NT_REQ_FCB
    // and release the MainResource.
    ASSERT(Fcb->LazyWriteThread == PsGetCurrentThread());
    Fcb->LazyWriteThread = 0;

    // Release the acquired resource.
    UDFReleaseResource(&Fcb->FcbNonpaged->FcbResource);

    IoSetTopLevelIrp( NULL );
    return;
} // end UDFRelLazyWrite()


/*************************************************************************
*
* Function: UDFAcqReadAhead()
*
* Description:
*   Not really a fast-io operation. Used by the NT Cache Mgr to acquire FSD
*   resources before performing a read-ahead operation.
*   NOTE: this function really must succeed since the Cache Manager will
*           typically ignore failure and continue on ...
*
* Expected Interrupt Level (for execution) :
*
*  IRQL_PASSIVE_LEVEL
*
* Return Value: TRUE/FALSE (Cache Manager does not tolerate FALSE well)
*
*************************************************************************/
BOOLEAN
NTAPI
UDFAcqReadAhead(
    IN PVOID   Context,
    IN BOOLEAN Wait
    )
{
    // The context is whatever we passed to the Cache Manager when invoking
    // the CcInitializeCacheMaps() function. In the case of the UDF FSD
    // implementation, this context is a pointer to the NT_REQ_FCB structure.
    PFCB Fcb = (PFCB)Context;

    MmPrint(("  AcqForReadAhead()\n"));

    // Acquire the MainResource in the NT_REQ_FCB shared.
    // Note: The read-ahead thread typically always supplies WAIT set to TRUE.
    UDF_CHECK_PAGING_IO_RESOURCE(Fcb);
    if (!UDFAcquireResourceShared(&Fcb->FcbNonpaged->FcbResource, Wait))
        return FALSE;

    ASSERT(IoGetTopLevelIrp() == NULL);
    IoSetTopLevelIrp((PIRP)FSRTL_CACHE_TOP_LEVEL_IRP);

    return TRUE;

} // end UDFAcqReadAhead()


/*************************************************************************
*
* Function: UDFRelReadAhead()
*
* Description:
*   Not really a fast-io operation. Used by the NT Cache Mgr to release FSD
*   resources after performing a read-ahead operation.
*
* Expected Interrupt Level (for execution) :
*
*  IRQL_PASSIVE_LEVEL
*
* Return Value: None
*
*************************************************************************/
VOID
NTAPI
UDFRelReadAhead(
    IN PVOID  Context)
{
    // The context is whatever we passed to the Cache Manager when invoking
    // the CcInitializeCacheMaps() function. In the case of the UDF FSD
    // implementation, this context is a pointer to the NT_REQ_FCB structure.
    PFCB Fcb = (PFCB)Context;

    MmPrint(("  RelFromReadAhead()\n"));

    // Release the acquired resource.
    UDFReleaseResource(&Fcb->FcbNonpaged->FcbResource);

    // Of course, the FSD should undo whatever else seems appropriate at this
    // time.
    IoSetTopLevelIrp( NULL );

    return;
} // end UDFRelReadAhead()

/*************************************************************************
*
* Function: UDFFastIoQueryNetInfo()
*
* Description:
*   Get information requested by a redirector across the network. This call
*   will originate from the LAN Manager server.
*
* Expected Interrupt Level (for execution) :
*
*  IRQL_PASSIVE_LEVEL
*
* Return Value: TRUE/FALSE
*
*************************************************************************/
BOOLEAN
NTAPI
UDFFastIoQueryNetInfo(
    IN PFILE_OBJECT                                 FileObject,
    IN BOOLEAN                                      Wait,
    OUT PFILE_NETWORK_OPEN_INFORMATION              Buffer,
    OUT PIO_STATUS_BLOCK                            IoStatus,
    IN PDEVICE_OBJECT                               DeviceObject)
{
    BOOLEAN ReturnedStatus = FALSE;     // fast i/o failed/not allowed
    TYPE_OF_OPEN TypeOfOpen;
    NTSTATUS Status = STATUS_SUCCESS;
    LONG Length = sizeof(FILE_NETWORK_OPEN_INFORMATION);
    PFCB Fcb;
    PCCB Ccb;
    BOOLEAN FcbAcquired = FALSE;

    // Decode the file object to find the type of open and the data
    // structures.

    TypeOfOpen = UDFDecodeFileObject(FileObject, &Fcb, &Ccb);

    // We only support this request on user file or directory objects.

    if ((TypeOfOpen != UserFileOpen) &&
        (TypeOfOpen != UserDirectoryOpen)) {

        return FALSE;
    }

    ASSERT_FCB(Fcb);
    ASSERT_CCB(Ccb);

    FsRtlEnterFileSystem();

    _SEH2_TRY{

        FcbAcquired = UDFAcquireFcbShared(NULL, Fcb, TRUE);

        if (FcbAcquired) {

            // Verify FCB operation
            if (UDFVerifyFcbOperation(NULL, Fcb, Ccb)) {

                ReturnedStatus =
                    ((Status = UDFGetNetworkInformation(Fcb, Buffer, &Length)) == STATUS_SUCCESS);

                IoStatus->Status = Status;

                if (ReturnedStatus) {
                    IoStatus->Information = sizeof(FILE_NETWORK_OPEN_INFORMATION);
                }
                else {
                    IoStatus->Information = 0;
                }
            }
        }
    } _SEH2_FINALLY{

        if (FcbAcquired) {
            UDFReleaseFcb(NULL, Fcb);
            FcbAcquired = FALSE;
        }

        FsRtlExitFileSystem();

    } _SEH2_END;

    return ReturnedStatus;
} // end UDFFastIoQueryNetInfo()


/*************************************************************************
*
* Function: UDFFastIoMdlRead()
*
* Description:
*   Bypass the traditional IRP method to perform a MDL read operation.
*
* Expected Interrupt Level (for execution) :
*
*  IRQL_PASSIVE_LEVEL
*
* Return Value: TRUE/FALSE
*
*************************************************************************/
/*BOOLEAN UDFFastIoMdlRead(
IN PFILE_OBJECT             FileObject,
IN PLARGE_INTEGER           FileOffset,
IN ULONG                    Length,
IN ULONG                    LockKey,
OUT PMDL*                   MdlChain,
OUT PIO_STATUS_BLOCK        IoStatus,
IN PDEVICE_OBJECT           DeviceObject)
{
    BOOLEAN ReturnedStatus = FALSE;     // fast i/o failed/not allowed
    NTSTATUS RC = STATUS_SUCCESS;
    PtrUDFIrpContext IrpContext = NULL;

    FsRtlEnterFileSystem();

    _SEH2_TRY {

        _SEH2_TRY {

            // See description in UDFFastIoRead() before filling-in the
            // stub here.
            NOTHING;


        } __except (UDFExceptionFilter(IrpContext, GetExceptionInformation())) {

            RC = UDFExceptionHandler(IrpContext, NULL);
        }

        //try_exit: NOTHING;

    } _SEH2_FINALLY {

    }

    FsRtlExitFileSystem();

    return(ReturnedStatus);
}*/


/*************************************************************************
*
* Function: UDFFastIoMdlReadComplete()
*
* Description:
*   Bypass the traditional IRP method to inform the NT Cache Manager and the
*   FSD that the caller no longer requires the data locked in the system cache
*   or the MDL to stay around anymore ..
*
* Expected Interrupt Level (for execution) :
*
*  IRQL_PASSIVE_LEVEL
*
* Return Value: TRUE/FALSE
*
*************************************************************************/
/*BOOLEAN UDFFastIoMdlReadComplete(
IN PFILE_OBJECT             FileObject,
OUT PMDL                            MdlChain,
IN PDEVICE_OBJECT               DeviceObject)
{
    BOOLEAN             ReturnedStatus = FALSE;     // fast i/o failed/not allowed
    NTSTATUS                RC = STATUS_SUCCESS;
   PtrUDFIrpContext IrpContext = NULL;

    FsRtlEnterFileSystem();

    _SEH2_TRY {

        _SEH2_TRY {

            // See description in UDFFastIoRead() before filling-in the
            // stub here.
            NOTHING;

        } __except (UDFExceptionFilter(IrpContext, GetExceptionInformation())) {

            RC = UDFExceptionHandler(IrpContext, NULL);
        }

        //try_exit: NOTHING;

    } _SEH2_FINALLY {

    }

    FsRtlExitFileSystem();

    return(ReturnedStatus);
}*/


/*************************************************************************
*
* Function: UDFFastIoPrepareMdlWrite()
*
* Description:
*   Bypass the traditional IRP method to prepare for a MDL write operation.
*
* Expected Interrupt Level (for execution) :
*
*  IRQL_PASSIVE_LEVEL
*
* Return Value: TRUE/FALSE
*
*************************************************************************/
/*BOOLEAN
UDFFastIoPrepareMdlWrite(
    IN PFILE_OBJECT      FileObject,
    IN PLARGE_INTEGER    FileOffset,
    IN ULONG             Length,
    IN ULONG             LockKey,
    OUT PMDL             *MdlChain,
    OUT PIO_STATUS_BLOCK IoStatus,
    IN PDEVICE_OBJECT    DeviceObject
    )
{
    BOOLEAN              ReturnedStatus = FALSE; // fast i/o failed/not allowed
    NTSTATUS             RC = STATUS_SUCCESS;
   PtrUDFIrpContext IrpContext = NULL;

    FsRtlEnterFileSystem();

    _SEH2_TRY {

        _SEH2_TRY {

            // See description in UDFFastIoRead() before filling-in the
            // stub here.
            NOTHING;

        } __except (UDFExceptionFilter(IrpContext, GetExceptionInformation())) {

            RC = UDFExceptionHandler(IrpContext, NULL);
        }

        //try_exit: NOTHING;

    } _SEH2_FINALLY {

    }

    FsRtlExitFileSystem();

    return(ReturnedStatus);
}*/


/*************************************************************************
*
* Function: UDFFastIoMdlWriteComplete()
*
* Description:
*   Bypass the traditional IRP method to inform the NT Cache Manager and the
*   FSD that the caller has updated the contents of the MDL. This data can
*   now be asynchronously written out to secondary storage by the Cache Mgr.
*
* Expected Interrupt Level (for execution) :
*
*  IRQL_PASSIVE_LEVEL
*
* Return Value: TRUE/FALSE
*
*************************************************************************/
/*BOOLEAN UDFFastIoMdlWriteComplete(
IN PFILE_OBJECT             FileObject,
IN PLARGE_INTEGER               FileOffset,
OUT PMDL                            MdlChain,
IN PDEVICE_OBJECT               DeviceObject)
{
    BOOLEAN             ReturnedStatus = FALSE;     // fast i/o failed/not allowed
    NTSTATUS                RC = STATUS_SUCCESS;
   PtrUDFIrpContext IrpContext = NULL;

    FsRtlEnterFileSystem();

    _SEH2_TRY {

        _SEH2_TRY {

            // See description in UDFFastIoRead() before filling-in the
            // stub here.
            NOTHING;

        } __except (UDFExceptionFilter(IrpContext, GetExceptionInformation())) {

            RC = UDFExceptionHandler(IrpContext, NULL);
        }

        //try_exit: NOTHING;

    } _SEH2_FINALLY {

    }

    FsRtlExitFileSystem();

    return(ReturnedStatus);
}*/


/*************************************************************************
*
* Function: UDFFastIoAcqModWrite()
*
* Description:
*   Not really a fast-io operation. Used by the VMM to acquire FSD resources
*   before initiating a write operation via the Modified Page/Block Writer.
*
* Expected Interrupt Level (for execution) :
*
*  IRQL_PASSIVE_LEVEL
*
* Return Value: STATUS_SUCCESS/Error (__try not to return an error, will 'ya ? :-)
*
*************************************************************************/
NTSTATUS
NTAPI
UDFFastIoAcqModWrite(
    IN PFILE_OBJECT   FileObject,
    IN PLARGE_INTEGER EndingOffset,
    OUT PERESOURCE    *ResourceToRelease,
    IN PDEVICE_OBJECT DeviceObject)
{
    PFCB Fcb = (PFCB)FileObject->FsContext;

    *ResourceToRelease = NULL;

    // We must determine which resource(s) we would like to
    // acquire at this time. We know that a write is imminent;
    // we will probably therefore acquire appropriate resources
    // exclusively.

    // We must first get the FCB and CCB pointers from the file object
    // that is passed in to this function (as an argument). Note that
    // the ending offset (when examined in conjunction with current valid data
    // length) may help us in determining the appropriate resource(s) to acquire.

    // For example, if the ending offset is beyond current valid data length,
    // We may decide to acquire *both* the MainResource and the PagingIoResource
    // exclusively; otherwise, we may decide simply to acquire the PagingIoResource.

    // Consult the text for more information on synchronization in FSDs.

    // One final note; the VMM expects that we will return a pointer to
    // the resource that we acquired (single return value). This pointer
    // will be returned back to we in the release call (below).

    // For embedded data files, acquire exclusive since data shares sector with metadata
    // For normal files, shared is enough
    BOOLEAN Acquired;
    if (Fcb->FcbState & UDF_FCB_EMBEDDED_DATA) {
        Acquired = UDFAcquireResourceExclusive(&Fcb->FcbNonpaged->FcbResource, FALSE);
    } else {
        Acquired = UDFAcquireResourceShared(&Fcb->FcbNonpaged->FcbResource, FALSE);
    }

    if (!Acquired) {
        return STATUS_CANT_WAIT;
    }

    *ResourceToRelease = &Fcb->FcbNonpaged->FcbResource;

    return STATUS_SUCCESS;
} // end UDFFastIoAcqModWrite()


/*************************************************************************
*
* Function: UDFFastIoAcqCcFlush()
*
* Description:
*   Not really a fast-io operation. Used by the NT Cache Mgr to acquire FSD
*   resources before performing a CcFlush() operation on a specific file
*   stream.
*
* Expected Interrupt Level (for execution) :
*
*  IRQL_PASSIVE_LEVEL
*
* Return Value: STATUS_SUCCESS/Error
*
*************************************************************************/
NTSTATUS
NTAPI
UDFFastIoAcqCcFlush(
    IN PFILE_OBJECT FileObject,
    IN PDEVICE_OBJECT DeviceObject
    )
{

    // Once again, the hack for making this look like
    // a recursive call if needed. We cannot let ourselves
    // verify under something that has resources held.
    //
    // This value is good.  We should never try to acquire
    // the file this way underneath of the cache.

    NT_ASSERT(IoGetTopLevelIrp() != (PIRP)FSRTL_CACHE_TOP_LEVEL_IRP);
    
    if (IoGetTopLevelIrp() == NULL) {
        
        IoSetTopLevelIrp((PIRP)FSRTL_CACHE_TOP_LEVEL_IRP);
    }

    // Acquire appropriate resources that will allow correct synchronization
    // with a flush call (and avoid deadlock).

    PFCB Fcb = (PFCB)FileObject->FsContext;

    // For embedded data files, acquire exclusive since data shares sector with metadata
    // For normal files, shared is enough
    if (Fcb->FcbState & UDF_FCB_EMBEDDED_DATA) {
        UDFAcquireResourceExclusive(&Fcb->FcbNonpaged->FcbResource, TRUE);
    } else {
        UDFAcquireResourceShared(&Fcb->FcbNonpaged->FcbResource, TRUE);
    }

    return STATUS_SUCCESS;

} // end UDFFastIoAcqCcFlush()

/*************************************************************************
*
* Function: UDFFastIoRelCcFlush()
*
* Description:
*   Not really a fast-io operation. Used by the NT Cache Mgr to acquire FSD
*   resources before performing a CcFlush() operation on a specific file
*   stream.
*
* Expected Interrupt Level (for execution) :
*
*  IRQL_PASSIVE_LEVEL
*
* Return Value: STATUS_SUCCESS/Error
*
*************************************************************************/
NTSTATUS
NTAPI
UDFFastIoRelCcFlush(
    IN PFILE_OBJECT         FileObject,
    IN PDEVICE_OBJECT       DeviceObject
    )
{
    //  Clear up our hint.
    
    if (IoGetTopLevelIrp() == (PIRP)FSRTL_CACHE_TOP_LEVEL_IRP) {

        IoSetTopLevelIrp(NULL);
    }

    // Release resource acquired in UDFFastIoAcqCcFlush() above.
    PFCB Fcb = (PFCB)FileObject->FsContext;

    UDFReleaseResource(&Fcb->FcbNonpaged->FcbResource);

    return STATUS_SUCCESS;

} // end UDFFastIoRelCcFlush()

BOOLEAN
NTAPI
UDFFastIoCopyRead(
    IN PFILE_OBJECT FileObject,
    IN PLARGE_INTEGER FileOffset,
    IN ULONG Length,
    IN BOOLEAN Wait,
    IN ULONG LockKey,
    IN PVOID Buffer,
    OUT PIO_STATUS_BLOCK IoStatus,
    IN PDEVICE_OBJECT DeviceObject
)
{
    return FALSE;
}

BOOLEAN
NTAPI
UDFFastIoCopyWrite(
    IN PFILE_OBJECT FileObject,
    IN PLARGE_INTEGER FileOffset,
    IN ULONG Length,
    IN BOOLEAN Wait,
    IN ULONG LockKey,
    IN PVOID Buffer,
    OUT PIO_STATUS_BLOCK IoStatus,
    IN PDEVICE_OBJECT DeviceObject
)
{
    return FALSE;
}
