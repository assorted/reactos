////////////////////////////////////////////////////////////////////
// Copyright (C) Alexander Telyatnikov, Ivan Keliukh, Yegor Anchishkin, SKIF Software, 1999-2013. Kiev, Ukraine
// All rights reserved
// This file was released under the GPLv2 on June 2015.
////////////////////////////////////////////////////////////////////
/*************************************************************************
*
* File: DirCntrl.cpp
*
* Module: UDF File System Driver (Kernel mode execution only)
*
* Description:
*   Contains code to handle the "directory control" dispatch entry point.
*
*************************************************************************/

#include            "udffs.h"

// define the file specific bug-check id
#define         UDF_BUG_CHECK_ID                UDF_FILE_DIR_CONTROL

/*
// Local support routine(s):
*/

#define UDF_FNM_FLAG_CAN_BE_8D3    0x01
#define UDF_FNM_FLAG_IGNORE_CASE   0x02
#define UDF_FNM_FLAG_CONTAINS_WC   0x04

NTSTATUS
UDFFindNextMatch(
    IN PVCB            Vcb,
    IN PDIR_INDEX_HDR  hDirIndex,
    IN PLONG           CurrentNumber,      // Must be modified
    IN PUNICODE_STRING SearchPattern,
    IN ULONG           CcbFlags,
    IN PHASH_ENTRY     hashes,
   OUT PDIR_INDEX_ITEM* _DirNdx);

/*************************************************************************
*
* Function: UDFCommonDirControl()
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
NTAPI
UDFCommonDirControl(
   PIRP_CONTEXT IrpContext,
   PIRP              Irp
   )
{
    NTSTATUS                RC = STATUS_SUCCESS;
    PIO_STACK_LOCATION      IrpSp;
    PFILE_OBJECT            FileObject = NULL;
    PFCB                    Fcb = NULL;
    PCCB                    Ccb = NULL;
    PVCB                    Vcb = NULL;

    PAGED_CODE();

    TmPrint(("UDFCommonDirControl: \n"));

    // Decode the user file object and fail this request if it is not
    // a user directory.

    IrpSp = IoGetCurrentIrpStackLocation(Irp);

    if (UDFDecodeFileObject(IrpSp->FileObject, &Fcb, &Ccb) != UserDirectoryOpen) {

        UDFCompleteRequest( IrpContext, Irp, STATUS_INVALID_PARAMETER );
        return STATUS_INVALID_PARAMETER;
    }

    Vcb = Fcb->Vcb;

    ASSERT_CCB(Ccb);
    ASSERT_FCB(Fcb);
    ASSERT_VCB(Vcb);

    // Validate the sent-in FCB
    if ((Fcb == Fcb->Vcb->VolumeDasdFcb) ||
        !(Fcb->FcbState & UDF_FCB_DIRECTORY)) {

        UDFCompleteRequest(IrpContext, Irp, STATUS_INVALID_PARAMETER);
        return STATUS_INVALID_PARAMETER;
    }

    UDFFlushTryBreak(Vcb);

    // Get some of the parameters supplied to us
    switch (IrpSp->MinorFunction) {
    case IRP_MN_QUERY_DIRECTORY:

        RC = UDFQueryDirectory(IrpContext, Irp, IrpSp, FileObject, Fcb, Ccb);
        break;
    case IRP_MN_NOTIFY_CHANGE_DIRECTORY:

        RC = UDFNotifyChangeDirectory(IrpContext, Irp, IrpSp, FileObject, Fcb, Ccb);
        break;
    default:

        UDFCompleteRequest(IrpContext, Irp, STATUS_INVALID_DEVICE_REQUEST);
        RC = STATUS_INVALID_DEVICE_REQUEST;
        break;
    }

    return(RC);
} // end UDFCommonDirControl()


/*************************************************************************
*
* Function: UDFQueryDirectory()
*
* Description:
*   Query directory request.
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
UDFQueryDirectory(
    PIRP_CONTEXT IrpContext,
    PIRP                        Irp,
    PIO_STACK_LOCATION          IrpSp,
    PFILE_OBJECT                FileObject,
    PFCB                        Fcb,
    PCCB                        Ccb
    )
{
    NTSTATUS                    RC = STATUS_SUCCESS;
    PVCB Vcb = NULL;
    unsigned long               BufferLength = 0;
    UNICODE_STRING              SearchPattern;
    PUNICODE_STRING             PtrSearchPattern;
    FILE_INFORMATION_CLASS      FileInformationClass;
    BOOLEAN                     ReturnSingleEntry = FALSE;
    PUCHAR                      Buffer = NULL;
    BOOLEAN                     FirstTimeQuery = FALSE;
    LONG                        NextMatch = 0;
    LONG                        PrevMatch = -1;
    ULONG                       CurrentOffset;
    ULONG                       BaseLength;
    ULONG                       FileNameBytes;
    ULONG                       Information = 0;
    ULONG                       LastOffset = 0;
    BOOLEAN                     AtLeastOneFound = FALSE;
    PUDF_FILE_INFO              DirFileInfo = NULL;
    PDIR_INDEX_HDR              hDirIndex = NULL;
    UCHAR                       DirInformationBuffer[sizeof(FILE_BOTH_DIR_INFORMATION) + UDF_NAME_LEN * sizeof(WCHAR)];
    PFILE_BOTH_DIR_INFORMATION  DirInformation = (PFILE_BOTH_DIR_INFORMATION)DirInformationBuffer;  // Returned from udf_info module
    PFILE_BOTH_DIR_INFORMATION  BothDirInformation = NULL;  // Pointer in callers buffer
    PFILE_NAMES_INFORMATION     NamesInfo;
    PFILE_ID_BOTH_DIR_INFORMATION IdBothDirInfo = NULL;
    ULONG                       BytesRemainingInBuffer;
    PHASH_ENTRY                 cur_hashes = NULL;
    PDIR_INDEX_ITEM             DirNdx;
    // do some pre-init...
    SearchPattern.Buffer = NULL;

    UDFPrint(("UDFQueryDirectory: @=%#x\n", &IrpContext));

#define CanBe8dot3    (FNM_Flags & UDF_FNM_FLAG_CAN_BE_8D3)
#define IgnoreCase    (FNM_Flags & UDF_FNM_FLAG_IGNORE_CASE)
#define ContainsWC    (FNM_Flags & UDF_FNM_FLAG_CONTAINS_WC)

    Vcb = Fcb->Vcb;
    ASSERT_VCB(Vcb);

    FileInformationClass = IrpSp->Parameters.QueryDirectory.FileInformationClass;

    // Check if we support this search mode.  Also remember the size of the base part of
    // each of these structures.

    switch (FileInformationClass) {

    case FileDirectoryInformation:
        BaseLength = FIELD_OFFSET(FILE_DIRECTORY_INFORMATION, FileName[0]);
        break;
    case FileFullDirectoryInformation:
        BaseLength = FIELD_OFFSET(FILE_FULL_DIR_INFORMATION, FileName[0]);
        break;
    case FileNamesInformation:
        BaseLength = FIELD_OFFSET(FILE_NAMES_INFORMATION, FileName[0]);
        break;
    case FileBothDirectoryInformation:
        BaseLength = FIELD_OFFSET(FILE_BOTH_DIR_INFORMATION, FileName[0]);
        break;
    case FileIdBothDirectoryInformation:
        BaseLength = FIELD_OFFSET(FILE_ID_BOTH_DIR_INFORMATION, FileName[0]);
        break;
    default:

        UDFCompleteRequest(IrpContext, Irp, STATUS_INVALID_INFO_CLASS);
        return STATUS_INVALID_INFO_CLASS;
    }

    // Acquire the directory.

    UDFAcquireFcbShared(IrpContext, Fcb, FALSE);

    _SEH2_TRY
    {
        DirFileInfo = Fcb->FileInfo;
        BufferLength = IrpSp->Parameters.QueryDirectory.Length;

        // Continue obtaining the callers parameters...
        if (FlagOn(Ccb->Flags, CCB_FLAG_IGNORE_CASE) && IrpSp->Parameters.QueryDirectory.FileName) {
            PtrSearchPattern = &SearchPattern;
            if (!NT_SUCCESS(RC = RtlUpcaseUnicodeString(PtrSearchPattern, (PUNICODE_STRING)(IrpSp->Parameters.QueryDirectory.FileName), TRUE)))
                try_return(RC);
        } else {
            PtrSearchPattern = (PUNICODE_STRING)(IrpSp->Parameters.QueryDirectory.FileName);
        }

        // Some additional arguments that affect the FSD behavior
        ReturnSingleEntry = (IrpSp->Flags & SL_RETURN_SINGLE_ENTRY) ? TRUE : FALSE;

        // We must determine the buffer pointer to be used. Since this
        // routine could either be invoked directly in the context of the
        // calling thread, or in the context of a worker thread, here is
        // a general way of determining what we should use.
        if (Irp->MdlAddress) {
            Buffer = (PUCHAR) MmGetSystemAddressForMdlSafe(Irp->MdlAddress, NormalPagePriority);
            if (!Buffer)
                try_return(RC = STATUS_INSUFFICIENT_RESOURCES);
        } else {
            Buffer = (PUCHAR) Irp->UserBuffer;
            if (!Buffer)
                try_return(RC = STATUS_INVALID_USER_BUFFER);
        }

        // The method of determining where to look from and what to look for is
        // unfortunately extremely confusing. However, here is a methodology
        // we broadly adopt:
        // (a) We have to maintain a search buffer per CCB structure.
        // (b) This search buffer is initialized the very first time
        //       a query directory operation is performed using the file object.
        // (For the UDF FSD, the search buffer is stored in the
        //   DirectorySearchPattern field)
        // However, the caller still has the option of "overriding" this stored
        // search pattern by supplying a new one in a query directory operation.
        if (PtrSearchPattern &&
           PtrSearchPattern->Buffer &&
           !(PtrSearchPattern->Buffer[PtrSearchPattern->Length/sizeof(WCHAR) - 1])) {
            PtrSearchPattern->Length -= sizeof(WCHAR);
        }

        if (IrpSp->Flags & SL_INDEX_SPECIFIED) {
            // Good idea from M$: we should continue search from NEXT item
            // when FileIndex specified...
            // Strange idea from M$: we should do it with EMPTY pattern...
            PtrSearchPattern = NULL;
            Ccb->Flags |= UDF_CCB_MATCH_ALL;
        } else if (PtrSearchPattern &&
                  PtrSearchPattern->Buffer &&
                  !UDFIsMatchAllMask(PtrSearchPattern, NULL) ) {

            Ccb->Flags &= ~(UDF_CCB_MATCH_ALL |
                               UDF_CCB_WILDCARD_PRESENT |
                               UDF_CCB_CAN_BE_8_DOT_3);
            // Once we have validated the search pattern, we must
            // check whether we need to store this search pattern in
            // the CCB.
            if (Ccb->DirectorySearchPattern) {
                MyFreePool__(Ccb->DirectorySearchPattern->Buffer);
                MyFreePool__(Ccb->DirectorySearchPattern);
                Ccb->DirectorySearchPattern = NULL;
            }
            // This must be the very first query request.
            FirstTimeQuery = TRUE;

            // Now, allocate enough memory to contain the caller
            // supplied search pattern and fill in the DirectorySearchPattern
            // field in the CCB
            Ccb->DirectorySearchPattern = (PUNICODE_STRING)MyAllocatePool__(NonPagedPool,sizeof(UNICODE_STRING));
            if (!(Ccb->DirectorySearchPattern)) {
                try_return(RC = STATUS_INSUFFICIENT_RESOURCES);
            }
            Ccb->DirectorySearchPattern->Length = PtrSearchPattern->Length;
            Ccb->DirectorySearchPattern->MaximumLength = PtrSearchPattern->MaximumLength;
            Ccb->DirectorySearchPattern->Buffer = (PWCHAR)MyAllocatePool__(NonPagedPool,PtrSearchPattern->MaximumLength);
            if (!(Ccb->DirectorySearchPattern->Buffer)) {
                try_return(RC = STATUS_INSUFFICIENT_RESOURCES);
            }
            RtlCopyMemory(Ccb->DirectorySearchPattern->Buffer,PtrSearchPattern->Buffer,
                          PtrSearchPattern->MaximumLength);
            if (FsRtlDoesNameContainWildCards(PtrSearchPattern)) {
                Ccb->Flags |= UDF_CCB_WILDCARD_PRESENT;
            } else {
                UDFBuildHashEntry(Vcb, PtrSearchPattern, cur_hashes = &(Ccb->hashes), HASH_POSIX | HASH_ULFN);
            }
            if (UDFCanNameBeA8dot3(PtrSearchPattern))
                Ccb->Flags |= UDF_CCB_CAN_BE_8_DOT_3;

        } else if (!Ccb->DirectorySearchPattern &&
                  !(Ccb->Flags & UDF_CCB_MATCH_ALL) ) {

            // If the filename is not specified or is a single '*' then we will
            // match all names.
            FirstTimeQuery = TRUE;
            PtrSearchPattern = NULL;
            Ccb->Flags |= UDF_CCB_MATCH_ALL;

        } else {
            // The caller has not supplied any search pattern that we are
            // forced to use. However, the caller had previously supplied
            // a pattern (or we must have invented one) and we will use it.
            // This is definitely not the first query operation on this
            // directory using this particular file object.
            if (Ccb->Flags & UDF_CCB_MATCH_ALL) {
                PtrSearchPattern = NULL;
/*                if (Ccb->CurrentIndex)
                    Ccb->CurrentIndex++;*/
            } else {
                PtrSearchPattern = Ccb->DirectorySearchPattern;
                if (!(Ccb->Flags & UDF_CCB_WILDCARD_PRESENT)) {
                    cur_hashes = &(Ccb->hashes);
                }
            }
        }

        if (IrpSp->Flags & SL_INDEX_SPECIFIED) {
            // Caller has told us wherefrom to begin.
            // We may need to round this to an appropriate directory entry
            // entry alignment value.
            NextMatch = IrpSp->Parameters.QueryDirectory.FileIndex;
        } else if (IrpSp->Flags & SL_RESTART_SCAN) {
            NextMatch = 0;
        } else {
            // Get the starting offset from the CCB.
            // Remember to update this value on our way out from this function.
            // But, do not update the CCB CurrentByteOffset field if our reach
            // the end of the directory (or get an error reading the directory)
            // while performing the search.
            NextMatch = Ccb->CurrentIndex; // Last good index
        }

        // This is an additional verifying
        if (!UDFIsADirectory(DirFileInfo)) {
            try_return(RC = STATUS_INVALID_PARAMETER);
        }

        hDirIndex = DirFileInfo->Dloc->DirIndex;
        if (!hDirIndex) {
            try_return(RC = STATUS_INVALID_PARAMETER);
        }

        RC = STATUS_SUCCESS;
        // Buffer for DirInformation is stack-allocated above
        CurrentOffset=0;
        BytesRemainingInBuffer = IrpSp->Parameters.QueryDirectory.Length;
        RtlZeroMemory(Buffer,BytesRemainingInBuffer);

        if ((!FirstTimeQuery) && !UDFDirIndex(hDirIndex, (uint_di)NextMatch) ) {
            try_return( RC = STATUS_NO_MORE_FILES);
        }

        // One final note though:
        // If we do not find a directory entry OR while searching we reach the
        // end of the directory, then the return code should be set as follows:

        // (a) If any files have been returned (i.e. ReturnSingleEntry was FALSE
        //       and we did find at least one match), then return STATUS_SUCCESS
        // (b) If no entry is being returned then:
        //       (i) If this is the first query i.e. FirstTimeQuery is TRUE
        //            then return STATUS_NO_SUCH_FILE
        //       (ii) Otherwise, return STATUS_NO_MORE_FILES

        while(TRUE) {
            // If the user had requested only a single match and we have
            // returned that, then we stop at this point.
            if (ReturnSingleEntry && AtLeastOneFound) {
                try_return(RC);
            }
            // We call UDFFindNextMatch to look down the next matching dirent.
            RC = UDFFindNextMatch(Vcb, hDirIndex,&NextMatch,PtrSearchPattern, Ccb->Flags, cur_hashes, &DirNdx);
            // If we didn't receive next match, then we are at the end of the
            // directory.  If we have returned any files, we exit with
            // success, otherwise we return STATUS_NO_MORE_FILES.
            if (!NT_SUCCESS(RC)) {
                RC = AtLeastOneFound ? STATUS_SUCCESS :
                                      (FirstTimeQuery ? STATUS_NO_SUCH_FILE : STATUS_NO_MORE_FILES);
                try_return(RC);
            }
            // We found at least one matching file entry
            AtLeastOneFound = TRUE;
            if (!NT_SUCCESS(RC = UDFFileDirInfoToNT(IrpContext, Vcb, DirNdx, DirInformation))) {
                // this happends when we can't allocate tmp buffers
                try_return(RC);
            }
            DirInformation->FileIndex = NextMatch;
            FileNameBytes = DirInformation->FileNameLength;

            if ((BaseLength + FileNameBytes) > BytesRemainingInBuffer) {
                // If this won't fit and we have returned a previous entry then just
                // return STATUS_SUCCESS. Otherwise
                // use a status code of STATUS_BUFFER_OVERFLOW.
                if (CurrentOffset) {
                    try_return(RC = STATUS_SUCCESS);
                }
                // strange policy...
                ReturnSingleEntry = TRUE;
                FileNameBytes = BaseLength + FileNameBytes - BytesRemainingInBuffer;
                RC = STATUS_BUFFER_OVERFLOW;
            }
            //  Now we have an entry to return to our caller.
            //  We'll case on the type of information requested and fill up
            //  the user buffer if everything fits.
            switch (FileInformationClass) {

            case FileBothDirectoryInformation:
            case FileFullDirectoryInformation:
            case FileIdBothDirectoryInformation:
            case FileDirectoryInformation:

                BothDirInformation = (PFILE_BOTH_DIR_INFORMATION)(Buffer + CurrentOffset);
                RtlCopyMemory(BothDirInformation,DirInformation,BaseLength);
                BothDirInformation->FileIndex = NextMatch;
                BothDirInformation->FileNameLength = FileNameBytes;
                break;

            case FileNamesInformation:

                NamesInfo = (PFILE_NAMES_INFORMATION)(Buffer + CurrentOffset);
                NamesInfo->FileIndex = NextMatch;
                NamesInfo->FileNameLength = FileNameBytes;
                break;

            default:
                break;
            }

            switch (FileInformationClass) {

            case FileIdBothDirectoryInformation:
                IdBothDirInfo = (PFILE_ID_BOTH_DIR_INFORMATION)(Buffer + CurrentOffset);
                IdBothDirInfo->FileId = UDFGetNTFileId(Vcb, Fcb->FileInfo);
                break;

            default:
                break;
            }

            if (FileNameBytes) {
                //  This is a Unicode name, we can copy the bytes directly.
                RtlCopyMemory( (PVOID)(Buffer + CurrentOffset + BaseLength),
                               DirInformation->FileName, FileNameBytes );
            }

            Information = CurrentOffset + BaseLength + FileNameBytes;

            //  ((..._INFORMATION)(PointerToPreviousEntryInBuffer))->NextEntryOffset = CurrentOffset - LastOffset;
            *((PULONG)(Buffer+LastOffset)) = CurrentOffset - LastOffset;
            //  Set up our variables for the next dirent.
            FirstTimeQuery = FALSE;

            LastOffset    = CurrentOffset;
            PrevMatch     = NextMatch;
            NextMatch++;
            CurrentOffset = UDFQuadAlign(Information);
            BytesRemainingInBuffer = BufferLength - CurrentOffset;
        }

try_exit:   NOTHING;


    } _SEH2_FINALLY {


#ifdef UDF_DBG
        if (!NT_SUCCESS(RC)) {
            UDFPrint(("    Not found\n"));
        }
#endif // UDF_DBG

        UDFReleaseFcb(IrpContext, Fcb);

        if (!_SEH2_AbnormalTermination() && !NT_ERROR(RC)) {

            // Remember to update the CurrentByteOffset field in the CCB if required.
            Ccb->CurrentIndex = NextMatch;
        }

        if (SearchPattern.Buffer) RtlFreeUnicodeString(&SearchPattern);
    } _SEH2_END;

    Irp->IoStatus.Information = Information;

    UDFCompleteRequest(IrpContext, Irp, RC);

    return(RC);
} // end UDFQueryDirectory()

/*
  Return: STATUS_NO_SUCH_FILE if no more files found
*/
NTSTATUS
UDFFindNextMatch(
    IN PVCB Vcb,
    IN PDIR_INDEX_HDR  hDirIndex,
    IN PLONG           CurrentNumber,      // Must be modified in case, when we found next match
    IN PUNICODE_STRING SearchPattern,
    IN ULONG           CcbFlags,
    IN PHASH_ENTRY     hashes,
   OUT PDIR_INDEX_ITEM* _DirNdx
    )
{
    LONG    EntryNumber = (*CurrentNumber);
    PDIR_INDEX_ITEM DirNdx;
    for(;(DirNdx = UDFDirIndex(hDirIndex, EntryNumber));EntryNumber++) {

        if (!DirNdx->FName.Buffer ||
            UDFIsDeleted(DirNdx)) {

            continue;
        }

        if (hashes &&
           (DirNdx->hashes.hLfn != hashes->hLfn) &&
           (DirNdx->hashes.hPosix != hashes->hPosix) &&
           (!FlagOn(CcbFlags, UDF_CCB_CAN_BE_8_DOT_3) || ((DirNdx->hashes.hDos != hashes->hLfn) && (DirNdx->hashes.hDos != hashes->hPosix))) ) {

            continue;
        }

        if (UDFIsNameInExpression(Vcb,
                                  &DirNdx->FName,
                                  SearchPattern,
                                  NULL,
                                  BooleanFlagOn(CcbFlags, CCB_FLAG_IGNORE_CASE),
                                  BooleanFlagOn(CcbFlags, UDF_CCB_WILDCARD_PRESENT),
                                  BooleanFlagOn(CcbFlags, UDF_CCB_CAN_BE_8_DOT_3) && !(DirNdx->FI_Flags & UDF_FI_FLAG_DOS),
                                  EntryNumber < 2) && !(DirNdx->FI_Flags & UDF_FI_FLAG_FI_INTERNAL)) {

            break;
        }
    }

    if (DirNdx) {
        // Modify CurrentNumber to appropriate value
        *CurrentNumber = EntryNumber;
        *_DirNdx = DirNdx;
        return STATUS_SUCCESS;
    } else {
        // Do not modify CurrentNumber because we have not found next match entry
        return STATUS_NO_MORE_FILES;
    }
} // end UDFFindNextMatch()

/*************************************************************************
*
* Function: UDFNotifyChangeDirectory()
*
* Description:
*   Handle the notify request.
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
UDFNotifyChangeDirectory(
    PIRP_CONTEXT IrpContext,
    PIRP                        Irp,
    PIO_STACK_LOCATION          IrpSp,
    PFILE_OBJECT                FileObject,
    PFCB                        Fcb,
    PCCB                        Ccb
    )
{
    PVCB Vcb;

    UDFPrint(("UDFNotifyChangeDirectory\n"));

    Vcb = Fcb->Vcb;

    // Acquire the Vcb shared.
    UDFAcquireResourceShared(&Vcb->VcbResource, TRUE);

    // Acquire the FCB resource shared
    UDF_CHECK_PAGING_IO_RESOURCE(Fcb);
    UDFAcquireResourceShared(&Fcb->FcbNonpaged->FcbResource, TRUE);

    _SEH2_TRY {

        // Verify the Vcb.

        UDFVerifyVcb(IrpContext, Vcb);

        FsRtlNotifyFullChangeDirectory(
                            Vcb->NotifySync,
                            &Vcb->NextNotifyIRP,
                            (PVOID)Ccb,
                            (Fcb->FileInfo->ParentFile) ? (PSTRING)&(Fcb->FCBName->ObjectName) : (PSTRING)&(UdfData.UnicodeStrRoot),
                            BooleanFlagOn(IrpSp->Flags, SL_WATCH_TREE),
                            FALSE,
                            IrpSp->Parameters.NotifyDirectory.CompletionFilter,
                            Irp,
                            NULL,
                            NULL);

    } _SEH2_FINALLY {

        // Release the FCB resources.
        UDF_CHECK_PAGING_IO_RESOURCE(Fcb);
        UDFReleaseResource(&Fcb->FcbNonpaged->FcbResource);

        // Release the Vcb.
        UDFReleaseResource(&Vcb->VcbResource);

        if (!_SEH2_AbnormalTermination()) {

            UDFCompleteRequest(IrpContext, NULL, STATUS_SUCCESS);
        }

    } _SEH2_END;

    return STATUS_PENDING;
} // end UDFNotifyChangeDirectory()
