////////////////////////////////////////////////////////////////////
// Copyright (C) Alexander Telyatnikov, Ivan Keliukh, Yegor Anchishkin, SKIF Software, 1999-2013. Kiev, Ukraine
// All rights reserved
// This file was released under the GPLv2 on June 2015.
////////////////////////////////////////////////////////////////////
/*************************************************************************
*
* File: Fileinfo.cpp
*
* Module: UDF File System Driver (Kernel mode execution only)
*
* Description:
*   Contains code to handle the "set/query file information" dispatch
*   entry points.
*
*************************************************************************/

#include            "udffs.h"

// define the file specific bug-check id
#define         UDF_BUG_CHECK_ID                UDF_FILE_INFORMATION

#define         MEM_USREN_TAG                   "US_Ren"
#define         MEM_USREN2_TAG                  "US_Ren2"
#define         MEM_USFIDC_TAG                  "US_FIDC"
#define         MEM_USHL_TAG                    "US_HL"

/*************************************************************************
*
* Function: UDFCommonQueryInfo()
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
UDFCommonQueryInfo(
    PIRP_CONTEXT IrpContext,
    PIRP             Irp
    )
{
    NTSTATUS Status = STATUS_SUCCESS;
    PIO_STACK_LOCATION IrpSp = IoGetCurrentIrpStackLocation(Irp);

    LONG Length;
    FILE_INFORMATION_CLASS FileInformationClass;
    PVOID Buffer;
    TYPE_OF_OPEN TypeOfOpen;
    PFCB Fcb;
    PCCB Ccb;
    BOOLEAN ReleaseFcb = FALSE;

    PAGED_CODE();

    // Reference our input parameters to make things easier

    Length = IrpSp->Parameters.QueryFile.Length;
    FileInformationClass = IrpSp->Parameters.QueryFile.FileInformationClass;
    Buffer = Irp->AssociatedIrp.SystemBuffer;

    // Decode the file object

    TypeOfOpen = UDFDecodeFileObject(IrpSp->FileObject, &Fcb, &Ccb);

    ASSERT_CCB(Ccb);
    ASSERT_FCB(Fcb);

    _SEH2_TRY {

        // We only support query on file and directory handles.

        switch (TypeOfOpen) {

        case UserDirectoryOpen:
        case UserFileOpen:

            UDFAcquireFcbShared(IrpContext, Fcb, FALSE);
            ReleaseFcb = TRUE;

            // Make sure the Fcb is in a usable condition.  This will raise
            // an error condition if the volume is unusable

            UDFVerifyFcbOperation(IrpContext, Fcb, Ccb);

            // Do whatever the caller asked us to do
            switch (FileInformationClass) {
            case FileBasicInformation:
                Status = UDFGetBasicInformation(IrpSp->FileObject, Fcb, (PFILE_BASIC_INFORMATION)Buffer, &Length);
                break;
            case FileStandardInformation:
                Status = UDFGetStandardInformation(Fcb, (PFILE_STANDARD_INFORMATION)Buffer, &Length);
                break;
            case FileNetworkOpenInformation:
                Status = UDFGetNetworkInformation(Fcb, (PFILE_NETWORK_OPEN_INFORMATION)Buffer, &Length);
                break;
            case FileInternalInformation:
                Status = UDFGetInternalInformation(IrpContext, Fcb, (PFILE_INTERNAL_INFORMATION)Buffer, &Length);
                break;
            case FileEaInformation:
                Status = UDFGetEaInformation(IrpContext, Fcb, (PFILE_EA_INFORMATION)Buffer, &Length);
                break;
            case FileNameInformation:

                // We don't allow this operation on a file opened by file Id.

                if (FlagOn(Ccb->Flags, CCB_FLAG_OPEN_BY_ID)) {

                    Status = STATUS_INVALID_PARAMETER;
                    break;
                }

                Status = UDFGetFullNameInformation(IrpSp->FileObject, (PFILE_NAME_INFORMATION)Buffer, &Length);
                break;
            case FileAlternateNameInformation:
                Status = UDFGetAltNameInformation(Fcb, (PFILE_NAME_INFORMATION)Buffer, &Length);
                break;
            //TODO: impl
    //            case FileCompressionInformation:
    //                // Status = UDFGetCompressionInformation(...);
    //                break;
            case FilePositionInformation:
                Status = UDFGetPositionInformation(IrpSp->FileObject, (PFILE_POSITION_INFORMATION)Buffer, &Length);
                break;
            case FileStreamInformation:
                Status = UDFGetFileStreamInformation(IrpContext, Fcb, (PFILE_STREAM_INFORMATION)Buffer, (PULONG)&Length);
                break;
            case FileAllInformation:
                {
                    // We don't allow this operation on a file opened by file Id.

                    if (FlagOn(Ccb->Flags, CCB_FLAG_OPEN_BY_ID)) {

                        Status = STATUS_INVALID_PARAMETER;
                        break;
                    }

                    PFILE_ALL_INFORMATION PtrAllInfo = (PFILE_ALL_INFORMATION)Buffer;

                    Length -= (sizeof(FILE_MODE_INFORMATION) +
                               sizeof(FILE_ACCESS_INFORMATION) +
                               sizeof(FILE_ALIGNMENT_INFORMATION));

                    // Get the remaining stuff.
                    if (!NT_SUCCESS(Status = UDFGetBasicInformation(IrpSp->FileObject, Fcb, &(PtrAllInfo->BasicInformation), &Length)) ||
                        !NT_SUCCESS(Status = UDFGetStandardInformation(Fcb, &(PtrAllInfo->StandardInformation), &Length)) ||
                        !NT_SUCCESS(Status = UDFGetInternalInformation(IrpContext, Fcb, &(PtrAllInfo->InternalInformation), &Length)) ||
                        !NT_SUCCESS(Status = UDFGetEaInformation(IrpContext, Fcb, &(PtrAllInfo->EaInformation), &Length)) ||
                        !NT_SUCCESS(Status = UDFGetPositionInformation(IrpSp->FileObject, &(PtrAllInfo->PositionInformation), &Length)) ||
                        !NT_SUCCESS(Status = UDFGetFullNameInformation(IrpSp->FileObject, &(PtrAllInfo->NameInformation), &Length))
                        )
                        break;
                }

                break;

            default:

                Status = STATUS_INVALID_PARAMETER;
                try_return(Status);
            }

            break;

        default:

            Status = STATUS_INVALID_PARAMETER;
        }

        // Set the information field to the number of bytes actually filled in
        // and then complete the request

        Irp->IoStatus.Information = IrpSp->Parameters.QueryFile.Length - Length;

try_exit:   NOTHING;

    } _SEH2_FINALLY {

        if (ReleaseFcb) {

            UDFReleaseFcb(IrpContext, Fcb);
        }

    } _SEH2_END;// end of "__finally" processing

    // Complete the request if we didn't raise.

    UDFCompleteRequest(IrpContext, Irp, Status);

    return Status;
} // end UDFCommonQueryInfo()

/*************************************************************************
*
* Function: UDFCommonSetInfo()
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
UDFCommonSetInfo(
    PIRP_CONTEXT IrpContext,
    PIRP             Irp
    )
{
    NTSTATUS                Status = STATUS_SUCCESS;
    PIO_STACK_LOCATION      IrpSp = NULL;
    PFILE_OBJECT            FileObject = NULL;
    TYPE_OF_OPEN TypeOfOpen;
    PFCB                    Fcb = NULL;
    PCCB                    Ccb = NULL;
    PVCB                    Vcb = NULL;
    BOOLEAN                 MainResourceAcquired = FALSE;
    BOOLEAN                 ParentResourceAcquired = FALSE;
    BOOLEAN                 PagingIoResourceAcquired = FALSE;
    PVOID                   Buffer = NULL;
    FILE_INFORMATION_CLASS  FunctionalityRequested;
    BOOLEAN                 CanWait = FALSE;
    BOOLEAN                 PostRequest = FALSE;
    BOOLEAN                 VcbAcquired = FALSE;

    TmPrint(("UDFCommonSetInfo: irp %x\n", Irp));

    IrpSp = IoGetCurrentIrpStackLocation(Irp);

    FileObject = IrpSp->FileObject;

    // Decode the file object

    TypeOfOpen = UDFDecodeFileObject(FileObject, &Fcb, &Ccb);

    Vcb = Fcb->Vcb;

    ASSERT_CCB(Ccb);
    ASSERT_FCB(Fcb);
    ASSERT_VCB(Vcb);

    _SEH2_TRY {

        // Case on the type of open we're dealing with

        switch (TypeOfOpen) {

        case UserVolumeOpen:

            // We cannot query the user volume open.

            try_return(Status = STATUS_INVALID_PARAMETER);
            break;
        case UserFileOpen:

            break;

        case UserDirectoryOpen:

            break;

        default:

            try_return(Status = STATUS_INVALID_PARAMETER);
        }


        CanWait = (IrpContext->Flags & IRP_CONTEXT_FLAG_WAIT) ? TRUE : FALSE;

        // The NT I/O Manager always allocates and supplies a system
        // buffer for query and set file information calls.
        // Copying information to/from the user buffer and the system
        // buffer is performed by the I/O Manager and the FSD need not worry about it.
        Buffer = Irp->AssociatedIrp.SystemBuffer;

        // Now, obtain some parameters.
        FunctionalityRequested = IrpSp->Parameters.SetFile.FileInformationClass;
        if ((Vcb->VcbState & VCB_STATE_VOLUME_READ_ONLY) &&
            (FunctionalityRequested != FilePositionInformation)) {
            try_return(Status = STATUS_ACCESS_DENIED);
        }

        if (FunctionalityRequested == FileRenameInformation ||
            FunctionalityRequested == FileLinkInformation) {

            UDFAcquireVcbExclusive(IrpContext, Vcb, FALSE);

            VcbAcquired = TRUE;
        }

        // Rename, and link operations require creation of a directory
        // entry and possibly deletion of another directory entry.

        // Unless this is an operation on a page file, we should go ahead and
        // acquire the FCB exclusively at this time. Note that we will pretty
        // much block out anything being done to the FCB from this point on.
        if ((FunctionalityRequested != FilePositionInformation) &&
            (FunctionalityRequested != FileRenameInformation) &&
            (FunctionalityRequested != FileLinkInformation)) {
            // Acquire the Parent & Main Resources exclusive.
            if (Fcb->FileInfo->ParentFile) {
                UDF_CHECK_PAGING_IO_RESOURCE(Fcb->ParentFcb);
                if (!UDFAcquireResourceExclusive(&Fcb->ParentFcb->FcbNonpaged->FcbResource, CanWait)) {
                    PostRequest = TRUE;
                    try_return(Status = STATUS_PENDING);
                }
                ParentResourceAcquired = TRUE;
            }

            if (!UDFAcquireResourceExclusive(&Fcb->FcbNonpaged->FcbResource, CanWait)) {
                PostRequest = TRUE;
                try_return(Status = STATUS_PENDING);
            }
            MainResourceAcquired = TRUE;

            if (!UDFAcquireResourceExclusive(&Fcb->FcbNonpaged->FcbPagingIoResource, CanWait)) {
                PostRequest = TRUE;
                try_return(Status = STATUS_PENDING);
            }
            PagingIoResourceAcquired = TRUE;
        } else
        // The only operations that could conceivably proceed from this point
        // on are paging-IO read/write operations. For delete, link (rename),
        // set allocation size, and set EOF, should also acquire the paging-IO
        // resource, thereby synchronizing with paging-IO requests.
        if ((FunctionalityRequested == FileDispositionInformation) ||
            (FunctionalityRequested == FileAllocationInformation) ||
            (FunctionalityRequested == FileEndOfFileInformation)) {

            // Acquire the MainResource shared.
            UDF_CHECK_PAGING_IO_RESOURCE(Fcb);
            if (!UDFAcquireResourceShared(&Fcb->FcbNonpaged->FcbResource, CanWait)) {
                PostRequest = TRUE;
                try_return(Status = STATUS_PENDING);
            }
            MainResourceAcquired = TRUE;
            // Acquire the PagingResource exclusive.
            if (!UDFAcquireResourceExclusive(&Fcb->FcbNonpaged->FcbPagingIoResource, CanWait)) {
                PostRequest = TRUE;
                try_return(Status = STATUS_PENDING);
            }
            PagingIoResourceAcquired = TRUE;
        } else if ((FunctionalityRequested != FileRenameInformation) &&
                    (FunctionalityRequested != FileLinkInformation)) {
            // Acquire the MainResource shared.
            UDF_CHECK_PAGING_IO_RESOURCE(Fcb);
            if (!UDFAcquireResourceShared(&Fcb->FcbNonpaged->FcbResource, CanWait)) {
                PostRequest = TRUE;
                try_return(Status = STATUS_PENDING);
            }
            MainResourceAcquired = TRUE;
        }

        // Do whatever the caller asked us to do
        switch (FunctionalityRequested) {
        case FileBasicInformation:
            Status = UDFSetBasicInformation(Fcb, Ccb, FileObject, (PFILE_BASIC_INFORMATION)Buffer);
            break;
        case FilePositionInformation: {
            // Check if no intermediate buffering has been specified.
            // If it was specified, do not allow non-aligned set file
            // position requests to succeed.
            PFILE_POSITION_INFORMATION       PtrFileInfoBuffer;

            PtrFileInfoBuffer = (PFILE_POSITION_INFORMATION)Buffer;

            if (FileObject->Flags & FO_NO_INTERMEDIATE_BUFFERING) {
                if (PtrFileInfoBuffer->CurrentByteOffset.LowPart & IrpSp->DeviceObject->AlignmentRequirement) {
                    // Invalid alignment.
                    try_return(Status = STATUS_INVALID_PARAMETER);
                }
            }

            FileObject->CurrentByteOffset = PtrFileInfoBuffer->CurrentByteOffset;
            break;
        }
        case FileDispositionInformation:
            Status = UDFSetDispositionInfo(IrpContext, FileObject, Fcb, Ccb, (PFILE_DISPOSITION_INFORMATION)Buffer);
            break;
        case FileRenameInformation:
            if (!CanWait) {
                PostRequest = TRUE;
                try_return(Status = STATUS_PENDING);
            }
            Status = UDFSetRenameInfo(IrpContext, Fcb, Ccb, FileObject, (PFILE_RENAME_INFORMATION)Buffer);
            if (Status == STATUS_PENDING) {
                PostRequest = TRUE;
                try_return(Status);
            }
            break;
#ifdef UDF_ALLOW_HARD_LINKS
        case FileLinkInformation:
            if (!CanWait) {
                PostRequest = TRUE;
                try_return(Status = STATUS_PENDING);
            }
            Status = UDFHardLink(IrpContext, IrpSp, Fcb, Ccb, FileObject, (PFILE_LINK_INFORMATION)Buffer);
            break;
#endif //UDF_ALLOW_HARD_LINKS
        case FileAllocationInformation:
            Status = UDFSetAllocationInfo(Fcb, Ccb, Vcb, FileObject, IrpContext, Irp, (PFILE_ALLOCATION_INFORMATION)Buffer);
            break;
        case FileEndOfFileInformation:
            Status = UDFSetEndOfFileInfo(IrpContext, IrpSp, Fcb, Ccb, Vcb, FileObject, Irp, (PFILE_END_OF_FILE_INFORMATION)Buffer);
            break;
        default:
            Status = STATUS_INVALID_PARAMETER;
            try_return(Status);
        }

try_exit:   NOTHING;

    } _SEH2_FINALLY {

        if (PagingIoResourceAcquired) {
            UDFReleaseResource(&Fcb->FcbNonpaged->FcbPagingIoResource);
            PagingIoResourceAcquired = FALSE;
        }

        if (MainResourceAcquired) {
            UDFReleaseResource(&Fcb->FcbNonpaged->FcbResource);
            MainResourceAcquired = FALSE;
        }

        if (ParentResourceAcquired) {
            UDF_CHECK_PAGING_IO_RESOURCE(Fcb->ParentFcb);
            UDFReleaseResource(&(Fcb->ParentFcb->FcbNonpaged->FcbResource));
            ParentResourceAcquired = FALSE;
        }

        if (VcbAcquired) {

            UDFReleaseVcb(IrpContext, Vcb);
            VcbAcquired = FALSE;
        }

        // Post IRP if required
        if (PostRequest) {

            // Since, the I/O Manager gave us a system buffer, we do not
            // need to "lock" anything.

            // Perform the post operation which will mark the IRP pending
            // and will return STATUS_PENDING back to us
            Status = UDFFsdPostRequest(IrpContext, Irp);

        } else {

            if (!_SEH2_AbnormalTermination()) {

#ifdef UDF_DELAYED_CLOSE
                if (NT_SUCCESS(Status)) {

                    if (FunctionalityRequested == FileDispositionInformation) {
                        UDFFspClose(Vcb);
                    }
                }
#endif //UDF_DELAYED_CLOSE

                UDFCompleteRequest(IrpContext, Irp, Status);
            }
        }
    } _SEH2_END;// end of "__finally" processing

    return Status;
} // end UDFCommonSetInfo()

/*
    Return some time-stamps and file attributes to the caller.
 */
NTSTATUS
UDFGetBasicInformation(
    IN PFILE_OBJECT                FileObject,
    IN PFCB                        Fcb,
    IN PFILE_BASIC_INFORMATION     PtrBuffer,
 IN OUT LONG*                      PtrReturnedLength
    )
{
    NTSTATUS            RC = STATUS_SUCCESS;
    PUDF_FILE_INFO      FileInfo;
    PDIR_INDEX_ITEM     DirNdx;

    AdPrint(("UDFGetBasicInformation: \n"));

    _SEH2_TRY {

        if (*PtrReturnedLength < (LONG)sizeof(FILE_BASIC_INFORMATION)) {
            try_return(RC = STATUS_BUFFER_OVERFLOW);
        }

        // Zero out the supplied buffer.
        RtlZeroMemory(PtrBuffer, sizeof(FILE_BASIC_INFORMATION));

        // Get information from the FCB and update TimesCache in DirIndex
        FileInfo = Fcb->FileInfo;

        if (!FileInfo) {
            AdPrint(("!!!!!!!! Bu-u-u-u-u-g !!!!!!!!!!!\n"));
            AdPrint(("!!!! GetBasicInfo to unopened file !!!!\n"));
            try_return(RC = STATUS_INVALID_PARAMETER);
        }

        DirNdx = UDFDirIndex(UDFGetDirIndexByFileInfo(FileInfo), FileInfo->Index);

        PtrBuffer->CreationTime = Fcb->CreationTime;
        DirNdx->CreationTime = PtrBuffer->CreationTime.QuadPart;

        PtrBuffer->LastAccessTime = Fcb->LastAccessTime;
        DirNdx->LastAccessTime = PtrBuffer->LastAccessTime.QuadPart;

        PtrBuffer->LastWriteTime = Fcb->LastWriteTime;
        DirNdx->LastWriteTime = PtrBuffer->LastWriteTime.QuadPart;

        PtrBuffer->ChangeTime = Fcb->ChangeTime;
        DirNdx->ChangeTime = PtrBuffer->ChangeTime.QuadPart;

        // Now fill in the attributes.
        if (Fcb->FcbState & UDF_FCB_DIRECTORY) {
            PtrBuffer->FileAttributes = FILE_ATTRIBUTE_DIRECTORY;
#ifdef UDF_DBG
            if (!FileInfo->Dloc->DirIndex) AdPrint(("*****!!!!! Directory has no DirIndex !!!!!*****\n"));
#endif
        }
        // Similarly, fill in attributes indicating a hidden file, system
        // file, compressed file, temporary file, etc. if the FSD supports
        // such file attribute values.
        PtrBuffer->FileAttributes |= UDFAttributesToNT(DirNdx,NULL);
        if (FileObject->Flags & FO_TEMPORARY_FILE) {
            PtrBuffer->FileAttributes |= FILE_ATTRIBUTE_TEMPORARY;
        } else {
            PtrBuffer->FileAttributes &= ~FILE_ATTRIBUTE_TEMPORARY;
        }
        if (!PtrBuffer->FileAttributes) {
            PtrBuffer->FileAttributes = FILE_ATTRIBUTE_NORMAL;
        }

try_exit: NOTHING;

    } _SEH2_FINALLY {

        if (NT_SUCCESS(RC)) {
            // Return the amount of information filled in.
            (*PtrReturnedLength) -= sizeof(FILE_BASIC_INFORMATION);
        }
    } _SEH2_END;
    return(RC);
} // end UDFGetBasicInformation()


/*
    Return file sizes to the caller.
 */
NTSTATUS
UDFGetStandardInformation(
    IN PFCB                        Fcb,
    IN PFILE_STANDARD_INFORMATION  PtrBuffer,
 IN OUT LONG*                      PtrReturnedLength
    )
{
    NTSTATUS            RC = STATUS_SUCCESS;
    PUDF_FILE_INFO      FileInfo;
//    PVCB Vcb;

    AdPrint(("UDFGetStandardInformation: \n"));

    _SEH2_TRY {

        if (*PtrReturnedLength < (LONG)sizeof(FILE_STANDARD_INFORMATION)) {
            try_return(RC = STATUS_BUFFER_OVERFLOW);
        }

        // Zero out the supplied buffer.
        RtlZeroMemory(PtrBuffer, sizeof(FILE_STANDARD_INFORMATION));

        FileInfo = Fcb->FileInfo;

        if (!FileInfo) {
            AdPrint(("!!!!!!!! Bu-u-u-u-u-g !!!!!!!!!!!\n"));
            AdPrint(("!!!! GetStandardInfo to unopened file !!!!\n"));
            try_return(RC = STATUS_INVALID_PARAMETER);
        }
//        Vcb = Fcb->Vcb;
        PtrBuffer->NumberOfLinks = UDFGetFileLinkCount(FileInfo);
        PtrBuffer->DeletePending = (Fcb->FcbState & UDF_FCB_DELETE_ON_CLOSE) ? TRUE : FALSE;

        //  Case on whether this is a file or a directory, and extract
        //  the information and fill in the fcb/dcb specific parts
        //  of the output buffer
        if (UDFIsADirectory(Fcb->FileInfo)) {
            PtrBuffer->Directory = TRUE;
        } else {
            if (Fcb->Header.AllocationSize.LowPart == 0xffffffff) {
                Fcb->Header.AllocationSize.QuadPart =
                    UDFSysGetAllocSize(Fcb->Vcb, UDFGetFileSize(FileInfo));
            }
            PtrBuffer->AllocationSize = Fcb->Header.AllocationSize;
            PtrBuffer->EndOfFile = Fcb->Header.FileSize;

            PtrBuffer->Directory = FALSE;
        }

        try_exit: NOTHING;
    } _SEH2_FINALLY {
        if (NT_SUCCESS(RC)) {
            // Return the amount of information filled in.
            *PtrReturnedLength -= sizeof(FILE_STANDARD_INFORMATION);
        }
    } _SEH2_END;
    return(RC);
} // end UDFGetStandardInformation()

/*
    Return some time-stamps and file attributes to the caller.
 */
NTSTATUS
UDFGetNetworkInformation(
    IN PFCB                           Fcb,
    IN PFILE_NETWORK_OPEN_INFORMATION PtrBuffer,
 IN OUT PLONG                         PtrReturnedLength
    )
{
    NTSTATUS            RC = STATUS_SUCCESS;
    PUDF_FILE_INFO      FileInfo;

    AdPrint(("UDFGetNetworkInformation: \n"));

    _SEH2_TRY {

        if (*PtrReturnedLength < (LONG)sizeof(FILE_NETWORK_OPEN_INFORMATION)) {
            try_return(RC = STATUS_BUFFER_OVERFLOW);
        }

        // Zero out the supplied buffer.
        RtlZeroMemory(PtrBuffer, sizeof(FILE_NETWORK_OPEN_INFORMATION));

        // Get information from the FCB.
        PtrBuffer->CreationTime = Fcb->CreationTime;
        PtrBuffer->LastAccessTime = Fcb->LastAccessTime;
        PtrBuffer->LastWriteTime = Fcb->LastWriteTime;
        PtrBuffer->ChangeTime = Fcb->ChangeTime;

        FileInfo = Fcb->FileInfo;

        if (!FileInfo) {
            AdPrint(("!!!!!!!! Bu-u-u-u-u-g !!!!!!!!!!!\n"));
            AdPrint(("!!!! UDFGetNetworkInformation to unopened file !!!!\n"));
            try_return(RC = STATUS_INVALID_PARAMETER);
        }
        // Now fill in the attributes.
        if (Fcb->FcbState & UDF_FCB_DIRECTORY) {
            PtrBuffer->FileAttributes = FILE_ATTRIBUTE_DIRECTORY;
#ifdef UDF_DBG
            if (!FileInfo->Dloc->DirIndex) AdPrint(("*****!!!!! Directory has no DirIndex !!!!!*****\n"));
#endif
        } else {
            if (Fcb->Header.AllocationSize.LowPart == 0xffffffff) {
                Fcb->Header.AllocationSize.QuadPart =
                    UDFSysGetAllocSize(Fcb->Vcb, UDFGetFileSize(FileInfo));
            }
            PtrBuffer->AllocationSize = Fcb->Header.AllocationSize;
            PtrBuffer->EndOfFile = Fcb->Header.FileSize;
        }
        // Similarly, fill in attributes indicating a hidden file, system
        // file, compressed file, temporary file, etc. if the FSD supports
        // such file attribute values.
        PtrBuffer->FileAttributes |= UDFAttributesToNT(UDFDirIndex(UDFGetDirIndexByFileInfo(FileInfo), FileInfo->Index),NULL);
        if (!PtrBuffer->FileAttributes) {
            PtrBuffer->FileAttributes = FILE_ATTRIBUTE_NORMAL;
        }

try_exit: NOTHING;

    } _SEH2_FINALLY {
        if (NT_SUCCESS(RC)) {
            // Return the amount of information filled in.
            (*PtrReturnedLength) -= sizeof(FILE_NETWORK_OPEN_INFORMATION);
        }
    } _SEH2_END;
    return(RC);
} // end UDFGetNetworkInformation()


/*
    Return some time-stamps and file attributes to the caller.
 */
NTSTATUS
UDFGetInternalInformation(
    _In_ PIRP_CONTEXT IrpContext,
    _In_ PFCB Fcb,
    _Out_ PFILE_INTERNAL_INFORMATION Buffer,
    _Inout_ PLONG Length
    )
{
    NTSTATUS Status = STATUS_SUCCESS;

    PAGED_CODE();

    UNREFERENCED_PARAMETER(IrpContext);

    AdPrint(("UDFGetInternalInformation\n"));

    if (*Length < (LONG)sizeof(FILE_INTERNAL_INFORMATION)) {

        return STATUS_BUFFER_OVERFLOW;
    }

    // Index number is the file Id number in the Fcb.

    Buffer->IndexNumber = Fcb->FileId;

    *Length -= sizeof(FILE_INTERNAL_INFORMATION);

    return Status;
} // end UDFGetInternalInformation()

/*
    Return zero-filled EAs to the caller.
 */
NTSTATUS
UDFGetEaInformation(
    PIRP_CONTEXT IrpContext,
    IN PFCB                 Fcb,
    IN PFILE_EA_INFORMATION PtrBuffer,
 IN OUT PLONG               PtrReturnedLength
    )
{
    NTSTATUS            RC = STATUS_SUCCESS;

    AdPrint(("UDFGetEaInformation\n"));

    _SEH2_TRY {

        if (*PtrReturnedLength < (LONG)sizeof(FILE_EA_INFORMATION)) {
            try_return(RC = STATUS_BUFFER_OVERFLOW);
        }

        // Zero out the supplied buffer.
        PtrBuffer->EaSize = 0;

try_exit: NOTHING;

    } _SEH2_FINALLY {
        if (NT_SUCCESS(RC)) {
            // Return the amount of information filled in.
            *PtrReturnedLength -= sizeof(FILE_EA_INFORMATION);
        }
    } _SEH2_END;
    return(RC);
} // end UDFGetEaInformation()

/*
    Return file's long name to the caller.
 */
NTSTATUS
UDFGetFullNameInformation(
    IN PFILE_OBJECT                FileObject,
    IN PFILE_NAME_INFORMATION      PtrBuffer,
 IN OUT PLONG                      PtrReturnedLength
    )
{
    ULONG BytesToCopy;

    AdPrint(("UDFGetFullNameInformation\n"));

    /* If buffer can't hold at least the file name length, bail out */
    if (*PtrReturnedLength < (LONG)FIELD_OFFSET(FILE_NAME_INFORMATION, FileName[0]))
        return STATUS_BUFFER_OVERFLOW;

    /* Save file name length, and as much file len, as buffer length allows */
    PtrBuffer->FileNameLength = FileObject->FileName.Length;

    /* Calculate amount of bytes to copy not to overflow the buffer */
    BytesToCopy = min(FileObject->FileName.Length,
                      *PtrReturnedLength - FIELD_OFFSET(FILE_NAME_INFORMATION, FileName[0]));

    /* Fill in the bytes */
    RtlCopyMemory(PtrBuffer->FileName, FileObject->FileName.Buffer, BytesToCopy);

    /* Check if we could write more but are not able to */
    if (*PtrReturnedLength < (LONG)FileObject->FileName.Length + (LONG)FIELD_OFFSET(FILE_NAME_INFORMATION, FileName[0]))
    {
        /* Return number of bytes written */
        *PtrReturnedLength -= FIELD_OFFSET(FILE_NAME_INFORMATION, FileName[0]) + BytesToCopy;
        return STATUS_BUFFER_OVERFLOW;
    }

    /* We filled up as many bytes, as needed */
    *PtrReturnedLength -= (FIELD_OFFSET(FILE_NAME_INFORMATION, FileName[0]) + FileObject->FileName.Length);

    return STATUS_SUCCESS;
} // end UDFGetFullNameInformation()

/*
    Return file short(8.3) name to the caller.
 */
NTSTATUS
UDFGetAltNameInformation(
    IN PFCB                        Fcb,
    IN PFILE_NAME_INFORMATION      PtrBuffer,
    IN OUT PLONG                   PtrReturnedLength
    )
{
    PDIR_INDEX_ITEM DirNdx;
    ULONG BytesToCopy;
    UNICODE_STRING ShortName;
    WCHAR ShortNameBuffer[13];

    AdPrint(("UDFGetAltNameInformation: \n"));

    *PtrReturnedLength -= FIELD_OFFSET(FILE_NAME_INFORMATION, FileName[0]);
    DirNdx = UDFDirIndex(UDFGetDirIndexByFileInfo(Fcb->FileInfo), Fcb->FileInfo->Index);

    ShortName.MaximumLength = 13 * sizeof(WCHAR);
    ShortName.Buffer = (PWCHAR)&ShortNameBuffer;

    UDFDOSName__(Fcb->Vcb, &ShortName, &(DirNdx->FName), Fcb->FileInfo);

    if (*PtrReturnedLength < ShortName.Length) {
        return(STATUS_BUFFER_OVERFLOW);
    } else {
        BytesToCopy = ShortName.Length;
        *PtrReturnedLength -= ShortName.Length;
    }

    RtlCopyMemory( &(PtrBuffer->FileName),
                   ShortName.Buffer,
                   BytesToCopy );

    PtrBuffer->FileNameLength = ShortName.Length;

    return(STATUS_SUCCESS);
} // end UDFGetAltNameInformation()

/*
    Get file position information
 */
NTSTATUS
UDFGetPositionInformation(
    IN PFILE_OBJECT               FileObject,
    IN PFILE_POSITION_INFORMATION PtrBuffer,
 IN OUT PLONG                     PtrReturnedLength
    )
{
    if (*PtrReturnedLength < (LONG)sizeof(FILE_POSITION_INFORMATION)) {
        return(STATUS_BUFFER_OVERFLOW);
    }
    PtrBuffer->CurrentByteOffset = FileObject->CurrentByteOffset;
    // Modify the local variable for BufferLength appropriately.
    *PtrReturnedLength -= sizeof(FILE_POSITION_INFORMATION);

    return(STATUS_SUCCESS);
} // end UDFGetAltNameInformation()

/*
    Get file file stream(s) information
 */
NTSTATUS
UDFGetFileStreamInformation(
    IN PIRP_CONTEXT IrpContext,
    IN PFCB Fcb,
    IN PFILE_STREAM_INFORMATION PtrBuffer,
    IN OUT PULONG PtrReturnedLength
    )
{
    NTSTATUS        RC = STATUS_SUCCESS;
    PUDF_FILE_INFO  FileInfo;
    PUDF_FILE_INFO  SDirInfo;
    PVCB            Vcb;
    BOOLEAN         FcbAcquired = FALSE;
    uint_di         i;
    ULONG CurrentSize;
    ULONG AlignedSize;
    ULONG BufferLength;
    ULONG TotalBytesWritten = 0;
    ULONG PreviousOffset = 0;   // padding of the previous entry (aligned - real)
    PDIR_INDEX_HDR  hSDirIndex;
    PDIR_INDEX_ITEM SDirIndex;
    PDIR_INDEX_ITEM DirNdx;
    PFILE_BOTH_DIR_INFORMATION NTFileInfo = NULL;

    PFILE_STREAM_INFORMATION CurrentInfo = PtrBuffer;
    PFILE_STREAM_INFORMATION Previous = NULL;

    AdPrint(("UDFGetFileStreamInformation\n"));

    DECLARE_CONST_UNICODE_STRING(StreamPrefix, L":");
    DECLARE_CONST_UNICODE_STRING(StreamSuffix, L":$DATA");

    _SEH2_TRY {

        UDFAcquireResourceExclusive(&(Fcb->Vcb->FileIdResource), TRUE);
        FcbAcquired = TRUE;

        FileInfo = Fcb->FileInfo;
        if (!FileInfo) {
            AdPrint(("!!!!!!!! Bu-u-u-u-u-g !!!!!!!!!!!\n"));
            AdPrint(("!!!! UDFGetFileStreamInformation to unopened file !!!!\n"));
            try_return(RC = STATUS_INVALID_PARAMETER);
        }
        Vcb = Fcb->Vcb;

        DirNdx = UDFDirIndex(UDFGetDirIndexByFileInfo(FileInfo), FileInfo->Index);
        ASSERT(DirNdx);

        NTFileInfo = (PFILE_BOTH_DIR_INFORMATION)MyAllocatePool__(NonPagedPool, sizeof(FILE_BOTH_DIR_INFORMATION)+UDF_NAME_LEN*sizeof(WCHAR));
        if (!NTFileInfo) try_return(RC = STATUS_INSUFFICIENT_RESOURCES);

        RC = UDFFileDirInfoToNT(IrpContext, Vcb, DirNdx, NTFileInfo);

        if (!NT_SUCCESS(RC)) {
            try_return(RC);
        }

        // Clear output buffer so alignment padding between entries and the final
        // NextEntryOffset are guaranteed zero.
        BufferLength = *PtrReturnedLength;
        RtlZeroMemory(PtrBuffer, BufferLength);

        CurrentSize = FIELD_OFFSET(FILE_STREAM_INFORMATION, StreamName) + StreamPrefix.Length + StreamSuffix.Length;

        if (CurrentSize > BufferLength) {
            try_return(RC = STATUS_BUFFER_OVERFLOW);
        }

        CurrentInfo->StreamNameLength = StreamPrefix.Length + StreamSuffix.Length;
        CurrentInfo->StreamSize = NTFileInfo->EndOfFile;
        CurrentInfo->StreamAllocationSize = NTFileInfo->AllocationSize;

        RtlCopyMemory(&CurrentInfo->StreamName[0], StreamPrefix.Buffer, StreamPrefix.Length);
        RtlCopyMemory(&CurrentInfo->StreamName[1], StreamSuffix.Buffer, StreamSuffix.Length);

        // Each entry must start on an 8-byte boundary: advance by the aligned
        // size, but count only the real size as written.
        TotalBytesWritten = CurrentSize;
        AlignedSize = UDFQuadAlign(CurrentSize);
        PreviousOffset = AlignedSize - CurrentSize;

        Previous = CurrentInfo;
        CurrentInfo = (PFILE_STREAM_INFORMATION)((ULONG_PTR)CurrentInfo + AlignedSize);

        if (!(SDirInfo = FileInfo->Dloc->SDirInfo) ||
             UDFIsSDirDeleted(SDirInfo) ) {

            *PtrReturnedLength = BufferLength - TotalBytesWritten;
            try_return(RC = STATUS_SUCCESS);
        }

        hSDirIndex = SDirInfo->Dloc->DirIndex;

        for(i=2; (SDirIndex = UDFDirIndex(hSDirIndex,i)); i++) {
            if ((SDirIndex->FI_Flags & UDF_FI_FLAG_FI_INTERNAL) ||
                UDFIsDeleted(SDirIndex) ||
                !SDirIndex->FName.Buffer )
                continue;

            CurrentSize = FIELD_OFFSET(FILE_STREAM_INFORMATION, StreamName) +
                            StreamPrefix.Length + SDirIndex->FName.Length + StreamSuffix.Length;

            // Need room for the previous entry's alignment padding plus this entry.
            if (PreviousOffset + CurrentSize > BufferLength - TotalBytesWritten) {
                RC = STATUS_BUFFER_OVERFLOW;
                break;
            }

            RC = UDFFileDirInfoToNT(IrpContext, Vcb, SDirIndex, NTFileInfo);

            if (!NT_SUCCESS(RC)) {
                try_return(RC);
            }

            CurrentInfo->StreamNameLength = StreamPrefix.Length + SDirIndex->FName.Length + StreamSuffix.Length;
            CurrentInfo->StreamSize = NTFileInfo->EndOfFile;
            CurrentInfo->StreamAllocationSize = NTFileInfo->AllocationSize;

            RtlCopyMemory(&CurrentInfo->StreamName[0], StreamPrefix.Buffer, StreamPrefix.Length);
            RtlCopyMemory(&CurrentInfo->StreamName[1], SDirIndex->FName.Buffer, SDirIndex->FName.Length);
            RtlCopyMemory(&CurrentInfo->StreamName[1 + SDirIndex->FName.Length / sizeof(WCHAR)], StreamSuffix.Buffer, StreamSuffix.Length);

            // Link the previous entry to this one using the aligned distance.
            Previous->NextEntryOffset = (ULONG)((ULONG_PTR)CurrentInfo - (ULONG_PTR)Previous);

            TotalBytesWritten = (ULONG)((ULONG_PTR)CurrentInfo - (ULONG_PTR)PtrBuffer) + CurrentSize;
            AlignedSize = UDFQuadAlign(CurrentSize);
            PreviousOffset = AlignedSize - CurrentSize;

            Previous = CurrentInfo;
            CurrentInfo = (PFILE_STREAM_INFORMATION)((ULONG_PTR)CurrentInfo + AlignedSize);
        }

        // Returned length is the real bytes used (no trailing padding).
        *PtrReturnedLength = BufferLength - TotalBytesWritten;

try_exit: NOTHING;

    } _SEH2_FINALLY {
        if (FcbAcquired)
            UDFReleaseResource(&(Fcb->Vcb->FileIdResource));
        if (NTFileInfo)
           MyFreePool__(NTFileInfo);
    } _SEH2_END;
    return(RC);
} // end UDFGetFileStreamInformation()

//*******************************************************************
/*
    Set some time-stamps and file attributes supplied by the caller.
 */
NTSTATUS
UDFSetBasicInformation(
    IN PFCB                        Fcb,
    IN PCCB                        Ccb,
    IN PFILE_OBJECT                FileObject,
    IN PFILE_BASIC_INFORMATION PtrBuffer)
{
    NTSTATUS        RC = STATUS_SUCCESS;
    ULONG           NotifyFilter = 0;

    AdPrint(("UDFSetBasicInformation\n"));

    _SEH2_TRY {

        // If the user is specifying -1 for a field, that means
        // we should leave that field unchanged, even if we might
        // have otherwise set it ourselves.  We'll set the Ccb flag
        // saying that the user set the field so that we
        // don't do our default updating.

        // We set the field to 0 then so we know not to actually
        // set the field to the user-specified (and in this case,
        // illegal) value.

        if (PtrBuffer->LastWriteTime.QuadPart == -1) {

            SetFlag(Ccb->Flags, UDF_CCB_WRITE_TIME_SET);
            PtrBuffer->LastWriteTime.QuadPart = 0;
        }

        if (PtrBuffer->LastAccessTime.QuadPart == -1) {

            SetFlag(Ccb->Flags, UDF_CCB_ACCESS_TIME_SET);
            PtrBuffer->LastAccessTime.QuadPart = 0;
        }

        if (PtrBuffer->CreationTime.QuadPart == -1) {

            SetFlag(Ccb->Flags, UDF_CCB_CREATE_TIME_SET);
            PtrBuffer->CreationTime.QuadPart = 0;
        }

        // Obtain a pointer to the directory entry associated with
        // the FCB being modifed. The directory entry is obviously
        // part of the data associated with the parent directory that
        // contains this particular file stream.
        if (PtrBuffer->FileAttributes) {
            UDFUpdateAttrTime(Fcb->Vcb, Fcb->FileInfo);
        } else
        if ( UDFIsADirectory(Fcb->FileInfo) &&
            !(Fcb->Vcb->CompatFlags & UDF_VCB_IC_UPDATE_UCHG_DIR_ACCESS_TIME) &&
              ((Fcb->FileInfo->Dloc->DataLoc.Modified ||
                Fcb->FileInfo->Dloc->AllocLoc.Modified ||
                (Fcb->FileInfo->Dloc->FE_Flags & UDF_FE_FLAG_FE_MODIFIED) ||
                Fcb->FileInfo->Dloc->FELoc.Modified))
             ) {
            // ignore Access Time Modification for unchanged Dir
            if (!PtrBuffer->CreationTime.QuadPart &&
               PtrBuffer->LastAccessTime.QuadPart &&
               !PtrBuffer->ChangeTime.QuadPart &&
               !PtrBuffer->LastWriteTime.QuadPart)
                try_return(RC);
        }

        UDFSetFileXTime(Fcb->FileInfo,
            &(PtrBuffer->CreationTime.QuadPart),
            &(PtrBuffer->LastAccessTime.QuadPart),
            &(PtrBuffer->ChangeTime.QuadPart),
            &(PtrBuffer->LastWriteTime.QuadPart) );

        if (PtrBuffer->CreationTime.QuadPart) {
            // The interesting thing here is that the user has set certain time
            // fields. However, before doing this, the user may have performed
            // I/O which in turn would have caused FSD to mark the fact that
            // write/access time should be modifed at cleanup.
            // We'll mark the fact that such updates are no longer
            // required since the user has explicitly specified the values he
            // wishes to see associated with the file stream.
            Fcb->CreationTime = PtrBuffer->CreationTime;
            Ccb->Flags |= UDF_CCB_CREATE_TIME_SET;
            NotifyFilter |= FILE_NOTIFY_CHANGE_CREATION;
        }
        if (PtrBuffer->LastAccessTime.QuadPart) {
            Fcb->LastAccessTime = PtrBuffer->LastAccessTime;
            Ccb->Flags |= UDF_CCB_ACCESS_TIME_SET;
            NotifyFilter |= FILE_NOTIFY_CHANGE_LAST_ACCESS;
        }
        if (PtrBuffer->ChangeTime.QuadPart) {
            Fcb->ChangeTime = PtrBuffer->ChangeTime;
            Ccb->Flags |= UDF_CCB_MODIFY_TIME_SET;
        }
        if (PtrBuffer->LastWriteTime.QuadPart) {
            Fcb->LastWriteTime = PtrBuffer->LastWriteTime;
            Ccb->Flags |= UDF_CCB_WRITE_TIME_SET;
            NotifyFilter |= FILE_NOTIFY_CHANGE_LAST_WRITE;
        }

        // Now come the attributes.
        if (PtrBuffer->FileAttributes) {
            // We have a non-zero attribute value.
            // The presence of a particular attribute indicates that the
            // user wishes to set the attribute value. The absence indicates
            // the user wishes to clear the particular attribute.

            // Our routine ignores unsupported flags
            PtrBuffer->FileAttributes &= ~(FILE_ATTRIBUTE_NORMAL);

            // Similarly, we should pick out other invalid flag values.
            if ( (PtrBuffer->FileAttributes & FILE_ATTRIBUTE_DIRECTORY) &&
               !(Fcb->FcbState & UDF_FCB_DIRECTORY))
                try_return(RC = STATUS_INVALID_PARAMETER);

            if (PtrBuffer->FileAttributes & FILE_ATTRIBUTE_TEMPORARY) {
                if (Fcb->FcbState & UDF_FCB_DIRECTORY)
                    try_return(RC = STATUS_INVALID_PARAMETER);
                FileObject->Flags |= FO_TEMPORARY_FILE;
            } else {
                FileObject->Flags &= ~FO_TEMPORARY_FILE;
            }

            if (PtrBuffer->FileAttributes & FILE_ATTRIBUTE_READONLY) {
                Fcb->FcbState |= UDF_FCB_READ_ONLY;
            } else {
                Fcb->FcbState &= ~UDF_FCB_READ_ONLY;
            }

            UDFAttributesToUDF(UDFDirIndex(UDFGetDirIndexByFileInfo(Fcb->FileInfo), Fcb->FileInfo->Index),
                               NULL, PtrBuffer->FileAttributes);

            (UDFDirIndex(UDFGetDirIndexByFileInfo(Fcb->FileInfo), Fcb->FileInfo->Index))
                ->FI_Flags |= UDF_FI_FLAG_SYS_ATTR;
            // If the FSD supports file compression, we may wish to
            // note the user's preferences for compressing/not compressing
            // the file at this time.
            Ccb->Flags |= UDF_CCB_ATTRIBUTES_SET;
            NotifyFilter |= FILE_NOTIFY_CHANGE_ATTRIBUTES;
        }

        if (NotifyFilter) {

            UDFNotifyFullReportChange(Fcb->Vcb,
                                      Fcb,
                                      NotifyFilter,
                                      FILE_ACTION_MODIFIED);

            UDFSetFileSizeInDirNdx(Fcb->Vcb, Fcb->FileInfo, NULL);
            Fcb->FileInfo->Dloc->FE_Flags |= UDF_FE_FLAG_FE_MODIFIED;
        }

try_exit: NOTHING;
    } _SEH2_FINALLY {
        ;
    } _SEH2_END;
    return(RC);
} // end UDFSetBasicInformation()

NTSTATUS
UDFMarkStreamsForDeletion(
    IN PIRP_CONTEXT IrpContext,
    IN PVCB           Vcb,
    IN PFCB           Fcb,
    IN BOOLEAN        ForDel
    )
{
    NTSTATUS        RC = STATUS_SUCCESS;
    PUDF_FILE_INFO  SDirInfo = NULL;
    PUDF_FILE_INFO  FileInfo = NULL;
    ULONG lc;
    BOOLEAN SDirAcq = FALSE;
    BOOLEAN StrAcq = FALSE;
    uint_di d,i;

    _SEH2_TRY {

        // In some cases we needn't marking Streams for deleteion
        // (Not opened or Don't exist)
        if (UDFIsAStream(Fcb->FileInfo) ||
           UDFIsAStreamDir(Fcb->FileInfo) ||
           !UDFHasAStreamDir(Fcb->FileInfo) ||
           !Fcb->FileInfo->Dloc->SDirInfo ||
           UDFIsSDirDeleted(Fcb->FileInfo->Dloc->SDirInfo) ||
           (UDFGetFileLinkCount(Fcb->FileInfo) > 1) )
            try_return (RC /*=STATUS_SUCCESS*/);

        // We shall mark Streams for deletion if there is no
        // Links to the file. Otherwise we'll delete only the file.
        // If we are asked to unmark Streams, we'll precess the whole Tree
        RC = UDFOpenStreamDir__(IrpContext, Vcb, Fcb->FileInfo, &SDirInfo);
        if (!NT_SUCCESS(RC))
            try_return(RC);

        if (SDirInfo->Fcb) {
            UDF_CHECK_PAGING_IO_RESOURCE(SDirInfo->Fcb);
            UDFAcquireResourceExclusive(&SDirInfo->Fcb->FcbNonpaged->FcbResource, TRUE);
            SDirAcq = TRUE;
        }

        if (!ForDel || ((lc = UDFGetFileLinkCount(Fcb->FileInfo)) < 2)) {

            UDF_DIR_SCAN_CONTEXT ScanContext;
            PDIR_INDEX_ITEM DirNdx;

            // It is not worth checking whether the Stream can be deleted if
            // Undelete requested
            if (ForDel &&
                // scan DirIndex
                UDFDirIndexInitScan(SDirInfo, &ScanContext, 2)) {

                // Check if we can delete Streams
                while((DirNdx = UDFDirIndexScan(&ScanContext, &FileInfo))) {
                    if (!FileInfo)
                        continue;
                    if (FileInfo->Fcb) {

                        MmPrint(("    MmFlushImageSection() for Stream\n"));
                        if (!MmFlushImageSection(&(FileInfo->Fcb->FcbNonpaged->SegmentObject), MmFlushForDelete)) {

                            try_return(RC = STATUS_CANNOT_DELETE);
                        }

                    }
                }
            }
            // (Un)Mark Streams for deletion

            // Perform sequencial Open for Streams & mark 'em
            // for deletion. We should not  get FileInfo pointers directly
            // from DirNdx[i] to prevent great troubles with linked
            // files. We should mark for deletion FI with proper ParentFile
            // pointer.
            d = UDFDirIndexGetLastIndex(SDirInfo->Dloc->DirIndex);
            for(i=2; i<d; i++) {
                RC = UDFOpenFile__(IrpContext,
                                   Vcb,
                                   FALSE,TRUE,NULL,
                                   SDirInfo,&FileInfo,&i);
                ASSERT(NT_SUCCESS(RC) || (RC == STATUS_FILE_DELETED));
                if (NT_SUCCESS(RC)) {
                    if (FileInfo->Fcb) {

                        UDF_CHECK_PAGING_IO_RESOURCE(FileInfo->Fcb);
                        UDFAcquireResourceExclusive(&FileInfo->Fcb->FcbNonpaged->FcbResource, TRUE);
                        StrAcq = TRUE;

#ifndef UDF_ALLOW_LINKS_TO_STREAMS
                        if (UDFGetFileLinkCount(FileInfo) >= 2) {
                            // Currently, UDF_INFO package doesn't
                            // support this case, so we'll inform developer
                            // about this to prevent on-disk space leaks...
                            BrutePoint();
                            try_return(RC = STATUS_CANNOT_DELETE);
                        }
#endif //UDF_ALLOW_LINKS_TO_STREAMS
                        if (ForDel) {
                            AdPrint(("    SET stream DeleteOnClose\n"));
#ifdef UDF_DBG
                            ASSERT(!(FileInfo->Fcb->FcbState & UDF_FCB_ROOT_DIRECTORY));
                            if (FileInfo->ParentFile &&
                               FileInfo->ParentFile->Fcb) {
                                ASSERT(!(FileInfo->ParentFile->Fcb->FcbState & UDF_FCB_ROOT_DIRECTORY));
                            }
#endif // UDF_DBG
                            FileInfo->Fcb->FcbState |= (UDF_FCB_DELETE_ON_CLOSE |
                                                        UDF_FCB_DELETE_PARENT);
                        } else {
                            AdPrint(("    CLEAR stream DeleteOnClose\n"));
                            FileInfo->Fcb->FcbState &= ~(UDF_FCB_DELETE_ON_CLOSE |
                                                         UDF_FCB_DELETE_PARENT);
                        }
                    }
                    UDFCloseFile__(IrpContext, Vcb, FileInfo);
                } else
                if (RC == STATUS_FILE_DELETED) {
                    // That's OK if STATUS_FILE_DELETED returned...
                    RC = STATUS_SUCCESS;
                }
                if (FileInfo) {
                    if (UDFCleanUpFile__(Vcb, FileInfo)) {
                        ASSERT(!StrAcq && !(FileInfo->Fcb));
                        MyFreePool__(FileInfo);
                    }
                    if (StrAcq) {
                        UDF_CHECK_PAGING_IO_RESOURCE(FileInfo->Fcb);
                        UDFReleaseResource(&FileInfo->Fcb->FcbNonpaged->FcbResource);
                        StrAcq = FALSE;
                    }
                }
                FileInfo = NULL;
            }
            // Mark SDir for deletion
            if (SDirInfo->Fcb) {
                if (ForDel) {
#ifdef UDF_DBG
                    ASSERT(!(SDirInfo->Fcb->FcbState & UDF_FCB_ROOT_DIRECTORY));
                    if (SDirInfo->ParentFile &&
                       SDirInfo->ParentFile->Fcb) {
                        ASSERT(!(SDirInfo->ParentFile->Fcb->FcbState & UDF_FCB_ROOT_DIRECTORY));
                    }
#endif // UDF_DBG
                    AdPrint(("    SET stream dir DeleteOnClose\n"));
                    SDirInfo->Fcb->FcbState |= (UDF_FCB_DELETE_ON_CLOSE |
                                                UDF_FCB_DELETE_PARENT);
                } else {
                    AdPrint(("    CLEAR stream dir DeleteOnClose\n"));
                    SDirInfo->Fcb->FcbState &= ~(UDF_FCB_DELETE_ON_CLOSE |
                                                 UDF_FCB_DELETE_PARENT);
                }
            }
        } else
        if (lc >= 2) {
            // if caller wants us to perform DelTree for Streams, but
            // someone keeps Stream opened and there is a Link to this
            // file, we can't delete it immediately (on Cleanup) & should
            // not delete the whole Tree. Instead, we'll set DELETE_PARENT
            // flag in SDir to kill this file later, when all the Handles
            // to Streams, opened via this file, would be closed
#ifdef UDF_DBG
            ASSERT(!(SDirInfo->Fcb->FcbState & UDF_FCB_ROOT_DIRECTORY));
            if (SDirInfo->ParentFile &&
               SDirInfo->ParentFile->Fcb) {
                ASSERT(!(SDirInfo->ParentFile->Fcb->FcbState & UDF_FCB_ROOT_DIRECTORY));
            }
#endif // UDF_DBG
            if (SDirInfo->Fcb)
                SDirInfo->Fcb->FcbState |= UDF_FCB_DELETE_PARENT;
        }

try_exit: NOTHING;

    } _SEH2_FINALLY {
        if (FileInfo) {
            UDFCloseFile__(IrpContext, Vcb, FileInfo);
            if (UDFCleanUpFile__(Vcb, FileInfo)) {
                ASSERT(!StrAcq && !(FileInfo->Fcb));
                MyFreePool__(FileInfo);
            }
            if (StrAcq) {
                UDF_CHECK_PAGING_IO_RESOURCE(FileInfo->Fcb);
                UDFReleaseResource(&FileInfo->Fcb->FcbNonpaged->FcbResource);
            }
            SDirInfo = NULL;
        }
        if (SDirInfo) {
            UDFCloseFile__(IrpContext, Vcb, SDirInfo);
            if (SDirAcq) {
                UDF_CHECK_PAGING_IO_RESOURCE(SDirInfo->Fcb);
                UDFReleaseResource(&SDirInfo->Fcb->FcbNonpaged->FcbResource);
            }
            if (UDFCleanUpFile__(Vcb, SDirInfo)) {
                MyFreePool__(SDirInfo);
            }
            SDirInfo = NULL;
        }
    } _SEH2_END;
    return RC;
} // end UDFMarkStreamsForDeletion()

/*
    (Un)Mark file for deletion.
 */
NTSTATUS
UDFSetDispositionInfo(
    IN PIRP_CONTEXT IrpContext,
    IN PFILE_OBJECT FileObject,
    IN PFCB Fcb,
    IN PCCB Ccb,
    IN PFILE_DISPOSITION_INFORMATION Buffer
    )
{
    NTSTATUS RC = STATUS_SUCCESS;
    PVCB Vcb = IrpContext->Vcb;
//    PUDF_FILE_INFO  SDirInfo = NULL;
//    PUDF_FILE_INFO  FileInfo = NULL;
    ULONG lc;

    // Skip if this is the same FCB as Volume DASD

    if (Fcb == Vcb->VolumeDasdFcb) {

        return STATUS_ACCESS_DENIED;
    }

    if (!Buffer->DeleteFile) {

        // The user doesn't want to delete the file so clear
        // the delete on close bit

        Fcb->FcbState &= ~UDF_FCB_DELETE_ON_CLOSE;
        FileObject->DeletePending = FALSE;

        RC = UDFMarkStreamsForDeletion(IrpContext, Vcb, Fcb, FALSE); // Undelete
        return RC;
    }

    _SEH2_TRY {

        AdPrint(("    SET DeleteOnClose\n"));

        // The easy part is over. Now, we know that the user wishes to
        // delete the corresponding directory entry (of course, if this
        // is the only link to the file stream, any on-disk storage space
        // associated with the file stream will also be released when the
        // (only) link is deleted!)

        // Do some checking to see if the file can even be deleted.
        if (Fcb->FcbState & UDF_FCB_DELETE_ON_CLOSE) {
            // All done!
            try_return(RC);
        }

        if (Vcb->VcbState & VCB_STATE_VOLUME_READ_ONLY) {
            try_return(RC = STATUS_CANNOT_DELETE);
        }

        if (Fcb->FcbState & UDF_FCB_READ_ONLY) {
            RC = UDFCheckAccessRights(NULL, NULL, Fcb->ParentFcb, NULL, FILE_DELETE_CHILD, 0);
            if (!NT_SUCCESS(RC)) {
                try_return (RC = STATUS_CANNOT_DELETE);
            }
        }

        // It would not be prudent to allow deletion of either a root
        // directory or a directory that is not empty.
        if (Fcb->FcbState & UDF_FCB_ROOT_DIRECTORY)
            try_return(RC = STATUS_CANNOT_DELETE);

        lc = UDFGetFileLinkCount(Fcb->FileInfo);

        if (Fcb->FcbState & UDF_FCB_DIRECTORY) {
            // Perform check to determine whether the directory
            // is empty or not.
            if (!UDFIsDirEmpty__(Fcb->FileInfo)) {
                 try_return(RC = STATUS_DIRECTORY_NOT_EMPTY);
            }

        } else {
            // An important step is to check if the file stream has been
            // mapped by any process. The delete cannot be allowed to proceed
            // in this case.
            MmPrint(("    MmFlushImageSection()\n"));

            if (!MmFlushImageSection(&Fcb->FcbNonpaged->SegmentObject,
                    (lc > 1) ? MmFlushForWrite : MmFlushForDelete)) {

                try_return(RC = STATUS_CANNOT_DELETE);
            }
        }
        // We should also mark Streams for deletion if there are no
        // Links to the file. Otherwise we'll delete only the file

        if (lc > 1) {
            RC = STATUS_SUCCESS;
        } else {
            RC = UDFMarkStreamsForDeletion(IrpContext, Vcb, Fcb, TRUE); // Delete
            if (!NT_SUCCESS(RC))
                try_return(RC);
        }

        // Set a flag to indicate that this directory entry will become history
        // at cleanup.
        Fcb->FcbState |= UDF_FCB_DELETE_ON_CLOSE;
        if (FileObject)
            FileObject->DeletePending = TRUE;

        if ((Fcb->FcbState & UDF_FCB_DIRECTORY) && Ccb) {
            FsRtlNotifyFullChangeDirectory( Vcb->NotifySync, &(Vcb->NextNotifyIRP),
                                            (PVOID)Ccb, NULL, FALSE, FALSE,
                                            0, NULL, NULL, NULL );
        }

try_exit: NOTHING;

    } _SEH2_FINALLY {
        ;
    } _SEH2_END;
    return(RC);
} // end UDFSetDispositionInformation()


/*
      Change file allocation length.
 */
NTSTATUS
UDFSetAllocationInfo(
    IN PFCB                            Fcb,
    IN PCCB                            Ccb,
    IN PVCB                            Vcb,
    IN PFILE_OBJECT                    FileObject,
    IN PIRP_CONTEXT IrpContext,
    IN PIRP                            Irp,
    IN PFILE_ALLOCATION_INFORMATION    Buffer
    )
{
    NTSTATUS        RC = STATUS_SUCCESS;
    BOOLEAN         TruncatedFile = FALSE;
    BOOLEAN         ModifiedAllocSize = FALSE;
    BOOLEAN         CacheMapInitialized = FALSE;

    // Allocation is only allowed on a file and not a directory

    if (NodeType(Fcb) != UDF_NODE_TYPE_DATA) {

        return STATUS_INVALID_PARAMETER;
    }

    UDFAcquirePagingIoExclusive(IrpContext, Fcb);

    _SEH2_TRY {
        // Increasing the allocation size associated with a file stream
        // is relatively easy. All we have to do is execute some FSD
        // specific code to check whether we have enough space available
        // (and if the FSD supports user/volume quotas, whether the user
        // is not exceeding quota), and then increase the file size in the
        // corresponding on-disk and in-memory structures.
        // Then, all we should do is inform the Cache Manager about the
        // increased allocation size.

        Fcb->Header.IsFastIoPossible = UDFIsFastIoPossible(Fcb);

        if ((FileObject->SectionObjectPointer->DataSectionObject != NULL) &&
            (FileObject->SectionObjectPointer->SharedCacheMap == NULL) &&
            !FlagOn(Irp->Flags, IRP_PAGING_IO)) {
            ASSERT( !FlagOn( FileObject->Flags, FO_CLEANUP_COMPLETE ) );
            //  Now initialize the cache map.
            MmPrint(("    CcInitializeCacheMap()\n"));
            CcInitializeCacheMap(FileObject,
                                 (PCC_FILE_SIZES)&Fcb->Header.AllocationSize,
                                 FALSE,
                                 &(UdfData.CacheMgrCallBacks),
                                 Fcb);

            CacheMapInitialized = TRUE;
        }

        // Are we increasing the allocation size?
        if (Fcb->Header.AllocationSize.QuadPart <
            Buffer->AllocationSize.QuadPart) {

            // Yes. Do the FSD specific stuff i.e. increase reserved
            // space on disk.
            if (((LONGLONG)UDFGetFreeSpace(Vcb) << Vcb->SectorShift) < Buffer->AllocationSize.QuadPart) {
                try_return(RC = STATUS_DISK_FULL);
            }
//          RC = STATUS_SUCCESS;
            ModifiedAllocSize = TRUE;

        } else if (Fcb->Header.AllocationSize.QuadPart > Buffer->AllocationSize.QuadPart) {
            // This is the painful part. See if the VMM will allow us to proceed.
            // The VMM will deny the request if:
            // (a) any image section exists OR
            // (b) a data section exists and the size of the user mapped view
            //       is greater than the new size
            // Otherwise, the VMM should allow the request to proceed.
            MmPrint(("    MmCanFileBeTruncated()\n"));
            if (!MmCanFileBeTruncated(&Fcb->FcbNonpaged->SegmentObject, &Buffer->AllocationSize)) {
                // VMM said no way!
                try_return(RC = STATUS_USER_MAPPED_FILE);
            }

            // Perform our directory entry modifications. Release any on-disk
            // space we may need to in the process.
            ModifiedAllocSize = TRUE;
            TruncatedFile = TRUE;
        }

        ASSERT(NT_SUCCESS(RC));
        // This is a good place to check if we have performed a truncate
        // operation. If we have perform a truncate (whether we extended
        // or reduced file size or even leave it intact), we should update
        // file time stamps.
        FileObject->Flags |= FO_FILE_MODIFIED;

        // Last, but not the lease, we must inform the Cache Manager of file size changes.
        if (ModifiedAllocSize) {

            // If we decreased the allocation size to less than the
            // current file size, modify the file size value.
            // Similarly, if we decreased the value to less than the
            // current valid data length, modify that value as well.

            // Update the FCB Header with the new allocation size.
            if (TruncatedFile) {
                if (Fcb->Header.ValidDataLength.QuadPart > Buffer->AllocationSize.QuadPart) {
                    // Decrease the valid data length value.
                    Fcb->Header.ValidDataLength =
                        Buffer->AllocationSize;
                }
                if (Fcb->Header.FileSize.QuadPart > Buffer->AllocationSize.QuadPart) {
                    // Decrease the file size value.
                    Fcb->Header.FileSize =
                        Buffer->AllocationSize;
                    RC = UDFResizeFile__(IrpContext, Vcb, Fcb->FileInfo, Buffer->AllocationSize.QuadPart);
//                    UDFSetFileSizeInDirNdx(Vcb, Fcb->FileInfo, NULL);
                }
            } else {
                Fcb->Header.AllocationSize = Buffer->AllocationSize;
//                UDFSetFileSizeInDirNdx(Vcb, Fcb->FileInfo,
//                                       &(PtrBuffer->AllocationSize.QuadPart));
            }

            // If the FCB has not had caching initiated, it is still valid
            // for us to invoke the NT Cache Manager. It is possible in such
            // situations for the call to be no'oped (unless some user has
            // mapped in the file)

            // NOTE: The invocation to CcSetFileSizes() will quite possibly
            //  result in a recursive call back into the file system.
            //  This is because the NT Cache Manager will typically
            //  perform a flush before telling the VMM to purge pages
            //  especially when caching has not been initiated on the
            //  file stream, but the user has mapped the file into
            //  the process' virtual address space.
            MmPrint(("    CcSetFileSizes()\n"));

            CcSetFileSizes(FileObject, (PCC_FILE_SIZES)&(Fcb->Header.AllocationSize));

            Fcb->NtReqFCBFlags |= UDF_NTREQ_FCB_MODIFIED;

            // Inform any pending IRPs (notify change directory).
            if (UDFIsAStream(Fcb->FileInfo)) {
                UDFNotifyFullReportChange(Vcb, Fcb,
                                          FILE_NOTIFY_CHANGE_STREAM_SIZE,
                                          FILE_ACTION_MODIFIED_STREAM);
            } else {
                UDFNotifyFullReportChange(Vcb, Fcb,
                                          FILE_NOTIFY_CHANGE_SIZE,
                                          FILE_ACTION_MODIFIED);
            }
        }

try_exit: NOTHING;

    } _SEH2_FINALLY {

        if (CacheMapInitialized) {

            MmPrint(("    CcUninitializeCacheMap()\n"));
            CcUninitializeCacheMap(FileObject, NULL, NULL);
        }

        UDFReleasePagingIo(IrpContext, Fcb);

    } _SEH2_END;
    return(RC);
} // end UDFSetAllocationInformation()

/*
    Set end of file (resize).
 */
NTSTATUS
UDFSetEndOfFileInfo(
    IN PIRP_CONTEXT IrpContext,
    IN PIO_STACK_LOCATION              IrpSp,
    IN PFCB                            Fcb,
    IN PCCB                            Ccb,
    IN PVCB                            Vcb,
    IN PFILE_OBJECT                    FileObject,
    IN PIRP                            Irp,
    IN PFILE_END_OF_FILE_INFORMATION   PtrBuffer
    )
{
    NTSTATUS        RC = STATUS_SUCCESS;
    BOOLEAN         TruncatedFile = FALSE;
    BOOLEAN         ModifiedAllocSize = FALSE;
    ULONG           Attr;
    PDIR_INDEX_ITEM DirNdx;
    LONGLONG        OldFileSize;
//    BOOLEAN         ZeroBlock;
    BOOLEAN         CacheMapInitialized = FALSE;

    // Allocation is only allowed on a file and not a directory

    if (NodeType(Fcb) != UDF_NODE_TYPE_DATA) {

        return STATUS_INVALID_PARAMETER;
    }

    UDFAcquirePagingIoExclusive(IrpContext, Fcb);

    _SEH2_TRY {
        // Increasing the allocation size associated with a file stream
        // is relatively easy. All we have to do is execute some FSD
        // specific code to check whether we have enough space available
        // (and if the FSD supports user/volume quotas, whether the user
        // is not exceeding quota), and then increase the file size in the
        // corresponding on-disk and in-memory structures.
        // Then, all we should do is inform the Cache Manager about the
        // increased allocation size.

        if ((Fcb->FcbState & UDF_FCB_DELETED) ||
           (Fcb->NtReqFCBFlags & UDF_NTREQ_FCB_DELETED)) {
#ifdef UDF_DBG
            if (UDFGetFileLinkCount(Fcb->FileInfo) < 1) {
                BrutePoint();
                try_return(RC = STATUS_SUCCESS);
            } else
#endif // UDF_DBG
                try_return(RC = STATUS_SUCCESS);
        }

        Fcb->Header.IsFastIoPossible = UDFIsFastIoPossible(Fcb);

        if ((FileObject->SectionObjectPointer->DataSectionObject != NULL) &&
            (FileObject->SectionObjectPointer->SharedCacheMap == NULL) &&
            !(Irp->Flags & IRP_PAGING_IO)) {

            ASSERT( !FlagOn( FileObject->Flags, FO_CLEANUP_COMPLETE ) );
            //  Now initialize the cache map.
            MmPrint(("    CcInitializeCacheMap()\n"));
            CcInitializeCacheMap(FileObject,
                                 (PCC_FILE_SIZES)&Fcb->Header.AllocationSize,
                                 FALSE,
                                 &UdfData.CacheMgrCallBacks,
                                 Fcb);

            CacheMapInitialized = TRUE;
        }

        //  Do a special case here for the lazy write of file sizes.
        if (IrpSp->Parameters.SetFile.AdvanceOnly) {
            //  Never have the dirent filesize larger than the fcb filesize
            PtrBuffer->EndOfFile.QuadPart =
                min(PtrBuffer->EndOfFile.QuadPart, Fcb->Header.FileSize.QuadPart);
            //  Only advance the file size, never reduce it with this call
            RC = STATUS_SUCCESS;
            if (UDFGetFileSizeFromDirNdx(Vcb, Fcb->FileInfo) >=
               PtrBuffer->EndOfFile.QuadPart)
                try_return(RC);

            UDFSetFileSizeInDirNdx(Vcb, Fcb->FileInfo, &(PtrBuffer->EndOfFile.QuadPart));
            goto notify_size_changes;
        }

        //             !!! IMPORTANT !!!

        // We can get here after all Handles to the file are closed
        // To prevent allocation size incoherency we should
        // reference FileInfo _before_ call to UDFResizeFile__()
        // and use UDFCloseFile__() _after_ that

        // Are we increasing the allocation size?
        OldFileSize = Fcb->Header.FileSize.QuadPart;
        if (OldFileSize < PtrBuffer->EndOfFile.QuadPart) {

            // Yes. Do the FSD specific stuff i.e. increase reserved
            // space on disk.
/*
            if (FileObject->PrivateCacheMap)
                ZeroBlock = TRUE;
*/

            // reference file to pretend that it is opened
            UDFReferenceFile__(Fcb->FileInfo);
            InterlockedIncrement((PLONG)&Fcb->FcbReference);
            // perform resize operation
            RC = UDFResizeFile__(IrpContext, Vcb, Fcb->FileInfo, PtrBuffer->EndOfFile.QuadPart);
            // dereference file
            UDFCloseFile__(IrpContext, Vcb, Fcb->FileInfo);
            InterlockedDecrement((PLONG)&Fcb->FcbReference);
            // update values in NtReqFcb
            Fcb->Header.FileSize.QuadPart =
//            NtReqFcb->CommonFCBHeader.ValidDataLength.QuadPart =
                PtrBuffer->EndOfFile.QuadPart;
            ModifiedAllocSize = TRUE;

        } else if (Fcb->Header.FileSize.QuadPart > PtrBuffer->EndOfFile.QuadPart) {

            // This is the painful part. See if the VMM will allow us to proceed.
            // The VMM will deny the request if:
            // (a) any image section exists OR
            // (b) a data section exists and the size of the user mapped view
            //       is greater than the new size
            // Otherwise, the VMM should allow the request to proceed.

            MmPrint(("    MmCanFileBeTruncated()\n"));
            if (!MmCanFileBeTruncated(&Fcb->FcbNonpaged->SegmentObject, &PtrBuffer->EndOfFile)) {
                // VMM said no way!
                try_return(RC = STATUS_USER_MAPPED_FILE);
            }

            // Perform directory entry modifications. Release any on-disk
            // space we may need to in the process.
            UDFReferenceFile__(Fcb->FileInfo);
            InterlockedIncrement((PLONG)&Fcb->FcbReference);
            // perform resize operation
            RC = UDFResizeFile__(IrpContext, Vcb, Fcb->FileInfo, PtrBuffer->EndOfFile.QuadPart);
            // dereference file
            UDFCloseFile__(IrpContext, Vcb, Fcb->FileInfo);
            InterlockedDecrement((PLONG)&Fcb->FcbReference);

            ModifiedAllocSize = TRUE;
            TruncatedFile = TRUE;
        }

        // This is a good place to check if we have performed a truncate
        // operation. If we have perform a truncate (whether we extended
        // or reduced file size), we should update file time stamps.

        // Last, but not the least, we must inform the Cache Manager of file size changes.
        if (ModifiedAllocSize && NT_SUCCESS(RC)) {
            // If we decreased the allocation size to less than the
            // current file size, modify the file size value.
            // Similarly, if we decreased the value to less than the
            // current valid data length, modify that value as well.
            if (TruncatedFile) {
                if (Fcb->Header.ValidDataLength.QuadPart > PtrBuffer->EndOfFile.QuadPart) {
                    // Decrease the valid data length value.
                    Fcb->Header.ValidDataLength = PtrBuffer->EndOfFile;
                }
                if (Fcb->Header.FileSize.QuadPart > PtrBuffer->EndOfFile.QuadPart) {
                    // Decrease the file size value.
                    Fcb->Header.FileSize = PtrBuffer->EndOfFile;
                }
                UDFSetFileSizeInDirNdx(Vcb, Fcb->FileInfo, NULL);
            } else {
                // Update the FCB Header with the new allocation size.
                // NT expects AllocationSize to be decreased on Close only
                Fcb->Header.AllocationSize.QuadPart =
                    PtrBuffer->EndOfFile.QuadPart;
//                    UDFSysGetAllocSize(Vcb, UDFGetFileSize(Fcb->FileInfo));
                UDFSetFileSizeInDirNdx(Vcb, Fcb->FileInfo, &(PtrBuffer->EndOfFile.QuadPart));
            }

            FileObject->Flags |= FO_FILE_MODIFIED;
//                UDFGetFileAllocationSize(Vcb, Fcb->FileInfo);

            // If the FCB has not had caching initiated, it is still valid
            // for us to invoke the NT Cache Manager. It is possible in such
            // situations for the call to be no'oped (unless some user has
            // mapped in the file)

            // Archive bit
            if (Vcb->CompatFlags & UDF_VCB_IC_UPDATE_ARCH_BIT) {
                DirNdx = UDFDirIndex(UDFGetDirIndexByFileInfo(Fcb->FileInfo), Fcb->FileInfo->Index);
                Ccb->Flags &= ~UDF_CCB_ATTRIBUTES_SET;
                Attr = UDFAttributesToNT(DirNdx, Fcb->FileInfo->Dloc->FileEntry);
                if (!(Attr & FILE_ATTRIBUTE_ARCHIVE))
                    UDFAttributesToUDF(DirNdx, Fcb->FileInfo->Dloc->FileEntry, Attr | FILE_ATTRIBUTE_ARCHIVE);
            }

            // NOTE: The invocation to CcSetFileSizes() will quite possibly
            //  result in a recursive call back into the file system.
            //  This is because the NT Cache Manager will typically
            //  perform a flush before telling the VMM to purge pages
            //  especially when caching has not been initiated on the
            //  file stream, but the user has mapped the file into
            //  the process' virtual address space.
            MmPrint(("    CcSetFileSizes(), thrd:%8.8x\n",PsGetCurrentThread()));

            CcSetFileSizes(FileObject, (PCC_FILE_SIZES)&Fcb->Header.AllocationSize);

/*            if (ZeroBlock) {
                UDFZeroDataEx(NtReqFcb,
                              OldFileSize,
                              PtrBuffer->EndOfFile.QuadPart - OldFileSize,
                              TRUE // CanWait, Vcb, FileObject);
            }*/
            Fcb->NtReqFCBFlags |= UDF_NTREQ_FCB_MODIFIED;

notify_size_changes:

            // Inform any pending IRPs (notify change directory).
            if (UDFIsAStream(Fcb->FileInfo)) {
                UDFNotifyFullReportChange( Vcb, Fcb,
                                           FILE_NOTIFY_CHANGE_STREAM_SIZE,
                                           FILE_ACTION_MODIFIED_STREAM);
            } else {
                UDFNotifyFullReportChange( Vcb, Fcb,
                                           FILE_NOTIFY_CHANGE_SIZE,
                                           FILE_ACTION_MODIFIED);
            }
        }

try_exit: NOTHING;

    } _SEH2_FINALLY {

        if (CacheMapInitialized) {

            MmPrint(("    CcUninitializeCacheMap()\n"));
            CcUninitializeCacheMap( FileObject, NULL, NULL );
        }

        UDFReleasePagingIo(IrpContext, Fcb);

    } _SEH2_END;
    return(RC);
} // end UDFSetEndOfFileInfo()

NTSTATUS
UDFPrepareForRenameMoveLink(
    PVCB Vcb,
    PBOOLEAN SingleDir,
    PBOOLEAN AcquiredDir1,
    PBOOLEAN AcquiredFcb1,
    IN PCCB Ccb1,
    PUDF_FILE_INFO File1,
    PUDF_FILE_INFO Dir1,
    PUDF_FILE_INFO Dir2,
    BOOLEAN HardLink
    )
{
    // convert acquisition to Exclusive
    // this will prevent us from the following situation:
    // There is a pair of objects among input dirs &
    // one of them is a parent of another. Sequential resource
    // acquisition may lead to deadlock due to concurrent
    // CleanUpFcbChain() or UDFCloseFileInfoChain()
    InterlockedIncrement((PLONG)&Vcb->VcbReference);


    (*SingleDir) = ((Dir1 == Dir2) && (Dir1->Fcb));

    if (!(*SingleDir) ||
       (UDFGetFileLinkCount(File1) != 1)) {
        InterlockedDecrement((PLONG)&Vcb->VcbReference);
    } else {
        InterlockedDecrement((PLONG)&Vcb->VcbReference);

        UDF_CHECK_PAGING_IO_RESOURCE(Dir1->Fcb);
        UDFAcquireResourceExclusive(&Dir1->Fcb->FcbNonpaged->FcbResource, TRUE);
        (*AcquiredDir1) = TRUE;

        UDF_CHECK_PAGING_IO_RESOURCE(File1->Fcb);
        UDFAcquireResourceExclusive(&File1->Fcb->FcbNonpaged->FcbResource, TRUE);
        (*AcquiredFcb1) = TRUE;
    }
    return STATUS_SUCCESS;
} // end UDFPrepareForRenameMoveLink()

/*
    Rename or move file
 */
NTSTATUS
UDFSetRenameInfo(
    IN PIRP_CONTEXT IrpContext,
    IN PFCB Fcb,
    IN PCCB Ccb,
    IN PFILE_OBJECT FileObject,   // Source File
    IN PFILE_RENAME_INFORMATION PtrBuffer
    )
{
    PIRP Irp = IrpContext->Irp;
    PIO_STACK_LOCATION IrpSp = IoGetCurrentIrpStackLocation(Irp);
    PFILE_OBJECT TargetFileObject = IrpSp->Parameters.SetFile.FileObject;
    BOOLEAN ReplaceIfExists = IrpSp->Parameters.SetFile.ReplaceIfExists;

    PFCB ParentFcb = Fcb->ParentFcb;

    NTSTATUS RC;
    PVCB Vcb = Fcb->Vcb;
    PFCB TargetFcb;
    BOOLEAN IgnoreCase;
    BOOLEAN ParentFcbAcquired = FALSE;
    BOOLEAN TargetParentFcbAcquired = FALSE;
    BOOLEAN SingleDir = TRUE;
    BOOLEAN UseClose;

    PUDF_FILE_INFO FileInfo;
    PUDF_FILE_INFO DirInfo;
    PUDF_FILE_INFO TargetDirInfo;
    PUDF_FILE_INFO NextFileInfo, fi;

    UNICODE_STRING NewName;
    UNICODE_STRING LocalPath;
    PCCB CurCcb = NULL;
    PLIST_ENTRY Link;
    ULONG i;
    ULONG DirRefCount;
    ULONG FileInfoRefCount;
    ULONG Attr;
    PDIR_INDEX_ITEM DirNdx;

    LocalPath.Buffer = NULL;

    _SEH2_TRY {
        // do we try to rename Volume ?
        if (ParentFcb == NULL) {

             try_return (RC = STATUS_INVALID_PARAMETER);
        }

        // do we try to rename RootDir ?
        if (Fcb == Vcb->RootIndexFcb) {

            try_return (RC = STATUS_INVALID_PARAMETER);
        }

        FileInfo = Fcb->FileInfo;
        ASSERT(FileInfo);
        DirInfo = FileInfo->ParentFile;
        ASSERT(DirInfo);

        // We don't allow this operation on a file opened by file Id.

        if (FlagOn(Ccb->Flags, CCB_FLAG_OPEN_BY_ID | CCB_FLAG_OPEN_RELATIVE_BY_ID)) {

            try_return (RC = STATUS_INVALID_PARAMETER);
        }

        // do we try to rename to RootDir or Volume ?
        if (!TargetFileObject) {

            TargetDirInfo = FileInfo->ParentFile;

        } else {

            PCCB TargetCcb;
            UDFDecodeFileObject(TargetFileObject, &TargetFcb, &TargetCcb);

            ASSERT_FCB(TargetFcb);

            if (!TargetFcb) {
                try_return (RC = STATUS_INVALID_PARAMETER);
            }

            TargetDirInfo = TargetFcb->FileInfo;
        }

        // invalid destination ?
        if (!TargetDirInfo) try_return (RC = STATUS_ACCESS_DENIED);

        // Stream can't be a Dir or have StreamDir
        if (UDFIsAStreamDir(TargetDirInfo)) {
            if (UDFIsADirectory(FileInfo) ||
               UDFHasAStreamDir(FileInfo)) {
                try_return (RC = STATUS_ACCESS_DENIED);
            }
        }

        SingleDir = (TargetDirInfo->Fcb == ParentFcb);

        if (SingleDir) {

            //TODO: FsRtlAreNamesEqual

            // Check ReplaceIfExists for same directory
            if (ReplaceIfExists) {

                UDFAcquireFcbExclusive(IrpContext, ParentFcb, FALSE);
                ParentFcbAcquired = TRUE;
            }

        } else {

            UDFAcquireFcbExclusive(IrpContext, TargetDirInfo->Fcb, FALSE);
            TargetParentFcbAcquired = TRUE;
        }

        // check if the source file is in use
        if (Fcb->FcbCleanup > 1)
            try_return (RC = STATUS_ACCESS_DENIED);
        ASSERT(Fcb->FcbCleanup);
        ASSERT(!Fcb->IrpContextLite);
        if (Fcb->IrpContextLite) {
            try_return (RC = STATUS_ACCESS_DENIED);
        }
        // Check if we have parallel/pending Close threads
        if (Fcb->CcbCount && !SingleDir) {
            // if this is the 1st attempt, we'll try to
            // synchronize with Close requests
            // otherwise fail request
            RC = STATUS_ACCESS_DENIED;
post_rename:
            if (Fcb->FcbState & UDF_FCB_POSTED_RENAME) {
                Fcb->FcbState &= ~UDF_FCB_POSTED_RENAME;
                try_return (RC);
            }
            Fcb->FcbState |= UDF_FCB_POSTED_RENAME;
            try_return (RC = STATUS_PENDING);
        }

        if (!TargetFileObject) {
            //  Make sure the name is of legal length.
            if (PtrBuffer->FileNameLength > UDF_NAME_LEN*sizeof(WCHAR)) {
                try_return(RC = STATUS_OBJECT_NAME_INVALID);
            }
            NewName.Length = NewName.MaximumLength = (USHORT)(PtrBuffer->FileNameLength);
            NewName.Buffer = (PWCHAR)&(PtrBuffer->FileName);
        } else {
            //  This name is by definition legal.
            NewName = *((PUNICODE_STRING)&TargetFileObject->FileName);
        }

        IgnoreCase = FlagOn(Ccb->Flags, CCB_FLAG_IGNORE_CASE);

        if (UDFIsDirOpened__(FileInfo)) {
            // We can't rename file because of unclean references.
            // UDF_INFO package can safely do it, but NT side cannot.
            // In this case NT requires STATUS_OBJECT_NAME_COLLISION
            // rather than STATUS_ACCESS_DENIED
            if (NT_SUCCESS(UDFFindFile__(Vcb, IgnoreCase, &NewName, TargetDirInfo)))
                try_return(RC = STATUS_OBJECT_NAME_COLLISION);
            try_return (RC = STATUS_ACCESS_DENIED);
        } else {
            // Last check before Moving.
            // We can't move across Dir referenced (even internally) file
            if (!SingleDir) {
                RC = UDFDoesOSAllowFileToBeMoved__(FileInfo);
                if (!NT_SUCCESS(RC)) {
//                    try_return(RC);
                    goto post_rename;
                }
            }

            ASSERT(Fcb->FcbReference >= FileInfo->RefCount);
            ASSERT(DirInfo->Fcb->FcbReference >= DirInfo->RefCount);
            ASSERT(TargetDirInfo->Fcb->FcbReference >= TargetDirInfo->RefCount);

            RC = UDFRenameMoveFile__(IrpContext, Vcb, IgnoreCase, &ReplaceIfExists, &NewName, DirInfo, TargetDirInfo, FileInfo);
        }
        if (!NT_SUCCESS(RC))
            try_return (RC);

        ASSERT(UDFDirIndex(FileInfo->ParentFile->Dloc->DirIndex, FileInfo->Index)->FileInfo == FileInfo);

        RC = MyCloneUnicodeString(&LocalPath, (TargetDirInfo->Fcb->FcbState & UDF_FCB_ROOT_DIRECTORY) ?
                                                    &UdfData.UnicodeStrRoot :
                                                    &TargetDirInfo->Fcb->FCBName->ObjectName);
        if (!NT_SUCCESS(RC)) try_return (RC);
//        RC = MyAppendUnicodeStringToString(&LocalPath, (Dir2->Fcb->FCBFlags & UDF_FCB_ROOT_DIRECTORY) ? &(UDFGlobalData.UnicodeStrRoot) : &(Dir2->Fcb->FCBName->ObjectName));
//        if (!NT_SUCCESS(RC)) try_return (RC);
        if (TargetDirInfo->ParentFile) {
            RC = MyAppendUnicodeToString(&LocalPath, L"\\");
            if (!NT_SUCCESS(RC)) try_return (RC);
        }
        RC = MyAppendUnicodeStringToStringTag(&LocalPath, &NewName, MEM_USREN_TAG);
        if (!NT_SUCCESS(RC)) try_return (RC);

        // Set Archive bit
        DirNdx = UDFDirIndex(FileInfo->ParentFile->Dloc->DirIndex, FileInfo->Index);
        if (Vcb->CompatFlags & UDF_VCB_IC_UPDATE_ARCH_BIT) {
            Attr = UDFAttributesToNT(DirNdx, FileInfo->Dloc->FileEntry);
            if (!(Attr & FILE_ATTRIBUTE_ARCHIVE))
                UDFAttributesToUDF(DirNdx, FileInfo->Dloc->FileEntry, Attr | FILE_ATTRIBUTE_ARCHIVE);
        }
        // Update Parent Objects (mark 'em as modified)
        if (Vcb->CompatFlags & UDF_VCB_IC_UPDATE_DIR_WRITE) {
            if (TargetFileObject) {
                TargetFileObject->Flags |= FO_FILE_MODIFIED;
                if (!ReplaceIfExists)
                    TargetFileObject->Flags |= FO_FILE_SIZE_CHANGED;
            }
        }
        // report changes
        if (SingleDir && !ReplaceIfExists) {
            UDFNotifyFullReportChange( Vcb, FileInfo->Fcb,
                                       UDFIsADirectory(FileInfo) ? FILE_NOTIFY_CHANGE_DIR_NAME : FILE_NOTIFY_CHANGE_FILE_NAME,
                                       FILE_ACTION_RENAMED_OLD_NAME);
/*          UDFNotifyFullReportChange( Vcb, File2,
                                       UDFIsADirectory(File2) ? FILE_NOTIFY_CHANGE_DIR_NAME : FILE_NOTIFY_CHANGE_FILE_NAME,
                                       FILE_ACTION_RENAMED_NEW_NAME );*/
            FsRtlNotifyFullReportChange( Vcb->NotifySync, &(Vcb->NextNotifyIRP),
                                         (PSTRING)&LocalPath,
                                         ((TargetDirInfo->Fcb->FcbState & UDF_FCB_ROOT_DIRECTORY) ? 0 : TargetDirInfo->Fcb->FCBName->ObjectName.Length) + sizeof(WCHAR),
                                         NULL,NULL,
                                         UDFIsADirectory(FileInfo) ? FILE_NOTIFY_CHANGE_DIR_NAME : FILE_NOTIFY_CHANGE_FILE_NAME,
                                         FILE_ACTION_RENAMED_NEW_NAME,
                                         NULL);
        } else {
            UDFNotifyFullReportChange( Vcb, FileInfo->Fcb,
                                       UDFIsADirectory(FileInfo) ? FILE_NOTIFY_CHANGE_DIR_NAME : FILE_NOTIFY_CHANGE_FILE_NAME,
                                       FILE_ACTION_REMOVED);
            if (ReplaceIfExists) {
/*              UDFNotifyFullReportChange( Vcb, File2,
                                       FILE_NOTIFY_CHANGE_ATTRIBUTES |
                                       FILE_NOTIFY_CHANGE_SIZE |
                                       FILE_NOTIFY_CHANGE_LAST_WRITE |
                                       FILE_NOTIFY_CHANGE_LAST_ACCESS |
                                       FILE_NOTIFY_CHANGE_CREATION |
                                       FILE_NOTIFY_CHANGE_EA,
                                       FILE_ACTION_MODIFIED );*/
                FsRtlNotifyFullReportChange( Vcb->NotifySync, &(Vcb->NextNotifyIRP),
                                             (PSTRING)&LocalPath,
                                             ((TargetDirInfo->Fcb->FcbState & UDF_FCB_ROOT_DIRECTORY) ?
                                                 0 : TargetDirInfo->Fcb->FCBName->ObjectName.Length) + sizeof(WCHAR),
                                             NULL,NULL,
                                             FILE_NOTIFY_CHANGE_ATTRIBUTES |
                                                 FILE_NOTIFY_CHANGE_SIZE |
                                                 FILE_NOTIFY_CHANGE_LAST_WRITE |
                                                 FILE_NOTIFY_CHANGE_LAST_ACCESS |
                                                 FILE_NOTIFY_CHANGE_CREATION |
                                                 FILE_NOTIFY_CHANGE_EA,
                                             FILE_ACTION_MODIFIED,
                                             NULL);
            } else {
/*              UDFNotifyFullReportChange( Vcb, File2,
                                       UDFIsADirectory(File2) ? FILE_NOTIFY_CHANGE_DIR_NAME : FILE_NOTIFY_CHANGE_FILE_NAME,
                                       FILE_ACTION_ADDED );*/
                FsRtlNotifyFullReportChange( Vcb->NotifySync, &(Vcb->NextNotifyIRP),
                                             (PSTRING)&LocalPath,
                                             ((TargetDirInfo->Fcb->FcbState & UDF_FCB_ROOT_DIRECTORY) ?
                                                 0 : TargetDirInfo->Fcb->FCBName->ObjectName.Length) + sizeof(WCHAR),
                                             NULL,NULL,
                                             UDFIsADirectory(FileInfo) ?
                                                 FILE_NOTIFY_CHANGE_DIR_NAME :
                                                 FILE_NOTIFY_CHANGE_FILE_NAME,
                                             FILE_ACTION_ADDED,
                                             NULL);
            }
        }

        // this will prevent structutre release before call to
        // UDFCleanUpFcbChain()
        InterlockedIncrement((PLONG)&DirInfo->Fcb->FcbReference);
        ASSERT(DirInfo->Fcb->FcbReference >= DirInfo->RefCount);

        // Look through Ccb list & decrement OpenHandleCounter(s)
        // acquire CcbList
        if (!SingleDir) {
            UDFAcquireResourceExclusive(&Fcb->FcbNonpaged->CcbListResource, TRUE);
            Link = Fcb->NextCCB.Flink;
            DirRefCount = 0;
            FileInfoRefCount = 0;
            ASSERT(Link != &Fcb->NextCCB);
            while (Link != &Fcb->NextCCB) {
                NextFileInfo = DirInfo;
                CurCcb = CONTAINING_RECORD(Link, CCB, NextCCB);
                ASSERT(CurCcb->TreeLength);
                i = (CurCcb->TreeLength) ? (CurCcb->TreeLength - 1) : 0;
                Link = Link->Flink;
                UseClose = (CurCcb->Flags & UDF_CCB_CLEANED) ? FALSE : TRUE;

                AdPrint(("  Ccb:%x:%s:i:%x\n", CurCcb, UseClose ? "Close" : "",i));
                // cleanup old parent chain
                for(; i && NextFileInfo; i--) {
                    // remember parent file now
                    // it will prevent us from data losses
                    // due to eventual structure release
                    fi = NextFileInfo->ParentFile;
                    if (UseClose) {
                        ASSERT(NextFileInfo->Fcb->FcbReference >= NextFileInfo->RefCount);
                        UDFCloseFile__(IrpContext, Vcb, NextFileInfo);
                    }
                    ASSERT(NextFileInfo->Fcb->FcbReference > NextFileInfo->RefCount);
                    ASSERT(NextFileInfo->Fcb->FcbReference);
                    InterlockedDecrement((PLONG)&NextFileInfo->Fcb->FcbReference);
                    ASSERT(NextFileInfo->Fcb->FcbReference >= NextFileInfo->RefCount);
                    NextFileInfo = fi;
                }

                if (CurCcb->TreeLength > 1) {
                    DirRefCount++;
                    if (UseClose)
                        FileInfoRefCount++;
                    CurCcb->TreeLength = 2;
#ifdef UDF_DBG
                } else {
                    BrutePoint();
#endif // UDF_DBG
                }
            }
            UDFReleaseResource(&Fcb->FcbNonpaged->CcbListResource);

            ASSERT(DirRefCount >= FileInfoRefCount);
            // update counters & pointers
            Fcb->ParentFcb = TargetDirInfo->Fcb;
            // move references to TargetDir
            InterlockedExchangeAdd((PLONG)&TargetDirInfo->Fcb->FcbReference, DirRefCount);
            ASSERT(TargetDirInfo->Fcb->FcbReference > TargetDirInfo->RefCount);
            UDFReferenceFileEx__(TargetDirInfo, FileInfoRefCount);
            ASSERT(TargetDirInfo->Fcb->FcbReference >= TargetDirInfo->RefCount);
        }
        ASSERT(TargetDirInfo->Fcb->FcbReference >= TargetDirInfo->RefCount);
        ASSERT(TargetDirInfo->RefCount);

        ASSERT(DirInfo->Fcb->FcbReference >= DirInfo->RefCount);
        // Modify name in Fcb1
        if (Fcb->FCBName) {
            if (Fcb->FCBName->ObjectName.Buffer) {
                MyFreePool__(Fcb->FCBName->ObjectName.Buffer);
            }
            UDFReleaseObjectName(Fcb->FCBName);
        }
        Fcb->FCBName = UDFAllocateObjectName();
        if (!(Fcb->FCBName)) {
insuf_res:
            BrutePoint();
            // UDFCleanUpFcbChain()...
            if (TargetParentFcbAcquired) {
                UDF_CHECK_PAGING_IO_RESOURCE(TargetDirInfo->Fcb);
                UDFReleaseResource(&TargetDirInfo->Fcb->FcbNonpaged->FcbResource);
                TargetParentFcbAcquired = FALSE;
            }
            if (ParentFcbAcquired) {
                UDF_CHECK_PAGING_IO_RESOURCE(DirInfo->Fcb);
                UDFReleaseResource(&DirInfo->Fcb->FcbNonpaged->FcbResource);
                ParentFcbAcquired = FALSE;
            }
            UDFTeardownStructures(IrpContext, DirInfo->Fcb, 1, NULL);
            try_return(RC = STATUS_INSUFFICIENT_RESOURCES);
        }

        RC = MyCloneUnicodeString(&Fcb->FCBName->ObjectName, &TargetFcb->FCBName->ObjectName);
        if (!NT_SUCCESS(RC))
            goto insuf_res;
/*        RC = MyAppendUnicodeStringToString(&(Fcb1->FCBName->ObjectName), &(Fcb2->FCBName->ObjectName));
        if (!NT_SUCCESS(RC))
            goto insuf_res;*/
        // if Dir2 is a RootDir, we shoud not append '\\' because
        // uit will be the 2nd '\\' character (RootDir's name is also '\\')
        if (TargetDirInfo->ParentFile) {
            RC = MyAppendUnicodeToString(&Fcb->FCBName->ObjectName, L"\\");
            if (!NT_SUCCESS(RC))
                goto insuf_res;
        }
        RC = MyAppendUnicodeStringToStringTag(&Fcb->FCBName->ObjectName, &NewName, MEM_USREN2_TAG);
        if (!NT_SUCCESS(RC))
            goto insuf_res;

        ASSERT(Fcb->FcbReference >= FileInfo->RefCount);
        ASSERT(DirInfo->Fcb->FcbReference >= DirInfo->RefCount);
        ASSERT(TargetDirInfo->Fcb->FcbReference >= TargetDirInfo->RefCount);

        RC = STATUS_SUCCESS;

try_exit:    NOTHING;

    } _SEH2_FINALLY {

        if (TargetParentFcbAcquired) {
            UDF_CHECK_PAGING_IO_RESOURCE(TargetDirInfo->Fcb);
            UDFReleaseResource(&TargetDirInfo->Fcb->FcbNonpaged->FcbResource);
        }
        if (ParentFcbAcquired) {
            UDF_CHECK_PAGING_IO_RESOURCE(DirInfo->Fcb);
            UDFReleaseResource(&DirInfo->Fcb->FcbNonpaged->FcbResource);
        }
        // perform protected structure release
        if (NT_SUCCESS(RC) &&
           (RC != STATUS_PENDING)) {

            UDFTeardownStructures(IrpContext, DirInfo->Fcb, 1, NULL);
            ASSERT(Fcb->FcbReference >= FileInfo->RefCount);
            ASSERT(TargetDirInfo->Fcb->FcbReference >= TargetDirInfo->RefCount);
        }

        if (LocalPath.Buffer) {
            MyFreePool__(LocalPath.Buffer);
        }
    } _SEH2_END;

    return RC;
} // end UDFRename()

LONG
UDFFindFileId(
    IN PVCB Vcb,
    IN FILE_ID Id
    )
{
    if (!Vcb->FileIdCache) return (-1);
    for(ULONG i=0; i<Vcb->FileIdCount; i++) {
        if (Vcb->FileIdCache[i].Id.QuadPart == Id.QuadPart) return i;
    }
    return (-1);
} // end UDFFindFileId()

LONG
UDFFindFreeFileId(
    IN PVCB Vcb,
    IN FILE_ID FileId
    )
{
    if (!Vcb->FileIdCache) {
        if (!(Vcb->FileIdCache = (PUDFFileIDCacheItem)MyAllocatePool__(NonPagedPool, sizeof(UDFFileIDCacheItem)*FILE_ID_CACHE_GRANULARITY)))
            return (-1);
        RtlZeroMemory(Vcb->FileIdCache, FILE_ID_CACHE_GRANULARITY*sizeof(UDFFileIDCacheItem));
        Vcb->FileIdCount = FILE_ID_CACHE_GRANULARITY;
    }
    for(ULONG i=0; i<Vcb->FileIdCount; i++) {
        if (!Vcb->FileIdCache[i].FullName.Buffer) return i;
    }
    if (!MyReallocPool__((PCHAR)(Vcb->FileIdCache), Vcb->FileIdCount*sizeof(UDFFileIDCacheItem),
                     (PCHAR*)&(Vcb->FileIdCache), (Vcb->FileIdCount+FILE_ID_CACHE_GRANULARITY)*sizeof(UDFFileIDCacheItem))) {
        return (-1);
    }
    RtlZeroMemory(&(Vcb->FileIdCache[Vcb->FileIdCount]), FILE_ID_CACHE_GRANULARITY*sizeof(UDFFileIDCacheItem));
    Vcb->FileIdCount += FILE_ID_CACHE_GRANULARITY;
    return (Vcb->FileIdCount - FILE_ID_CACHE_GRANULARITY);
} // end UDFFindFreeFileId()

NTSTATUS
UDFStoreFileId(
    IN PVCB Vcb,
    IN PCCB Ccb,
    IN PUDF_FILE_INFO fi,
    IN FILE_ID FileId
    )
{
    LONG i;
    NTSTATUS RC = STATUS_SUCCESS;

    if ((i = UDFFindFileId(Vcb, FileId)) == (-1)) {
        if ((i = UDFFindFreeFileId(Vcb, FileId)) == (-1)) return STATUS_INSUFFICIENT_RESOURCES;
    } else {
        return STATUS_SUCCESS;
    }
    Vcb->FileIdCache[i].Id = FileId;
    Vcb->FileIdCache[i].IgnoreCase = BooleanFlagOn(Ccb->Flags, CCB_FLAG_IGNORE_CASE);
    RC = MyCloneUnicodeString(&(Vcb->FileIdCache[i].FullName), &(Ccb->Fcb->FCBName->ObjectName));
/*    if (NT_SUCCESS(RC)) {
        RC = MyAppendUnicodeStringToStringTag(&(Vcb->FileIdCache[i].FullName), &(Ccb->Fcb->FCBName->ObjectName), MEM_USFIDC_TAG);
    }*/
    return RC;
} // end UDFStoreFileId()

NTSTATUS
UDFRemoveFileId(
    IN PVCB Vcb,
    IN FILE_ID FileId
    )
{
    LONG i;

    if ((i = UDFFindFileId(Vcb, FileId)) == (-1)) return STATUS_INVALID_PARAMETER;
    MyFreePool__(Vcb->FileIdCache[i].FullName.Buffer);
    RtlZeroMemory(&(Vcb->FileIdCache[i]), sizeof(UDFFileIDCacheItem));
    return STATUS_SUCCESS;
} // end UDFRemoveFileId()

VOID
UDFReleaseFileIdCache(
    IN PVCB Vcb
    )
{
    if (!Vcb->FileIdCache) return;
    for(ULONG i=0; i<Vcb->FileIdCount; i++) {
        if (Vcb->FileIdCache[i].FullName.Buffer) {
            MyFreePool__(Vcb->FileIdCache[i].FullName.Buffer);
        }
    }
    MyFreePool__(Vcb->FileIdCache);
    Vcb->FileIdCache = NULL;
    Vcb->FileIdCount = 0;
} // end UDFReleaseFileIdCache()

NTSTATUS
UDFGetOpenParamsByFileId(
    IN PVCB Vcb,
    IN FILE_ID FileId,
    OUT PUNICODE_STRING* FName,
    OUT BOOLEAN* IgnoreCase
    )
{
    LONG i;

    if ((i = UDFFindFileId(Vcb, FileId)) == (-1)) return STATUS_NOT_FOUND;
    (*FName) = &(Vcb->FileIdCache[i].FullName);
    (*IgnoreCase) = (Vcb->FileIdCache[i].IgnoreCase);
    return STATUS_SUCCESS;
} // end UDFGetOpenParamsByFileId()

#ifdef UDF_ALLOW_HARD_LINKS
/*
    create hard link for the file
 */
NTSTATUS
UDFHardLink(
    IN PIRP_CONTEXT IrpContext,
    IN PIO_STACK_LOCATION IrpSp,
    IN PFCB Fcb1,
    IN PCCB Ccb1,
    IN PFILE_OBJECT FileObject1,   // Source File
    IN PFILE_LINK_INFORMATION PtrBuffer
    )
{
    // Target Directory
    PFILE_OBJECT DirObject2 = IrpSp->Parameters.SetFile.FileObject;
    // Overwite Flag
    BOOLEAN Replace = IrpSp->Parameters.SetFile.ReplaceIfExists &&
                      PtrBuffer->ReplaceIfExists;
    NTSTATUS RC;
    PVCB Vcb = Fcb1->Vcb;
    PFCB Fcb2;
    BOOLEAN IgnoreCase;
    BOOLEAN AcquiredDir1 = FALSE;
    BOOLEAN AcquiredFcb1 = FALSE;
    BOOLEAN SingleDir = TRUE;

    PUDF_FILE_INFO File1;
    PUDF_FILE_INFO Dir1 = NULL;
    PUDF_FILE_INFO Dir2;

    UNICODE_STRING NewName;
    UNICODE_STRING LocalPath;
//    PtrUDFCCB CurCcb = NULL;

    AdPrint(("UDFHardLink\n"));

    LocalPath.Buffer = NULL;

    _SEH2_TRY {

        // do we try to link Volume ?
        if (!(File1 = Fcb1->FileInfo))
            try_return (RC = STATUS_ACCESS_DENIED);

        // do we try to link RootDir ?
        if (!(Dir1 = File1->ParentFile))
            try_return (RC = STATUS_ACCESS_DENIED);

        // do we try to link Stream / Stream Dir ?
#ifdef UDF_ALLOW_LINKS_TO_STREAMS
        if (UDFIsAStreamDir(File1))
            try_return (RC = STATUS_ACCESS_DENIED);
#else //UDF_ALLOW_LINKS_TO_STREAMS
        if (UDFIsAStream(File1) || UDFIsAStreamDir(File1) /*||
           UDFIsADirectory(File1) || UDFHasAStreamDir(File1)*/)
            try_return (RC = STATUS_ACCESS_DENIED);
#endif // UDF_ALLOW_LINKS_TO_STREAMS

        // do we try to link to RootDir or Volume ?
        if (!DirObject2) {
            Dir2 = File1->ParentFile;
            DirObject2 = FileObject1->RelatedFileObject;
        } else
        if (DirObject2->FsContext2 &&
          (Fcb2 = ((PCCB)(DirObject2->FsContext2))->Fcb)) {
            Dir2 = ((PCCB)(DirObject2->FsContext2))->Fcb->FileInfo;
        } else {
            try_return (RC = STATUS_INVALID_PARAMETER);
        }

        // check target dir
        if (!Dir2) try_return (RC = STATUS_ACCESS_DENIED);

        // Stream can't be a Dir or have Streams
        if (UDFIsAStreamDir(Dir2)) {
            try_return (RC = STATUS_ACCESS_DENIED);
/*            if (UDFIsADirectory(File1) ||
               UDFHasAStreamDir(File1)) {
                BrutePoint();
                try_return (RC = STATUS_ACCESS_DENIED);
            }*/
        }

/*        if (UDFIsAStreamDir(Dir2))
            try_return (RC = STATUS_ACCESS_DENIED);*/

        RC = UDFPrepareForRenameMoveLink(Vcb,
                                         &SingleDir,
                                         &AcquiredDir1, &AcquiredFcb1,
                                         Ccb1, File1,
                                         Dir1, Dir2,
                                         TRUE); // it is HLink operation
        if (!NT_SUCCESS(RC))
            try_return(RC);

        // check if the source file is used
        if (!DirObject2) {
            //  Make sure the name is of legal length.
            if (PtrBuffer->FileNameLength > UDF_NAME_LEN*sizeof(WCHAR)) {
                try_return(RC = STATUS_OBJECT_NAME_INVALID);
            }
            NewName.Length = NewName.MaximumLength = (USHORT)(PtrBuffer->FileNameLength);
            NewName.Buffer = (PWCHAR)&(PtrBuffer->FileName);
        } else {
            //  This name is by definition legal.
            NewName = *((PUNICODE_STRING)&DirObject2->FileName);
        }

        IgnoreCase = FlagOn(Ccb1->Flags, CCB_FLAG_IGNORE_CASE);

        AdPrint(("  %ws ->\n    %ws\n",
            Fcb1->FCBName->ObjectName.Buffer,
            NewName.Buffer));

        RC = UDFHardLinkFile__(IrpContext, Vcb, IgnoreCase, &Replace, &NewName, Dir1, Dir2, File1);
        if (!NT_SUCCESS(RC)) try_return (RC);

        // Update Parent Objects (mark 'em as modified)
        if (Vcb->CompatFlags & UDF_VCB_IC_UPDATE_DIR_WRITE) {
            if (DirObject2) {
                DirObject2->Flags |= FO_FILE_MODIFIED;
                if (!Replace)
                    DirObject2->Flags |= FO_FILE_SIZE_CHANGED;
            }
        }
        // report changes
        UDFNotifyFullReportChange( Vcb, File1->Fcb,
                                   FILE_NOTIFY_CHANGE_LAST_WRITE |
                                   FILE_NOTIFY_CHANGE_LAST_ACCESS,
                                   FILE_ACTION_MODIFIED );

        RC = MyCloneUnicodeString(&LocalPath, (Dir2->Fcb->FcbState & UDF_FCB_ROOT_DIRECTORY) ?
                                                    &UdfData.UnicodeStrRoot :
                                                    &(Dir2->Fcb->FCBName->ObjectName));
        if (!NT_SUCCESS(RC)) try_return (RC);
/*        RC = MyAppendUnicodeStringToString(&LocalPath, (Dir2->Fcb->FCBFlags & UDF_FCB_ROOT_DIRECTORY) ? &(UDFGlobalData.UnicodeStrRoot) : &(Dir2->Fcb->FCBName->ObjectName));
        if (!NT_SUCCESS(RC)) try_return (RC);*/
        // if Dir2 is a RootDir, we shoud not append '\\' because
        // it will be the 2nd '\\' character (RootDir's name is also '\\')
        if (Dir2->ParentFile) {
            RC = MyAppendUnicodeToString(&LocalPath, L"\\");
            if (!NT_SUCCESS(RC)) try_return (RC);
        }
        RC = MyAppendUnicodeStringToStringTag(&LocalPath, &NewName, MEM_USHL_TAG);
        if (!NT_SUCCESS(RC)) try_return (RC);

        if (!Replace) {
/*          UDFNotifyFullReportChange( Vcb, File2,
                                       UDFIsADirectory(File1) ? FILE_NOTIFY_CHANGE_DIR_NAME : FILE_NOTIFY_CHANGE_FILE_NAME,
                                       FILE_ACTION_ADDED );*/
            FsRtlNotifyFullReportChange( Vcb->NotifySync, &(Vcb->NextNotifyIRP),
                                         (PSTRING)&LocalPath,
                                         ((Dir2->Fcb->FcbState & UDF_FCB_ROOT_DIRECTORY) ? 0 : Dir2->Fcb->FCBName->ObjectName.Length) + sizeof(WCHAR),
                                         NULL,NULL,
                                         UDFIsADirectory(File1) ? FILE_NOTIFY_CHANGE_DIR_NAME : FILE_NOTIFY_CHANGE_FILE_NAME,
                                         FILE_ACTION_ADDED,
                                         NULL);
        } else {
/*          UDFNotifyFullReportChange( Vcb, File2,
                                       FILE_NOTIFY_CHANGE_ATTRIBUTES |
                                       FILE_NOTIFY_CHANGE_SIZE |
                                       FILE_NOTIFY_CHANGE_LAST_WRITE |
                                       FILE_NOTIFY_CHANGE_LAST_ACCESS |
                                       FILE_NOTIFY_CHANGE_CREATION |
                                       FILE_NOTIFY_CHANGE_EA,
                                       FILE_ACTION_MODIFIED );*/
            FsRtlNotifyFullReportChange( Vcb->NotifySync, &(Vcb->NextNotifyIRP),
                                         (PSTRING)&LocalPath,
                                         ((Dir2->Fcb->FcbState & UDF_FCB_ROOT_DIRECTORY) ? 0 : Dir2->Fcb->FCBName->ObjectName.Length) + sizeof(WCHAR),
                                         NULL,NULL,
                                         UDFIsADirectory(File1) ? FILE_NOTIFY_CHANGE_DIR_NAME : FILE_NOTIFY_CHANGE_FILE_NAME,
                                         FILE_NOTIFY_CHANGE_ATTRIBUTES |
                                             FILE_NOTIFY_CHANGE_SIZE |
                                             FILE_NOTIFY_CHANGE_LAST_WRITE |
                                             FILE_NOTIFY_CHANGE_LAST_ACCESS |
                                             FILE_NOTIFY_CHANGE_CREATION |
                                             FILE_NOTIFY_CHANGE_EA,
                                         NULL);
        }

        RC = STATUS_SUCCESS;

try_exit:    NOTHING;

    } _SEH2_FINALLY {

        if (AcquiredFcb1) {
            UDF_CHECK_PAGING_IO_RESOURCE(Fcb1);
            UDFReleaseResource(&Fcb1->FcbNonpaged->FcbResource);
        }
        if (AcquiredDir1) {
            UDF_CHECK_PAGING_IO_RESOURCE(Dir1->Fcb);
            UDFReleaseResource(&Dir1->Fcb->FcbNonpaged->FcbResource);
        }

        if (LocalPath.Buffer) {
            MyFreePool__(LocalPath.Buffer);
        }
    } _SEH2_END;

    return RC;
} // end UDFHardLink()
#endif //UDF_ALLOW_HARD_LINKS
