////////////////////////////////////////////////////////////////////
// Copyright (C) Alexander Telyatnikov, Ivan Keliukh, Yegor Anchishkin, SKIF Software, 1999-2013. Kiev, Ukraine
// All rights reserved
// This file was released under the GPLv2 on June 2015.
////////////////////////////////////////////////////////////////////
/*************************************************************************
*
* File: LockCtrl.cpp.cpp
*
* Module: UDF File System Driver (Kernel mode execution only)
*
* Description:
*   Contains code to handle the "byte-range locking" dispatch entry point.
*
*************************************************************************/

#include            "udffs.h"

// define the file specific bug-check id
#define         UDF_BUG_CHECK_ID                UDF_FILE_SHUTDOWN

/*************************************************************************
*
* Function: UDFCommonLockControl()
*
* Description:
*  This is the common routine for doing Lock control operations called
*  by both the fsd and fsp threads
*
* Expected Interrupt Level (for execution) :
*
*  IRQL_PASSIVE_LEVEL
*
* Return Value: Irrelevant
*
*************************************************************************/
NTSTATUS
NTAPI
UDFCommonLockControl(
    IN PIRP_CONTEXT IrpContext,
    IN PIRP             Irp)
{
    NTSTATUS Status = STATUS_SUCCESS;
    PIO_STACK_LOCATION  IrpSp = IoGetCurrentIrpStackLocation(Irp);
    TYPE_OF_OPEN TypeOfOpen;
    PFCB Fcb = NULL;
    PCCB Ccb = NULL;

    UDFPrint(("UDFCommonLockControl\n"));

    // Extract and decode the type of file object we're being asked to process

    TypeOfOpen = UDFDecodeFileObject(IrpSp->FileObject, &Fcb, &Ccb);

    ASSERT_CCB(Ccb);
    ASSERT_FCB(Fcb);

    // If the file is not a user file open then we reject the request
    // as an invalid parameter

    if (TypeOfOpen != UserFileOpen) {

        UDFCompleteRequest(IrpContext, Irp, STATUS_INVALID_PARAMETER);
        return STATUS_INVALID_PARAMETER;
    }

    UDFAcquireFcbShared(IrpContext, Fcb, FALSE);

    _SEH2_TRY {

        UDFVerifyFcbOperation(IrpContext, Fcb, Ccb);

        // If we don't have a file lock, then get one now.
        if (Fcb->FileLock == NULL) {

            if (!UDFCreateFileLock(NULL, Fcb, FALSE)) {

                try_return(Status = STATUS_INSUFFICIENT_RESOURCES);
            }
        }

        // Now call the FsRtl routine to do the actual processing of the
        // Lock request

        Status = FsRtlProcessFileLock(Fcb->FileLock, Irp, NULL);

        // Set the flag indicating if Fast I/O is possible

        //TODO: impl
        //UDFLockFcb(IrpContext, Fcb);
        Fcb->Header.IsFastIoPossible = UDFIsFastIoPossible(Fcb);
        //UDFUnlockFcb(IrpContext, Fcb);

try_exit: NOTHING;

    } _SEH2_FINALLY {

        UDFReleaseFcb(IrpContext, Fcb);

    } _SEH2_END; // end of "__finally" processing

    // Complete the request.

    UDFCompleteRequest(IrpContext, NULL, Status);

    return Status;
} // end UDFCommonLockControl()


/*
Routine Description:
    This is a call back routine for doing the fast lock call.
Arguments:
    FileObject - Supplies the file object used in this operation
    FileOffset - Supplies the file offset used in this operation
    Length - Supplies the length used in this operation
    ProcessId - Supplies the process ID used in this operation
    Key - Supplies the key used in this operation
    FailImmediately - Indicates if the request should fail immediately
        if the lock cannot be granted.
    ExclusiveLock - Indicates if this is a request for an exclusive or
        shared lock
    IoStatus - Receives the Status if this operation is successful

Return Value:
    BOOLEAN - TRUE if this operation completed and FALSE if caller
        needs to take the long route.
*/

BOOLEAN
NTAPI
UDFFastLock (
    IN PFILE_OBJECT FileObject,
    IN PLARGE_INTEGER FileOffset,
    IN PLARGE_INTEGER Length,
    PEPROCESS ProcessId,
    ULONG Key,
    BOOLEAN FailImmediately,
    BOOLEAN ExclusiveLock,
    OUT PIO_STATUS_BLOCK IoStatus,
    IN PDEVICE_OBJECT DeviceObject
    )
{
    BOOLEAN FcbAcquired = FALSE;
    BOOLEAN Results = FALSE;
    TYPE_OF_OPEN TypeOfOpen;
    PFCB Fcb = NULL;

    // Decode the type of file object we're being asked to process and
    // make sure that is is only a user file open.

    TypeOfOpen = UDFFastDecodeFileObject(FileObject, &Fcb);

    if (TypeOfOpen != UserFileOpen) {

        IoStatus->Status = STATUS_INVALID_PARAMETER;
        return TRUE;
    }

    ASSERT_FCB(Fcb);

    FsRtlEnterFileSystem();

    _SEH2_TRY {

        FcbAcquired = UDFAcquireFcbShared(NULL, Fcb, TRUE);

        if (FcbAcquired == FALSE) {

            try_return(NOTHING);
        }

        //  If we don't have a file lock, then get one now.
        if ((Fcb->FileLock == NULL) && !UDFCreateFileLock(NULL, Fcb, FALSE)) {

            try_return(NOTHING);
        }

        //  We check whether we can proceed
        //  based on the state of the file oplocks.

        //  Now call the FsRtl routine to do the actual processing of the
        //  Lock request
        if ((Results = FsRtlFastLock(Fcb->FileLock,
                                     FileObject,
                                     FileOffset,
                                     Length,
                                     ProcessId,
                                     Key,
                                     FailImmediately,
                                     ExclusiveLock,
                                     IoStatus,
                                     NULL,
                                     FALSE ))) {

            //  Set the flag indicating if Fast I/O is possible
            Fcb->Header.IsFastIoPossible = UDFIsFastIoPossible(Fcb);
        }

try_exit:  NOTHING;
    } _SEH2_FINALLY {

        // Release the Fcb, and return to our caller

        if (FcbAcquired) {

            UDFReleaseFcb(NULL, Fcb);
        }


        FsRtlExitFileSystem();

    } _SEH2_END;

    return Results;
} // end UDFFastLock()


/*
Routine Description:

    This is a call back routine for doing the fast unlock single call.

Arguments:

    FileObject - Supplies the file object used in this operation
    FileOffset - Supplies the file offset used in this operation
    Length - Supplies the length used in this operation
    ProcessId - Supplies the process ID used in this operation
    Key - Supplies the key used in this operation
    Status - Receives the Status if this operation is successful

Return Value:

    BOOLEAN - TRUE if this operation completed and FALSE if caller
        needs to take the long route.
*/
BOOLEAN
NTAPI
UDFFastUnlockSingle(
    IN PFILE_OBJECT FileObject,
    IN PLARGE_INTEGER FileOffset,
    IN PLARGE_INTEGER Length,
    PEPROCESS ProcessId,
    ULONG Key,
    OUT PIO_STATUS_BLOCK IoStatus,
    IN PDEVICE_OBJECT DeviceObject
    )

{
    BOOLEAN Results = FALSE;

//    BOOLEAN             AcquiredFCB = FALSE;
    TYPE_OF_OPEN TypeOfOpen;
    PFCB                Fcb = NULL;

    UDFPrint(("UDFFastUnlockSingle\n"));
    //  Decode the type of file object we're being asked to process and make
    //  sure it is only a user file open.

    IoStatus->Information = 0;

    // Decode the type of file object we're being asked to process and
    // make sure that is is only a user file open.

    TypeOfOpen = UDFFastDecodeFileObject(FileObject, &Fcb);

    ASSERT_FCB(Fcb);

    // Validate the sent-in FCB
    if ( (Fcb == Fcb->Vcb->VolumeDasdFcb) ||
         (Fcb->FcbState & UDF_FCB_DIRECTORY)) {

        IoStatus->Status = STATUS_INVALID_PARAMETER;
        return TRUE;
    }

    // If there is no lock then return immediately.
    if (Fcb->FileLock == NULL) {

        IoStatus->Status = STATUS_RANGE_NOT_LOCKED;
        return TRUE;
    }

    //  Acquire exclusive access to the Fcb this operation can always wait

    FsRtlEnterFileSystem();

    // BUGBUG: kenr
    // (VOID) ExAcquireResourceShared( Fcb->Header.Resource, TRUE );

    _SEH2_TRY {

        //  We check whether we can proceed
        //  based on the state of the file oplocks.

        //  Now call the FsRtl routine to do the actual processing of the
        //  Lock request
        Results = TRUE;
        IoStatus->Status = FsRtlFastUnlockSingle(Fcb->FileLock,
                                                 FileObject,
                                                 FileOffset,
                                                 Length,
                                                 ProcessId,
                                                 Key,
                                                 NULL,
                                                 FALSE);
        //  Set the flag indicating if Fast I/O is possible
        Fcb->Header.IsFastIoPossible = UDFIsFastIoPossible(Fcb);

//try_exit:  NOTHING;
    } _SEH2_FINALLY {

        //  Release the Fcb, and return to our caller

        // BUGBUG: kenr
        //    UDFReleaseResource( (Fcb)->Header.Resource );

        FsRtlExitFileSystem();

    } _SEH2_END;

    return Results;
} // end UDFFastUnlockSingle()


/*
Routine Description:

    This is a call back routine for doing the fast unlock all call.

Arguments:
    FileObject - Supplies the file object used in this operation
    ProcessId - Supplies the process ID used in this operation
    Status - Receives the Status if this operation is successful

Return Value:

    BOOLEAN - TRUE if this operation completed and FALSE if caller
        needs to take the long route.
*/
BOOLEAN
NTAPI
UDFFastUnlockAll(
    IN PFILE_OBJECT FileObject,
    PEPROCESS ProcessId,
    OUT PIO_STATUS_BLOCK IoStatus,
    IN PDEVICE_OBJECT DeviceObject
    )

{
    BOOLEAN Results = FALSE;

//    BOOLEAN             AcquiredFCB = FALSE;
    TYPE_OF_OPEN TypeOfOpen;
    PFCB                Fcb = NULL;

    UDFPrint(("UDFFastUnlockAll\n"));

    IoStatus->Information = 0;

    // Decode the type of file object we're being asked to process and
    // make sure that is is only a user file open.

    TypeOfOpen = UDFFastDecodeFileObject(FileObject, &Fcb);

    ASSERT_FCB(Fcb);

    // Validate the sent-in FCB
    if ( (Fcb == Fcb->Vcb->VolumeDasdFcb) ||
         (Fcb->FcbState & UDF_FCB_DIRECTORY)) {

        IoStatus->Status = STATUS_INVALID_PARAMETER;
        return TRUE;
    }

    //  Acquire shared access to the Fcb this operation can always wait

    FsRtlEnterFileSystem();

    UDF_CHECK_PAGING_IO_RESOURCE(Fcb);
    UDFAcquireResourceShared(&Fcb->FcbNonpaged->FcbResource, TRUE);

    _SEH2_TRY {

        //  We check whether we can proceed
        //  based on the state of the file oplocks.

        //  If we don't have a file lock, then get one now.
        if ((Fcb->FileLock == NULL) && !UDFCreateFileLock(NULL, Fcb, FALSE)) {

            _SEH2_LEAVE;
        }

        //  Now call the FsRtl routine to do the actual processing of the
        //  Lock request
        Results = TRUE;
        IoStatus->Status = FsRtlFastUnlockAll(Fcb->FileLock,
                                              FileObject,
                                              ProcessId,
                                              NULL);

        //  Set the flag indicating if Fast I/O is questionable

        Fcb->Header.IsFastIoPossible = UDFIsFastIoPossible(Fcb);

//try_exit:  NOTHING;
    } _SEH2_FINALLY {

        //  Release the Fcb, and return to our caller

        UDF_CHECK_PAGING_IO_RESOURCE(Fcb);
        UDFReleaseResource(&Fcb->FcbNonpaged->FcbResource);
        FsRtlExitFileSystem();

    } _SEH2_END;

    return Results;
} // end UDFFastUnlockAll()


/*
Routine Description:

    This is a call back routine for doing the fast unlock all call.

Arguments:
    FileObject - Supplies the file object used in this operation
    ProcessId - Supplies the process ID used in this operation
    Status - Receives the Status if this operation is successful

Return Value:

    BOOLEAN - TRUE if this operation completed and FALSE if caller
        needs to take the long route.
*/

BOOLEAN
NTAPI
UDFFastUnlockAllByKey(
    _In_ PFILE_OBJECT FileObject,
    _In_ PVOID ProcessId,
    _In_ ULONG Key,
    _Out_ PIO_STATUS_BLOCK IoStatus,
    _In_ PDEVICE_OBJECT DeviceObject
    )

{
    BOOLEAN Results = FALSE;

//    BOOLEAN             AcquiredFCB = FALSE;
    TYPE_OF_OPEN TypeOfOpen;
    PFCB                Fcb = NULL;

    UDFPrint(("UDFFastUnlockAllByKey\n"));

    IoStatus->Information = 0;

    // Decode the type of file object we're being asked to process and
    // make sure that is is only a user file open.

    TypeOfOpen = UDFFastDecodeFileObject(FileObject, &Fcb);

    ASSERT_FCB(Fcb);

    // Validate the sent-in FCB
    if ( (Fcb == Fcb->Vcb->VolumeDasdFcb) ||
         (Fcb->FcbState & UDF_FCB_DIRECTORY)) {

        IoStatus->Status = STATUS_INVALID_PARAMETER;
        return TRUE;
    }

    //  Acquire shared access to the Fcb this operation can always wait

    FsRtlEnterFileSystem();

    UDF_CHECK_PAGING_IO_RESOURCE(Fcb);
    UDFAcquireResourceShared(&Fcb->FcbNonpaged->FcbResource, TRUE);

    _SEH2_TRY {

        //  We check whether we can proceed
        //  based on the state of the file oplocks.

        //  If we don't have a file lock, then get one now.
        if ((Fcb->FileLock == NULL) && !UDFCreateFileLock( NULL, Fcb, FALSE )) {

            _SEH2_LEAVE;
        }

        //  Now call the FsRtl routine to do the actual processing of the
        //  Lock request
        Results = TRUE;
        IoStatus->Status = FsRtlFastUnlockAllByKey(Fcb->FileLock,
                                                   FileObject,
                                                   (PEPROCESS)ProcessId,
                                                   Key,
                                                   NULL);

        //  Set the flag indicating if Fast I/O is possible

        Fcb->Header.IsFastIoPossible = UDFIsFastIoPossible(Fcb);

//try_exit:  NOTHING;
    } _SEH2_FINALLY {

        //  Release the Fcb, and return to our caller

        UDF_CHECK_PAGING_IO_RESOURCE(Fcb);
        UDFReleaseResource(&Fcb->FcbNonpaged->FcbResource);
        FsRtlExitFileSystem();

    } _SEH2_END;

    return Results;
} // end UDFFastUnlockAllByKey()
