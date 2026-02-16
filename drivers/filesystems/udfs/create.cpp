////////////////////////////////////////////////////////////////////
// Copyright (C) Alexander Telyatnikov, Ivan Keliukh, Yegor Anchishkin, SKIF Software, 1999-2013. Kiev, Ukraine
// All rights reserved
// This file was released under the GPLv2 on June 2015.
////////////////////////////////////////////////////////////////////
/*************************************************************************
*
* File: Create.cpp
*
* Module: UDF File System Driver (Kernel mode execution only)
*
* Description:
*   Contains code to handle the "Create"/"Open" dispatch entry point.
*
*************************************************************************/

#include            "udffs.h"

#define IsFileObjectReadOnly(FO) (!((FO)->WriteAccess | (FO)->DeleteAccess))

// define the file specific bug-check id
#define         UDF_BUG_CHECK_ID                UDF_FILE_CREATE

#define         MEM_USABS_TAG                   "US_Abs"
#define         MEM_USLOC_TAG                   "US_Loc"
#define         MEM_USOBJ_TAG                   "US_Obj"

_When_(RelatedTypeOfOpen != UnopenedFileObject, _At_(RelatedCcb, _In_))
_When_(RelatedTypeOfOpen == UnopenedFileObject, _At_(RelatedCcb, _In_opt_))
_When_(RelatedTypeOfOpen != UnopenedFileObject, _At_(RelatedFileName, _In_))
_When_(RelatedTypeOfOpen == UnopenedFileObject, _At_(RelatedFileName, _In_opt_))
NTSTATUS
UDFNormalizeFileNames(
    _Inout_ PIRP_CONTEXT IrpContext,
    _In_ PVCB Vcb,
    _In_ BOOLEAN OpenByFileId,
    _In_ TYPE_OF_OPEN RelatedTypeOfOpen,
    PCCB RelatedCcb,
    PUNICODE_STRING RelatedFileName,
    _Inout_ PUNICODE_STRING FileName,
    _Inout_ PUNICODE_STRING RemainingName
);

NTSTATUS
UDFSupersedeOrOverwriteFile(
    IN PIRP_CONTEXT IrpContext,
    IN PFILE_OBJECT FileObject,
    IN PVCB Vcb,
    IN PFCB Fcb,
    IN PUDF_FILE_INFO FileInfo,
    IN LONGLONG AllocationSize,
    IN ULONG FileAttributes,
    IN BOOLEAN Supersede
    )
{
    NTSTATUS RC;
    ULONG NewFileAttributes;

    UDFAcquirePagingIoExclusive(IrpContext, Fcb);

    _SEH2_TRY {

        if (!MmCanFileBeTruncated(&Fcb->FcbNonpaged->SegmentObject, &UdfData.UDFLargeZero)) {

            AdPrint(("    Can't truncate. File is mapped\n"));
            try_return(RC = STATUS_USER_MAPPED_FILE);
        }

        // Truncate file to zero
        RC = UDFResizeFile__(IrpContext, Vcb, FileInfo, 0);

        if (!NT_SUCCESS(RC)) {

            AdPrint(("    Error during resize operation\n"));
            try_return(RC);
        }

        // Set file sizes
        Fcb->Header.AllocationSize.QuadPart = UDFSysGetAllocSize(Vcb, AllocationSize);
        Fcb->Header.FileSize.QuadPart = 0;
        Fcb->Header.ValidDataLength.QuadPart = 0;
        Fcb->FcbState &= ~UDF_FCB_DELAY_CLOSE;

        MmPrint(("    CcSetFileSizes()\n"));
        CcSetFileSizes(FileObject, (PCC_FILE_SIZES)&Fcb->Header.AllocationSize);
        Fcb->NtReqFCBFlags |= UDF_NTREQ_FCB_MODIFIED;

        // Set attributes
        NewFileAttributes = FileAttributes | FILE_ATTRIBUTE_ARCHIVE;
        if (!Supersede) {
            // For Overwrite, combine with current attributes (get from FileInfo)
            NewFileAttributes |= UDFAttributesToNT(
                UDFDirIndex(UDFGetDirIndexByFileInfo(FileInfo), FileInfo->Index),
                FileInfo->Dloc->FileEntry);
        }
        UDFAttributesToUDF(UDFDirIndex(UDFGetDirIndexByFileInfo(FileInfo), FileInfo->Index),
                           FileInfo->Dloc->FileEntry, NewFileAttributes);

try_exit: NOTHING;

    } _SEH2_FINALLY {

        UDFReleasePagingIo(IrpContext, Fcb);
    } _SEH2_END;

    return RC;
} // end UDFSupersedeOrOverwriteFile()


/*************************************************************************
*
* Function: UDFOpenExistingFcb()
*
* Description:
*   Open an existing FCB. Determines the type of open, sets CCB flags,
*   and calls UDFCompleteFcbOpen.
*
*   Opens an existing FCB with proper type detection.
*
* Expected Interrupt Level (for execution) :
*
*  IRQL_PASSIVE_LEVEL
*
* Return Value: STATUS_SUCCESS/Error
*
*************************************************************************/
NTSTATUS
UDFOpenExistingFcb(
    IN PIRP_CONTEXT IrpContext,
    IN PIO_STACK_LOCATION IrpSp,
    IN PVCB Vcb,
    IN OUT PFCB *CurrentFcb,
    IN BOOLEAN IgnoreCase,
    IN BOOLEAN OpenByFileId,
    IN ULONG CreateDisposition
    )
{
    ULONG CcbFlags = 0;
    TYPE_OF_OPEN TypeOfOpen;

    ASSERT_FCB(*CurrentFcb);

    // Determine type of open based on FCB type
    if (UDFIsADirectory((*CurrentFcb)->FileInfo)) {
        TypeOfOpen = UserDirectoryOpen;
    } else {
        TypeOfOpen = UserFileOpen;
    }

    // Set CCB flags
    if (IgnoreCase) {
        SetFlag(CcbFlags, CCB_FLAG_IGNORE_CASE);
    }

    if (OpenByFileId) {
        SetFlag(CcbFlags, CCB_FLAG_OPEN_BY_ID);
    }

    // Complete the open
    return UDFCompleteFcbOpen(IrpContext,
                              IrpSp,
                              Vcb,
                              CurrentFcb,
                              TypeOfOpen,
                              CcbFlags,
                              CreateDisposition);

} // end UDFOpenExistingFcb()


/*************************************************************************
*
* Function: UDFCommonCreate()
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
UDFCommonCreate(
    PIRP_CONTEXT IrpContext,
    PIRP                            Irp
    )
{
    NTSTATUS RC = STATUS_SUCCESS;
    PIO_STACK_LOCATION IrpSp = IoGetCurrentIrpStackLocation(Irp);
    PFILE_OBJECT FileObject = NULL;
    PFILE_OBJECT RelatedFileObject = NULL;
    LONGLONG AllocationSize;     // if we create a new file
    ULONG Options;
    ULONG CreateDisposition;
    USHORT FileAttributes;
    USHORT TmpFileAttributes;
    USHORT ShareAccess;
    ACCESS_MASK DesiredAccess;
    PACCESS_STATE AccessState;

    PVCB Vcb = NULL;
    BOOLEAN OpenExisting = FALSE;
    // Hold two locks during tree traversal (child + parent)
    // CurrentFcb = current node lock, PreviousFcb = parent node lock (released after operations)
    PFCB CurrentFcb = NULL;
    PFCB PreviousFcb = NULL;

    BOOLEAN DeleteOnClose;
    BOOLEAN OpenByFileId;
    BOOLEAN DirectoryFile;
    BOOLEAN NonDirectoryFile;
    BOOLEAN SequentialOnly;

    // Is this open for a target directory (used in rename operations)?
    BOOLEAN OpenTargetDirectory;
    // Should we ignore case when attempting to locate the object?
    BOOLEAN IgnoreCase;

    PCCB RelatedCcb = NULL;
    PCCB PtrNewCcb = NULL;
    PFCB NextFcb = NULL;
    PFCB PtrNewFcb = NULL;

    ULONG ReturnedInformation = 0;

    PUNICODE_STRING FileName;
    UNICODE_STRING  RelatedObjectName;
    PUNICODE_STRING RelatedFileName = NULL;

    //BOOLEAN VolumeOpen = FALSE;

    TYPE_OF_OPEN RelatedTypeOfOpen = UnopenedFileObject;

    UNICODE_STRING AbsolutePathName;    // '\aaa\cdf\fff\rrrr.tre:s'
    UNICODE_STRING LocalPath;           // '\aaa\cdf'
    UNICODE_STRING CurName;             // 'cdf'
    UNICODE_STRING TailName;            // 'fff\rrrr.tre:s'
    UNICODE_STRING LastGoodName;        // it depends...
    UNICODE_STRING LastGoodTail;        // it depends...
    UNICODE_STRING StreamName;          // ':s'

    UNICODE_STRING RemainingName;

    PUDF_FILE_INFO RelatedFileInfo;
    PUDF_FILE_INFO OldRelatedFileInfo = NULL;
    PUDF_FILE_INFO NewFileInfo = NULL;
    PUDF_FILE_INFO LastGoodFileInfo = NULL;
    PWCHAR TmpBuffer;
    BOOLEAN VolumeOpen = FALSE;

    BOOLEAN StreamOpen = FALSE;
    BOOLEAN StreamTargetOpen = FALSE;
    BOOLEAN StreamExists = FALSE;
    BOOLEAN RestoreShareAccess = FALSE;
    BOOLEAN SkipPathTraversal = FALSE;
    PWCHAR TailNameBuffer = NULL;
    ULONG SNameIndex = 0;
    DECLARE_CONST_UNICODE_STRING(StreamSuffix, L":$DATA");
    DIR_ENUM_CONTEXT DirContext;

    PAGED_CODE();

    ASSERT(IrpContext);
    ASSERT(Irp);

    // If we were called with our file system device object instead of a
    // volume device object, just complete this request with STATUS_SUCCESS.

    if (IrpContext->Vcb == NULL) {

        UDFCompleteRequest(IrpContext, Irp, STATUS_SUCCESS);
        return STATUS_SUCCESS;
    }

    Options             = IrpSp->Parameters.Create.Options;
    OpenTargetDirectory = BooleanFlagOn(IrpSp->Flags, SL_OPEN_TARGET_DIRECTORY);
    DirectoryFile       = BooleanFlagOn(Options, FILE_DIRECTORY_FILE);
    NonDirectoryFile    = BooleanFlagOn(Options, FILE_NON_DIRECTORY_FILE);
    OpenByFileId = BooleanFlagOn(IrpSp->Parameters.Create.Options, FILE_OPEN_BY_FILE_ID);
    IgnoreCase = !BooleanFlagOn( IrpSp->Flags, SL_CASE_SENSITIVE );
    CreateDisposition = (IrpSp->Parameters.Create.Options >> 24) & 0x000000ff;
    SequentialOnly = BooleanFlagOn(Options, FILE_SEQUENTIAL_ONLY);
    DeleteOnClose = BooleanFlagOn(IrpSp->Parameters.Create.Options, FILE_DELETE_ON_CLOSE);

    Vcb = IrpContext->Vcb;

    ASSERT_VCB(Vcb);

    // Check if the volume is read - only or write - protected and if the operation
    // requires write access

    if (FlagOn(Vcb->VcbState, VCB_STATE_MEDIA_WRITE_PROTECT | VCB_STATE_VOLUME_READ_ONLY)) {

        if (CreateDisposition == FILE_OVERWRITE_IF ||
            CreateDisposition == FILE_SUPERSEDE ||
            CreateDisposition == FILE_CREATE ||
            CreateDisposition == FILE_OVERWRITE ||
            OpenTargetDirectory ||
            FlagOn(Options, FILE_DELETE_ON_CLOSE)) {

            if (FlagOn(Vcb->VcbState, VCB_STATE_MEDIA_WRITE_PROTECT)) {

                IoSetHardErrorOrVerifyDevice(IrpContext->Irp, Vcb->Vpb->RealDevice);
                IrpContext->ExceptionStatus = STATUS_MEDIA_WRITE_PROTECTED;
                ExRaiseStatus(STATUS_MEDIA_WRITE_PROTECTED);
            }

            RC = STATUS_ACCESS_DENIED; 
            if (FlagOn(Vcb->VcbState, VCB_STATE_MOUNTED_DIRTY)) {

                RC = STATUS_VOLUME_DIRTY;
            }

            UDFCompleteRequest(IrpContext, Irp, RC);
            return RC;
        }
    }

    // Check for invalid combination of DELETE_ON_CLOSE with other flags

    if (FlagOn(Options, FILE_DELETE_ON_CLOSE) &&
        (OpenTargetDirectory || FlagOn(Options, FILE_OPEN_BY_FILE_ID))) {

        UDFCompleteRequest(IrpContext, Irp, STATUS_INVALID_PARAMETER);
        return STATUS_INVALID_PARAMETER;
    }

    // UDFS does not support Extended Attributes

    if (IrpSp->Parameters.Create.EaLength || Irp->AssociatedIrp.SystemBuffer) {

        UDFCompleteRequest(IrpContext, Irp, STATUS_EAS_NOT_SUPPORTED);
        return STATUS_EAS_NOT_SUPPORTED;
    }

    //  UDFS does not support paging files

    if (FlagOn(IrpSp->Flags, SL_OPEN_PAGING_FILE)) {

        UDFCompleteRequest(IrpContext, Irp, STATUS_NOT_SUPPORTED);
        return STATUS_NOT_SUPPORTED;
    }

#if (NTDDI_VERSION >= NTDDI_WIN7)

    // UDFS does not support FILE_OPEN_REQUIRING_OPLOCK

    if (FlagOn(Options, FILE_OPEN_REQUIRING_OPLOCK)) {

        UDFCompleteRequest(IrpContext, Irp, STATUS_INVALID_PARAMETER);
        return STATUS_INVALID_PARAMETER;
    }
#endif

    // Reference our input parameters to make things easier

    FileObject = IrpSp->FileObject;
    RelatedFileObject = NULL;

    FileName = &FileObject->FileName;

    // Set up the file object's Vpb pointer in case anything happens.
    // This will allow us to get a reasonable pop-up.

    if ((FileObject->RelatedFileObject != NULL) && !OpenByFileId) {

        RelatedFileObject = FileObject->RelatedFileObject;
        FileObject->Vpb = RelatedFileObject->Vpb;

        RelatedTypeOfOpen = UDFDecodeFileObject(RelatedFileObject, &NextFcb, &RelatedCcb);

        ASSERT_CCB(RelatedCcb);
        ASSERT_FCB(NextFcb);

        //  Fail the request if this is not a user file object.

        if (RelatedTypeOfOpen < UserVolumeOpen) {

            UDFCompleteRequest( IrpContext, Irp, STATUS_INVALID_PARAMETER );
            return STATUS_INVALID_PARAMETER;
        }

        //  Remember the name in the related file object.

        RelatedFileName = &RelatedFileObject->FileName;
    }

   // If we haven't initialized the names then make sure the strings are valid.
   // If this an OpenByFileId then verify the file id buffer.
   //
   // After this routine returns we know that the full name is in the
   // FileName buffer and the buffer will hold the upcased portion
   // of the name yet to parse immediately after the full name in the
   // buffer.  Any trailing backslash has been removed and the flag
   // in the IrpContext will indicate whether we removed the
   // backslash.

    RC = UDFNormalizeFileNames(IrpContext,
                               Vcb,
                               OpenByFileId,
                               RelatedTypeOfOpen,
                               RelatedCcb,
                               RelatedFileName,
                               FileName,
                               &RemainingName);

    //  Return the error code if not successful.

    if (!NT_SUCCESS(RC)) {

        UDFCompleteRequest(IrpContext, Irp, RC);
        return RC;
    }

    // We want to acquire the Vcb.  Exclusively for a volume open, shared otherwise.
    // The file name is empty for a volume open.

    if ((FileName->Length == 0) &&
        (RelatedTypeOfOpen <= UserVolumeOpen) &&
        !OpenByFileId) {

        VolumeOpen = TRUE;
        UDFAcquireVcbExclusive(IrpContext, Vcb, FALSE);

    } else {

        UDFAcquireVcbShared(IrpContext, Vcb, FALSE);
    }

    // Use a try-finally to facilitate cleanup.

    _SEH2_TRY {

        AbsolutePathName.Buffer =
        LocalPath.Buffer = NULL;

        AbsolutePathName.Length = AbsolutePathName.MaximumLength =
        LocalPath.Length = LocalPath.MaximumLength = 0;

        // If the caller cannot block, post the request to be handled
        //  asynchronously
        if (!(IrpContext->Flags & IRP_CONTEXT_FLAG_WAIT)) {
            // We must defer processing of this request since we could
            //  block anytime while performing the create/open ...
            ASSERT(FALSE);
            RC = UDFFsdPostRequest(IrpContext, Irp);
            try_return(RC);
        }

        // If a related file object is present, get the pointers
        //  to the CCB and the FCB for the related file object
        if (RelatedFileObject) {

            RelatedObjectName = RelatedFileObject->FileName;
            if (!(RelatedObjectName.Length) || (RelatedObjectName.Buffer[0] != L'\\')) {
                if (NextFcb->FCBName)
                    RelatedObjectName = NextFcb->FCBName->ObjectName;
            }
        }

        // Allocation size is only used if a new file is created
        //  or a file is superseded.
        AllocationSize = Irp->Overlay.AllocationSize.QuadPart;

        AccessState = IrpSp->Parameters.Create.SecurityContext->AccessState;
        DesiredAccess = IrpSp->Parameters.Create.SecurityContext->DesiredAccess;
        FileAttributes  = (USHORT)(IrpSp->Parameters.Create.FileAttributes & FILE_ATTRIBUTE_VALID_FLAGS);
        ShareAccess = IrpSp->Parameters.Create.ShareAccess;

        // Verify that the Vcb is not in an unusable condition.  This routine
        // will raise if not usable.

        UDFVerifyVcb(IrpContext, Vcb);

        // If the Vcb is locked then we cannot open another file

        if (FlagOn(Vcb->VcbState, VCB_STATE_LOCKED)) {

            try_return(RC = STATUS_ACCESS_DENIED);
        }

        ASSERT(Vcb->VcbCondition == VcbMounted);

        // If we are opening this volume Dasd then process this immediately
        // and exit.

        // ****************
        // If a Volume open is requested, satisfy it now
        // ****************
        if (VolumeOpen) {

            BOOLEAN UndoLock = FALSE;

            // If the supplied file name is NULL *and* either there exists
            //  no related file object *or* if a related file object was supplied
            //  but it too refers to a previously opened instance of a logical
            //  volume, this open must be for a logical volume.

            //  Note: the FSD might decide to do "special" things (whatever they
            //  might be) in response to an open request for the logical volume.

            //  Logical volume open requests are done primarily to get/set volume
            //  information, lock the volume, dismount the volume (using the IOCTL
            //  FSCTL_DISMOUNT_VOLUME) etc.

            // The only create disposition we allow is OPEN.

            if ((CreateDisposition != FILE_OPEN) &&
                (CreateDisposition != FILE_OPEN_IF)) {

                try_return(RC = STATUS_ACCESS_DENIED);
            }

            //  If a volume open is requested, perform checks to ensure that
            //  invalid options have not also been specified ...
            if (OpenTargetDirectory) {
                try_return(RC = STATUS_INVALID_PARAMETER);
            }

            if (DirectoryFile) {
                // a volume is not a directory
                try_return(RC = STATUS_NOT_A_DIRECTORY);
            }

            if (DeleteOnClose) {
                // delete volume.... hmm
                try_return(RC = STATUS_CANNOT_DELETE);
            }

            UDFPrint(("  ShareAccess %x, DesiredAccess %x\n", ShareAccess, DesiredAccess));
/*
            if (!(ShareAccess & (FILE_SHARE_WRITE | FILE_SHARE_DELETE)) &&
               !(DesiredAccess & (FILE_GENERIC_WRITE & ~SYNCHRONIZE)) &&
                (ShareAccess & FILE_SHARE_READ) ) {
*/
            if (!(DesiredAccess & ((GENERIC_WRITE | FILE_GENERIC_WRITE) & ~(SYNCHRONIZE | READ_CONTROL))) &&
                (ShareAccess & FILE_SHARE_READ) ) {
                UDFPrint(("  R/O volume open\n"));
            } else {

                UDFPrint(("  R/W volume open\n"));
                if (Vcb->VcbState & VCB_STATE_MEDIA_WRITE_PROTECT) {
                    UDFPrint(("  media-ro\n"));
                    try_return(RC = STATUS_MEDIA_WRITE_PROTECTED);
                }
            }

            if (!(ShareAccess & (FILE_SHARE_WRITE | FILE_SHARE_DELETE)) &&
               !(DesiredAccess & ((GENERIC_WRITE | FILE_GENERIC_WRITE) & ~(SYNCHRONIZE | READ_CONTROL))) &&
                (ShareAccess & FILE_SHARE_READ) ) {
                // do nothing
            } else {

                if (!(ShareAccess & FILE_SHARE_READ) ||
                    (DesiredAccess & ((GENERIC_WRITE | FILE_GENERIC_WRITE) & ~(SYNCHRONIZE | READ_CONTROL))) ) {
                    // As soon as OpenVolume flushes the volume
                    // we should complete all pending requests (Close)

                    UDFPrint(("  set UDF_IRP_CONTEXT_FLUSH2_REQUIRED\n"));
                    IrpContext->Flags |= UDF_IRP_CONTEXT_FLUSH2_REQUIRED;

/*
                    InterlockedIncrement((PLONG)&Vcb->VcbReference);
                    UDFReleaseResource(&(Vcb->VcbResource));
                    AcquiredVcb = FALSE;

                    UDFCloseAllSystemDelayedInDir(Vcb, Vcb->RootDirFCB->FileInfo);

#ifdef UDF_DELAYED_CLOSE
                    UDFCloseAllDelayed(Vcb);
#endif //UDF_DELAYED_CLOSE

                    UDFAcquireResourceExclusive(&(Vcb->VcbResource), TRUE);
                    AcquiredVcb = TRUE;
                    InterlockedDecrement((PLONG)&Vcb->VcbReference);
*/
                }
            }

            // If the user does not want to share write or delete then we will try
            // and take out a lock on the volume.
            if (!(ShareAccess & (FILE_SHARE_WRITE | FILE_SHARE_DELETE))) {
                // Do a quick check here for handles on exclusive open.
                if ((Vcb->VcbCleanup) &&
                    !(ShareAccess & FILE_SHARE_READ)) {
                    // Sharing violation
                    UDFPrint(("  !FILE_SHARE_READ + open handles (%d)\n", Vcb->VcbCleanup));
                    try_return(RC = STATUS_SHARING_VIOLATION);
                }
                if (IrpContext->Flags & UDF_IRP_CONTEXT_FLUSH2_REQUIRED) {

                    UDFPrint(("  perform flush\n"));
                    IrpContext->Flags &= ~UDF_IRP_CONTEXT_FLUSH2_REQUIRED;

                    InterlockedIncrement((PLONG)&Vcb->VcbReference);

                    UDFFspClose(Vcb);

                    InterlockedDecrement((PLONG)&Vcb->VcbReference);

                    UDFFlushVolume(IrpContext, Vcb);
                }
                // Lock the volume
                if (!(ShareAccess & FILE_SHARE_READ)) {
                    UDFPrint(("  set Lock\n"));
                    Vcb->VcbState |= VCB_STATE_LOCKED;
                    Vcb->VolumeLockFileObject = FileObject;
                    UndoLock = TRUE;
                } else
                if (DesiredAccess & ((GENERIC_WRITE | FILE_GENERIC_WRITE) & ~(SYNCHRONIZE | READ_CONTROL))) {
                    UDFPrint(("  set UDF_IRP_CONTEXT_FLUSH_REQUIRED\n"));
                    IrpContext->Flags |= UDF_IRP_CONTEXT_FLUSH_REQUIRED;
                }
            }

            PtrNewFcb = Vcb->VolumeDasdFcb;

            ASSERT(!(PtrNewFcb->FcbState & UDF_FCB_DELETE_ON_CLOSE));

            RC = UDFCompleteFcbOpen(IrpContext, IrpSp, Vcb, &PtrNewFcb, UserVolumeOpen, 0, CreateDisposition);

            if (NT_SUCCESS(RC)) {
                PtrNewCcb = UDFDecodeFileObjectCcb(FileObject);

                // Check _Security_
                RC = UDFCheckAccessRights(NULL, AccessState, Vcb->RootIndexFcb, PtrNewCcb, DesiredAccess, ShareAccess);
                if (NT_SUCCESS(RC)) {
                    // Check _ShareAccess_
                    RC = UDFCheckAccessRights(FileObject, AccessState, PtrNewFcb, PtrNewCcb, DesiredAccess, ShareAccess);
                    if (NT_SUCCESS(RC)) {
                        Options |= FILE_NO_INTERMEDIATE_BUFFERING;
                        ReturnedInformation = FILE_OPENED;
                        try_return(RC);
                    } else {
                        AdPrint(("    Sharing violation (Volume)\n"));
                    }
                } else {
                    AdPrint(("    Access violation (Volume)\n"));
                }
            }

            // Error cleanup
            if (UndoLock) {
                Vcb->VcbState &= ~VCB_STATE_LOCKED;
                Vcb->VolumeLockFileObject = NULL;
            }
            try_return(RC);
        }

        if (UDFIllegalFcbAccess(Vcb, DesiredAccess)) {
            ReturnedInformation = 0;
            AdPrint(("    Illegal share access\n"));
            try_return(RC = STATUS_ACCESS_DENIED);
        }
        // we could mount blank R/RW media in order to allow
        // user-mode applications to get access with Write privileges
        ASSERT(Vcb->VcbCondition == VcbMounted);

        // The FSD might wish to implement the open-by-id option. The "id"
        //  is some unique numerical representation of the on-disk object.
        //  The caller then therefore give us this file id and the FSD
        //  should be completely capable of "opening" the object (it must
        //  exist since the caller received an id for the object from the
        //  FSD in a "query file" call ...

        //  If the file has been deleted in the meantime, we'll return
        //  "not found"

        // ****************
        // Open by FileID
        // ****************
        if (OpenByFileId) {
            // perform the open ...
            PUNICODE_STRING TmpPath;
            FILE_ID FileId;

            UDFPrint(("    open by File ID\n"));

            FileId = *((FILE_ID*)(FileName->Buffer));
            AdPrint(("  Opening by ID %8.8x%8.8x\n", (ULONG)(FileId.QuadPart>>32), (ULONG)FileId.QuadPart));
            if ((CreateDisposition != FILE_OPEN) &&
                (CreateDisposition != FILE_OPEN_IF)) {
                AdPrint(("    Illegal disposition for ID open\n"));
                try_return(RC = STATUS_ACCESS_DENIED);
            }

            RC = UDFGetOpenParamsByFileId(Vcb, FileId, &TmpPath, &IgnoreCase);
            if (!NT_SUCCESS(RC)) {
                AdPrint(("    ID open failed\n"));
                try_return(RC);
            }
            // simulate absolute path open
/*            if (!NT_SUCCESS(RC = MyInitUnicodeString(&TargetObjectName, L"")) ||
               !NT_SUCCESS(RC = MyAppendUnicodeStringToStringTag(&TargetObjectName, TmpPath, MEM_USABS_TAG))) {*/
            if (!NT_SUCCESS(RC = MyCloneUnicodeString(&AbsolutePathName, TmpPath))) {
                AdPrint(("    Init String failed\n"));
                try_return(RC);
            }

            FileName = &AbsolutePathName;
            RelatedFileObject = NULL;
        } else
        // ****************
        // Relative open
        // ****************
        // Now determine the starting point from which to begin the parsing
        if (RelatedFileObject) {
            // We have a user supplied related file object.
            //  This implies a "relative" open i.e. relative to the directory
            //  represented by the related file object ...

            UDFPrint(("    PtrRelatedFileObject %x, FCB %x\n", RelatedFileObject, NextFcb));
            //  Note: The only purpose FSD implementations ever have for
            //  the related file object is to determine whether this
            //  is a relative open or not. At all other times (including
            //  during I/O operations), this field is meaningless from
            //  the FSD's perspective.
            if (!(NextFcb->FcbState & UDF_FCB_DIRECTORY)) {
                if (UDFIsStreamsSupported(Vcb) && FileName->Length && (FileName->Buffer[0] == L':')) {
                    StreamTargetOpen = TRUE;
                }
                else {
                    // we must have a directory as the "related" object
                    RC = STATUS_INVALID_PARAMETER;
                    AdPrint(("    Related object must be a directory\n"));
                    AdPrint(("    Flags %x\n", NextFcb->FcbState));
                    _SEH2_TRY {
                        AdPrint(("    ObjName %x, ", NextFcb->FCBName->ObjectName));
                        AdPrint(("    Name %S\n", NextFcb->FCBName->ObjectName.Buffer));
                    } _SEH2_EXCEPT(EXCEPTION_EXECUTE_HANDLER) {
                        AdPrint(("    exception when printing name\n"));
                    } _SEH2_END;
                    try_return(RC);
                }

            }

            // So we have a directory, ensure that the name begins with
            //  a "\" i.e. begins at the root and does *not* begin with a "\\"
            //  NOTE: This is just an example of the kind of path-name string
            //  validation that a FSD must do. Although the remainder of
            //  the code may not include such checks, any commercial
            //  FSD *must* include such checking (no one else, including
            //  the I/O Manager will perform checks on the FSD's behalf)
            if (!(RelatedObjectName.Length) || (RelatedObjectName.Buffer[0] != L'\\')) {
                AdPrint(("    Wrong pathname (1)\n"));
                RC = STATUS_INVALID_PARAMETER;
                try_return(RC);
            }
            // similarly, if the target file name starts with a "\", it
            //  is wrong since the target file name can no longer be absolute
            ASSERT(FileName->Buffer || !FileName->Length);
            if (FileName->Length && (FileName->Buffer[0] == L'\\')) {
                AdPrint(("    Wrong pathname (2)\n"));
                RC = STATUS_INVALID_PARAMETER;
                try_return(RC);
            }
            // Create an absolute path-name. We could potentially use
            //  the absolute path-name if we cache previously opened
            //  file/directory object names.
/*            if (!NT_SUCCESS(RC = MyInitUnicodeString(&AbsolutePathName, L"")) ||
               !NT_SUCCESS(RC MyAppendUnicodeStringToStringTag(&AbsolutePathName, &RelatedObjectName, MEM_USABS_TAG)))*/
            if (!NT_SUCCESS(RC = MyCloneUnicodeString(&AbsolutePathName, &RelatedObjectName)))
                try_return(RC);
            if (!StreamTargetOpen) {
                if (RelatedObjectName.Length &&
                    (RelatedObjectName.Buffer[ (RelatedObjectName.Length/sizeof(WCHAR)) - 1 ] != L'\\')) {
                    RC = MyAppendUnicodeToString(&AbsolutePathName, L"\\");
                    if (!NT_SUCCESS(RC)) try_return(RC);
                }
                if (!AbsolutePathName.Length ||
                    (AbsolutePathName.Buffer[ (AbsolutePathName.Length/sizeof(WCHAR)) - 1 ] != L'\\')) {
                    ASSERT(FileName->Buffer);
                    if (FileName->Length && FileName->Buffer[0] != L'\\') {
                        RC = MyAppendUnicodeToString(&AbsolutePathName, L"\\");
                        if (!NT_SUCCESS(RC)) try_return(RC);
                    }
                }
            }
            //ASSERT(TargetObjectName.Buffer);
            RC = MyAppendUnicodeStringToStringTag(&AbsolutePathName, FileName, MEM_USABS_TAG);
            if (!NT_SUCCESS(RC))
                try_return(RC);

            // check for :$DATA suffix 
            if (StreamTargetOpen && AbsolutePathName.Length > StreamSuffix.Length) {
                UNICODE_STRING Tail;
                Tail.Buffer = &AbsolutePathName.Buffer[(AbsolutePathName.Length - StreamSuffix.Length)/sizeof(WCHAR)];
                Tail.Length = Tail.MaximumLength = StreamSuffix.Length;

                if (RtlEqualUnicodeString(&Tail, &StreamSuffix, TRUE)) {
                    AbsolutePathName.Length -= StreamSuffix.Length;
                }
            }
        } else {
        // ****************
        // Absolute open
        // ****************
            // The suplied path-name must be an absolute path-name i.e.
            //  starting at the root of the file system tree
            UDFPrint(("    Absolute open\n"));
            ASSERT(FileName->Buffer);
            if (!FileName->Length || FileName->Buffer[0] != L'\\') {
                AdPrint(("    Wrong target name (1)\n"));
                try_return(RC = STATUS_INVALID_PARAMETER);
            }
/*            if (!NT_SUCCESS(RC = MyInitUnicodeString(&AbsolutePathName, L"")) ||
               !NT_SUCCESS(RC = MyAppendUnicodeStringToStringTag(&AbsolutePathName, &TargetObjectName, MEM_USABS_TAG)))*/
            ASSERT(FileName->Buffer);
            if (!NT_SUCCESS(RC = MyCloneUnicodeString(&AbsolutePathName, FileName)))
                try_return(RC);
        }
        // Win 32 protection :)
        if ((AbsolutePathName.Length >= sizeof(WCHAR)*2) &&
            (AbsolutePathName.Buffer[1] == L'\\') &&
            (AbsolutePathName.Buffer[0] == L'\\')) {

            //  If there are still two beginning backslashes, the name is bogus.
            if ((AbsolutePathName.Length > 2*sizeof(WCHAR)) &&
                (AbsolutePathName.Buffer[2] == L'\\')) {
                AdPrint(("    Wrong target name (2)\n"));
                try_return (RC = STATUS_OBJECT_NAME_INVALID);
            }
            //  Slide the name down in the buffer.
            RtlMoveMemory( AbsolutePathName.Buffer,
                           AbsolutePathName.Buffer + 1,
                           AbsolutePathName.Length ); // .Length includes
                                                      //      NULL-terminator
            AbsolutePathName.Length -= sizeof(WCHAR);
        }
        if ( (AbsolutePathName.Length > sizeof(WCHAR) ) &&
            (AbsolutePathName.Buffer[ (AbsolutePathName.Length/sizeof(WCHAR)) - 1 ] == L'\\') ) {

            AbsolutePathName.Length -= sizeof(WCHAR);
        }
        // TERMINATOR (2)   ;)
        AbsolutePathName.Buffer[AbsolutePathName.Length/sizeof(WCHAR)] = 0;

        // Sometimes W2000 decides to duplicate handle of
        // already opened File/Dir. In this case it sends us
        // RelatedFileObject & specifies zero-filled RelativePath
        if (!FileName->Length) {
            FileName = &AbsolutePathName;
            OpenExisting = TRUE;
        }
        //ASSERT(TargetObjectName.Buffer);

        // ****************
        //  First, check if the caller simply wishes to open the Root
        //  of the file system tree.
        // ****************
        if (AbsolutePathName.Length == sizeof(WCHAR)) {
            AdPrint(("  Opening RootDir\n"));
            // this is an open of the root directory, ensure that the caller
            // has not requested a file only
            if (NonDirectoryFile || (CreateDisposition == FILE_SUPERSEDE) ||
                 (CreateDisposition == FILE_OVERWRITE) ||
                 (CreateDisposition == FILE_OVERWRITE_IF)) {
                AdPrint(("    Can't overwrite RootDir\n"));
                RC = STATUS_FILE_IS_A_DIRECTORY;
                try_return(RC);
            }

            if (DeleteOnClose) {
                // delete RootDir.... rather strange idea... I dislike it
                AdPrint(("    Can't delete RootDir\n"));
                try_return(RC = STATUS_CANNOT_DELETE);
            }

            PtrNewFcb = Vcb->RootIndexFcb;
            RC = UDFCompleteFcbOpen(IrpContext, IrpSp, Vcb, &PtrNewFcb, UserDirectoryOpen, 0, CreateDisposition);
            if (!NT_SUCCESS(RC)) try_return(RC);
            // Reference root's FileInfo (root has no parent, so no LCB)
            UDFReferenceFile__(PtrNewFcb->FileInfo);
            PtrNewCcb = UDFDecodeFileObjectCcb(FileObject);

            RC = UDFCheckAccessRights(FileObject, AccessState, PtrNewFcb, PtrNewCcb, DesiredAccess, ShareAccess);
            if (!NT_SUCCESS(RC)) {
                AdPrint(("    Access/Sharing violation (RootDir)\n"));
                try_return(RC);
            }

            ReturnedInformation = FILE_OPENED;

            try_return(RC);
        } // end of OpenRootDir

        _SEH2_TRY {
            AdPrint(("    Opening file %ws %8.8x\n",AbsolutePathName.Buffer, FileObject));
        } _SEH2_EXCEPT(EXCEPTION_EXECUTE_HANDLER) {
            AdPrint(("    Exception when printing FN\n"));
        } _SEH2_END;
        // ****************
        // Check if we have DuplicateHandle (or Reopen) request
        // ****************
        if (OpenExisting) {

//            BrutePoint();
            // We don't handle OpenTargetDirectory in this case
            if (OpenTargetDirectory)
                try_return(RC = STATUS_INVALID_PARAMETER);

            // Init environment to simulate normal open procedure behavior
/*            if (!NT_SUCCESS(RC = MyInitUnicodeString(&LocalPath, L"")) ||
               !NT_SUCCESS(RC = MyAppendUnicodeStringToStringTag(&LocalPath, &TargetObjectName, MEM_USLOC_TAG)))*/
            ASSERT(FileName->Buffer);
            if (!NT_SUCCESS(RC = MyCloneUnicodeString(&LocalPath, FileName)))
                try_return(RC);

            ASSERT(NextFcb);
            RelatedFileInfo = NextFcb->FileInfo;

            RC = STATUS_SUCCESS;
            NewFileInfo =
            LastGoodFileInfo = RelatedFileInfo;

            RelatedFileInfo =
            OldRelatedFileInfo = RelatedFileInfo->ParentFile;
            NextFcb = NextFcb->ParentFcb;
            // Parent references are now handled by LCB in UDFCompleteFcbOpen

            // Acquire FCB lock for tree traversal
            // Skip if same FCB to avoid recursive acquire leading to lock leak
            PtrNewFcb = NewFileInfo->Fcb;
            if (PtrNewFcb != CurrentFcb) {
                UDF_CHECK_PAGING_IO_RESOURCE(PtrNewFcb);
                UDFAcquireFcbExclusive(IrpContext, PtrNewFcb, FALSE);
                // Release grandparent lock from previous iteration
                if (PreviousFcb && PreviousFcb != CurrentFcb) {
                    UDFReleaseResource(&PreviousFcb->FcbNonpaged->FcbResource);
                }
                PreviousFcb = CurrentFcb;
                CurrentFcb = PtrNewFcb;
            }

            // Reference target file's FileInfo (balanced by cleanup)
            UDFReferenceFile__(NewFileInfo);

            SkipPathTraversal = TRUE;
        }

        if (!SkipPathTraversal) {

        //AdPrint(("    Opening file %ws %8.8x\n",AbsolutePathName.Buffer, PtrNewFileObject));

        if (AbsolutePathName.Length > UDF_X_PATH_LEN*sizeof(WCHAR)) {
            try_return(RC = STATUS_OBJECT_NAME_INVALID);
        }

        // validate path specified
        // (sometimes we can see here very strange characters ;)
        if (!UDFIsNameValid(&AbsolutePathName, &StreamOpen, &SNameIndex)) {
            AdPrint(("    Absolute path is not valid\n"));
            try_return(RC = STATUS_OBJECT_NAME_INVALID);
        }
        if (StreamOpen && !UDFIsStreamsSupported(Vcb)) {
            ReturnedInformation = FILE_DOES_NOT_EXIST;
            try_return(RC = STATUS_OBJECT_NAME_INVALID);
        }

        RC = MyInitUnicodeString(&LocalPath, L"");
        if (!NT_SUCCESS(RC))
            try_return(RC);
        if (RelatedFileObject) {
            // Our "start directory" is the one identified
            // by the related file object
            RelatedFileInfo = NextFcb->FileInfo;
            if (RelatedFileInfo != Vcb->RootIndexFcb->FileInfo) {
                RC = MyAppendUnicodeStringToStringTag(&LocalPath, &(NextFcb->FCBName->ObjectName), MEM_USLOC_TAG);
                if (!NT_SUCCESS(RC))
                    try_return(RC);
            }
            if (FileName->Buffer != AbsolutePathName.Buffer) {
                ASSERT(FileName->Buffer);
                if (!NT_SUCCESS(RC = MyCloneUnicodeString(&TailName, FileName))) {
                    AdPrint(("    Init String 'TargetObjectName' failed\n"));
                    try_return(RC);
                }
                TailNameBuffer = TailName.Buffer;
            } else {
                TailName = AbsolutePathName;
            }
        } else {
            // Start at the root of the file system
            RelatedFileInfo = Vcb->RootIndexFcb->FileInfo;
            TailName = AbsolutePathName;
        }

        if (StreamOpen) {
            StreamName = AbsolutePathName;
            StreamName.Buffer += SNameIndex;
            StreamName.Length -= (USHORT)SNameIndex*sizeof(WCHAR);
            // if StreamOpen specified & stream name starts with NULL character
            // we should create Stream Dir at first
            TailName.Length -= (AbsolutePathName.Length - (USHORT)SNameIndex*sizeof(WCHAR));
            AbsolutePathName.Length = (USHORT)SNameIndex*sizeof(WCHAR);
        }
        CurName.MaximumLength = TailName.MaximumLength;

        RC = STATUS_SUCCESS;
        LastGoodName.Length = 0;
        LastGoodFileInfo = RelatedFileInfo;
        // reference RelatedObject to prevent releasing parent structures
        // Acquire only CurrentFcb (= LastGoodFileInfo->Fcb)
        CurrentFcb = RelatedFileInfo->Fcb;
        UDF_CHECK_PAGING_IO_RESOURCE(CurrentFcb);
        UDFAcquireFcbExclusive(IrpContext, CurrentFcb, FALSE);

        // Parent references are now handled by LCB in UDFCompleteFcbOpen
        // when child files are opened through this directory.

        // go into a loop parsing the supplied name

        //  Note that we may have to "open" intermediate directory objects
        //  while traversing the path. We should __try to reuse existing code
        //  whenever possible therefore we should consider using a common
        //  open routine regardless of whether the open is on behalf of the
        //  caller or an intermediate (internal) open performed by the driver.

        // ****************
        // now we'll parse path to desired file
        // ****************

        while (TRUE) {

            // remember last 'good' ('good' means NO ERRORS before) path tail
            if (NT_SUCCESS(RC)) {
                LastGoodTail = TailName;
                while(LastGoodTail.Buffer[0] == L'\\') {
                    LastGoodTail.Buffer++;
                    LastGoodTail.Length -= sizeof(WCHAR);
                }
            }
            // get next path part...
            TmpBuffer = TailName.Buffer;
            TailName.Buffer = UDFDissectName(IrpContext, TailName.Buffer, &CurName.Length);
            TailName.Length -= (USHORT)((ULONG_PTR)(TailName.Buffer) - (ULONG_PTR)TmpBuffer);
            CurName.Buffer = TailName.Buffer - CurName.Length;
            CurName.Length *= sizeof(WCHAR);
            CurName.MaximumLength = CurName.Length + sizeof(WCHAR);
            // check if we have already opened the component before last one
            // in this case OpenTargetDir request will be served in a special
            // way...
            if (OpenTargetDirectory && NT_SUCCESS(RC) && !TailName.Length) {
                // check if we should open SDir..
                if (!StreamOpen ||
                   (TailName.Buffer[0]!=L':')) {
                    // no, we should not. Continue with OpenTargetDir
                    break;
                }
            }

            if ( CurName.Length &&
               (NT_SUCCESS(RC) || !StreamOpen)) {
                // ...wow! non-zero! try to open!
                if (!NT_SUCCESS(RC)) {
                    AdPrint(("    Error opening path component\n"));
                    // we haven't reached last name part... hm..
                    // probably, the path specified is invalid..
                    // or we had a hard error... What else can we do ?
                    // Only say ..CK OFF !!!!
                    if (RC == STATUS_OBJECT_NAME_NOT_FOUND)
                        RC = STATUS_OBJECT_PATH_NOT_FOUND;
                    ReturnedInformation = FILE_DOES_NOT_EXIST;
                    try_return(RC);
                }

                // Note: FcbReference may be 0 for intermediate directories during path traversal;
                // it will be incremented when CCB is created in UDFCompleteFcbOpen

                // Mark intermediate directory FCB as valid (internal open)
                if (RelatedFileInfo && RelatedFileInfo->ParentFile) {
                    RelatedFileInfo->Fcb->NtReqFCBFlags |= UDF_NTREQ_FCB_VALID;
                    RelatedFileInfo->Fcb->FcbState |= UDF_FCB_VALID;
                }
                // check path fragment size
                if (CurName.Length > UDF_X_NAME_LEN * sizeof(WCHAR)) {
                    AdPrint(("    Path component is too long\n"));
                    try_return(RC = STATUS_OBJECT_NAME_INVALID);
                }
                // Acquire FCB lock for tree traversal
                // Skip if same FCB to avoid recursive acquire leading to lock leak
                {
                    PFCB NewFcb = RelatedFileInfo->Fcb;
                    if (NewFcb != CurrentFcb) {
                        UDF_CHECK_PAGING_IO_RESOURCE(NewFcb);
                        UDFAcquireFcbExclusive(IrpContext, NewFcb, FALSE);
                        if (PreviousFcb && PreviousFcb != CurrentFcb) {
                            UDFReleaseResource(&PreviousFcb->FcbNonpaged->FcbResource);
                        }
                        PreviousFcb = CurrentFcb;
                        CurrentFcb = NewFcb;
                    }
                }

                // check traverse rights (uses RelatedFileInfo->Fcb which is now locked as PreviousFcb)
                RC = UDFCheckAccessRights(NULL, NULL, RelatedFileInfo->Fcb, RelatedCcb, FILE_TRAVERSE, 0);
                if (!NT_SUCCESS(RC)) {
                    NewFileInfo = NULL;
                    AdPrint(("    Traverse check failed\n"));
                } else if (CurName.Buffer[0] != ':') {
                    // standard open: first find, then open
                    RC = UDFFindDirEntry(Vcb, RelatedFileInfo, &CurName, IgnoreCase, TRUE, &DirContext);
                    if (NT_SUCCESS(RC)) {
                        // Check if intermediate path component is a directory
                        if (TailName.Length &&
                            !(DirContext.DirNdx->FileCharacteristics & FILE_DIRECTORY)) {
                            AdPrint(("    Not a directory\n"));
                            RC = STATUS_NOT_A_DIRECTORY;
                        } else {
                            RC = UDFOpenObjectFromDirContext(IrpContext, Vcb, &DirContext, TRUE, &NewFileInfo);
                            if (RC == STATUS_FILE_DELETED) {
                                // file has gone, but system still remembers it...
                                NewFileInfo = NULL;
                                AdPrint(("    File deleted\n"));
                                RC = STATUS_ACCESS_DENIED;
                            } else
                            if (RC == STATUS_SHARING_PAUSED) {
                                AdPrint(("    Dloc is being initialized\n"));
                                BrutePoint();
                                RC = STATUS_SHARING_VIOLATION;
                            }
                        }
                    }
#ifdef UDF_DBG
                    else if (RC == STATUS_NOT_A_DIRECTORY) {
                        AdPrint(("    Not a directory\n"));
                    }
#endif // UDF_DBG
                } else {
                    // And here we should open Stream Dir (if any, of course)
                    RC = UDFOpenStreamDir__(IrpContext, Vcb, RelatedFileInfo, &NewFileInfo);
                    if (NT_SUCCESS(RC)) {
                        // this indicates that we needn't Stream Dir creation
                        StreamExists = TRUE;
                        StreamName.Buffer++;
                        StreamName.Length -= sizeof(WCHAR);
                        // update TailName
                        TailName = StreamName;
                    } else
                    if (RC == STATUS_NOT_FOUND) {
                        // Stream Dir doesn't exist, but caller wants it to be
                        // created. Lets try to help him...
                        if ((CreateDisposition == FILE_CREATE) ||
                           (CreateDisposition == FILE_OPEN_IF) ||
                           (CreateDisposition == FILE_OVERWRITE_IF) ||
                            OpenTargetDirectory) {
                            RC = UDFCreateStreamDir__(IrpContext, Vcb, RelatedFileInfo, &NewFileInfo);
                            if (NT_SUCCESS(RC)) {
                                StreamExists = TRUE;
                                StreamName.Buffer++;
                                StreamName.Length -= sizeof(WCHAR);
                                TailName = StreamName;
                            }
                        }
                    }
                }

                // check if we have successfully opened path component
                if (NT_SUCCESS(RC)) {
                    // Get or create FCB for the opened FileInfo
                    if (!(PtrNewFcb = NewFileInfo->Fcb)) {
                        // First open - create FCB
                        RC = UDFFirstOpenFile(IrpContext, IrpSp, Vcb,
                                              NULL, &PtrNewFcb, RelatedFileInfo, NewFileInfo,
                                              &LocalPath, &CurName, CreateDisposition);
                        if (!NT_SUCCESS(RC)) {
                            BrutePoint();
                            AdPrint(("    Can't perform FirstOpen\n"));
                            UDFCloseFile__(IrpContext, Vcb, NewFileInfo);
                            if (PtrNewFcb) UDFDeleteFcb(IrpContext, PtrNewFcb);
                            NewFileInfo->Fcb = NULL;
                            if (UDFCleanUpFile__(Vcb, NewFileInfo)) {
                                MyFreePool__(NewFileInfo);
                                NewFileInfo = NULL;
                            }
                            try_return(RC);
                        }
                    } else {
                        // Validate existing FCB
                        if (!(PtrNewFcb->FcbState & UDF_FCB_VALID)) {
                            BrutePoint();
                            AdPrint(("    Fcb not valid\n"));
                            UDFCloseFile__(IrpContext, Vcb, NewFileInfo);
                            if (UDFCleanUpFile__(Vcb, NewFileInfo)) {
                                MyFreePool__(NewFileInfo);
                                NewFileInfo = NULL;
                            }
                            try_return(RC = STATUS_ACCESS_DENIED);
                        }
                    }

                    // Acquire FCB lock for tree traversal (try-lock to prevent deadlock)
                    {
                        PFCB NewFcb = NewFileInfo->Fcb;
                        if (NewFcb != CurrentFcb) {
                            UDF_CHECK_PAGING_IO_RESOURCE(NewFcb);
                            if (!UDFAcquireFcbExclusive(IrpContext, NewFcb, TRUE)) {
                                // Try-lock failed - rollback and reacquire in order
                                UDFLockVcb(IrpContext, Vcb);
                                NewFcb->FcbCleanup++;
                                UDFUnlockVcb(IrpContext, Vcb);

                                if (PreviousFcb && PreviousFcb != CurrentFcb) {
                                    UDFReleaseResource(&PreviousFcb->FcbNonpaged->FcbResource);
                                }
                                PFCB OldCurrentFcb = CurrentFcb;
                                UDF_CHECK_PAGING_IO_RESOURCE(OldCurrentFcb);
                                UDFReleaseResource(&OldCurrentFcb->FcbNonpaged->FcbResource);

                                UDFAcquireFcbExclusive(IrpContext, NewFcb, FALSE);
                                UDFAcquireFcbExclusive(IrpContext, OldCurrentFcb, FALSE);
                                PreviousFcb = OldCurrentFcb;

                                UDFLockVcb(IrpContext, Vcb);
                                NewFcb->FcbCleanup--;
                                UDFUnlockVcb(IrpContext, Vcb);
                            } else {
                                if (PreviousFcb && PreviousFcb != CurrentFcb) {
                                    UDFReleaseResource(&PreviousFcb->FcbNonpaged->FcbResource);
                                }
                                PreviousFcb = CurrentFcb;
                            }
                            CurrentFcb = NewFcb;
                        }
                    }

                    // FCB references are handled by LCB in UDFCompleteFcbOpen
                    // Note: FcbReference may be 0 here during path traversal;
                    // it will be incremented when CCB is created

                    // Update state
                    LastGoodFileInfo = NewFileInfo;
                    LastGoodName = CurName;

                    // Update current path
                    if (!StreamOpen ||
                         ((CurName.Buffer[0] != L':') &&
                          (!LocalPath.Length || (LocalPath.Buffer[LocalPath.Length/sizeof(WCHAR)-1] != L':'))) ) {
                        ASSERT(!LocalPath.Length ||
                               (LocalPath.Buffer[LocalPath.Length/2-1] != L'\\'));
                        RC = MyAppendUnicodeToString(&LocalPath, L"\\");
                        if (!NT_SUCCESS(RC)) try_return(RC);
                    }
                    RC = MyAppendUnicodeStringToStringTag(&LocalPath, &CurName, MEM_USLOC_TAG);
                    if (!NT_SUCCESS(RC))
                        try_return(RC);
                } else {
                    AdPrint(("    Can't open file\n"));
                    // We have failed durring last Open attempt
                    // Roll back to last good state

                    // Cleanup FileInfo if any
                    if (NewFileInfo) {
                        PtrNewFcb = NewFileInfo->Fcb;
                        // Temporarily acquire NewFcb for cleanup,
                        // but keep CurrentFcb unchanged (= LastGoodFileInfo->Fcb)
                        if (PtrNewFcb) {
                            UDF_CHECK_PAGING_IO_RESOURCE(PtrNewFcb);
                            UDFAcquireFcbExclusive(IrpContext, PtrNewFcb, FALSE);
                        }
                        // cleanup pointer to Fcb in FileInfo to allow
                        // UDF_INFO package release FileInfo if there are
                        // no more references
                        if (PtrNewFcb &&
                           !PtrNewFcb->FcbReference &&
                           !PtrNewFcb->FcbCleanup) {
                            NewFileInfo->Fcb = NULL;
                        }
                        // cleanup pointer to CommonFcb in Dloc to allow
                        // UDF_INFO package release Dloc if there are
                        // no more references
                        if (NewFileInfo->Dloc &&
                           !NewFileInfo->Dloc->LinkRefCount &&
                           (!PtrNewFcb || !PtrNewFcb->FcbReference)) {
                            NewFileInfo->Dloc->CommonFcb = NULL;
                        }
                        // try to release FileInfo
                        if (UDFCleanUpFile__(Vcb, NewFileInfo)) {
                            ASSERT(!PtrNewFcb);
                            if (PtrNewFcb) {
                                BrutePoint();
                                UDFDeleteFcb(IrpContext, PtrNewFcb);
                            }
                            MyFreePool__(NewFileInfo);
                        } else {
                            // if we can't release FileInfo
                            // restore pointers to Fcb & CommonFcb in
                            // FileInfo & Dloc
                            NewFileInfo->Fcb = PtrNewFcb;
                            if (PtrNewFcb)
                                NewFileInfo->Dloc->CommonFcb = PtrNewFcb;
                        }
                        // Release NewFcb lock after cleanup
                        if (PtrNewFcb) {
                            UDFReleaseResource(&PtrNewFcb->FcbNonpaged->FcbResource);
                        }
                        // forget about last FileInfo & Fcb,
                        // further unwind staff needs only last good
                        // structures
                        PtrNewFcb = NULL;
                        NewFileInfo = NULL;
                    }
                }

                // should return error if 'delete in progress'
                if (LastGoodFileInfo->Fcb->FcbState & (UDF_FCB_DELETE_ON_CLOSE |
                                                      UDF_FCB_DELETED |
                                                      UDF_FCB_POSTED_RENAME)) {
                    AdPrint(("  Return DeletePending (no err)\n"));
                    try_return(RC = STATUS_DELETE_PENDING);
                }
                // update last good state information...
                OldRelatedFileInfo = RelatedFileInfo;
                RelatedFileInfo = NewFileInfo;
                // ...and go to the next open cycle
            } else {
                // ************
                if (StreamOpen && (RC == STATUS_NOT_FOUND))
                    // handle SDir return code
                    RC = STATUS_OBJECT_NAME_NOT_FOUND;
                if (RC == STATUS_OBJECT_NAME_NOT_FOUND) {
                    // good path, but no such file.... Amen
                    // break open loop and continue with Create
                    break;
                }
                if (!NT_SUCCESS(RC)) {
                    // Hard error or damaged data structures ...
#ifdef UDF_DBG
                    if ((RC != STATUS_OBJECT_PATH_NOT_FOUND) &&
                       (RC != STATUS_ACCESS_DENIED) &&
                       (RC != STATUS_NOT_A_DIRECTORY)) {
                        AdPrint(("    Hard error or damaged data structures\n"));
                    }
#endif // UDF_DBG
                    // ... and exit with error
                    try_return(RC);
                }
                // Note: With LCB model, FcbReference is not incremented during path traversal,
                // so no decrement is needed here (removed the old InterlockedDecrement).
                RC = STATUS_SUCCESS;
                ASSERT(!OpenTargetDirectory);
                // break open loop and continue with Open
                // (Create will be skipped)
                break;
            }
        } // end of while(TRUE)

        // ****************
        // If "open target directory" was specified
        // ****************
        if (OpenTargetDirectory) {

            if (!UDFIsADirectory(LastGoodFileInfo)) {
                AdPrint(("    Not a directory (2)\n"));
                RC = STATUS_NOT_A_DIRECTORY;
            }
            if (!NT_SUCCESS(RC) ||
               TailName.Length) {
                AdPrint(("    Target name should not contain (back)slashes\n"));
                NewFileInfo = NULL;
                try_return(RC = STATUS_OBJECT_NAME_INVALID);
            }

            NewFileInfo = LastGoodFileInfo;
            RtlCopyUnicodeString(&(FileObject->FileName), &CurName);

            // now we have to check if last component exists...
            if (NT_SUCCESS(RC = UDFFindFile__(Vcb, IgnoreCase,
                                             &CurName, RelatedFileInfo))) {
                // file exists, set this information in the Information field
                ReturnedInformation = FILE_EXISTS;
                AdPrint(("  Open Target: FILE_EXISTS\n"));
            } else
            if (RC == STATUS_OBJECT_NAME_NOT_FOUND) {
#ifdef UDF_DBG
                // check name. If there are '\\'s in TailName, some
                // directories in path specified do not exist
                for(TmpBuffer = LastGoodTail.Buffer; *TmpBuffer; TmpBuffer++) {
                    if ((*TmpBuffer) == L'\\') {
                        ASSERT(FALSE);
                        AdPrint(("    Target name should not contain (back)slashes\n"));
                        try_return(RC = STATUS_OBJECT_NAME_INVALID);
                    }
                }
#endif // UDF_DBG
                // Tell the I/O Manager that file does not exit
                ReturnedInformation = FILE_DOES_NOT_EXIST;
                AdPrint(("  Open Target: FILE_DOES_NOT_EXIST\n"));
                RC = STATUS_SUCCESS; // is already set here
            } else {
                AdPrint(("  Open Target: unexpected error\n"));
                NewFileInfo = NULL;
                ReturnedInformation = FILE_DOES_NOT_EXIST;
                try_return(RC = STATUS_OBJECT_NAME_INVALID);
            }

//          RC = STATUS_SUCCESS; // is already set here

            // Update the file object FsContext and FsContext2 fields
            //  to reflect the fact that the parent directory of the
            //  target has been opened
            PtrNewFcb = NewFileInfo->Fcb;
            // Note: Removed FcbReference decrement - no longer needed with LCB model.
            // The old TreeLength model incremented parent FcbReference during path traversal,
            // but LCB model handles parent references differently (via UDFAcquirePrefix).

            RC = UDFCompleteFcbOpen(IrpContext, IrpSp, Vcb, &PtrNewFcb, UserDirectoryOpen, 0, CreateDisposition);
            if (!NT_SUCCESS(RC)) {
                AdPrint(("    Can't perform OpenFile operation for target\n"));
                try_return(RC);
            }
            PtrNewCcb = UDFDecodeFileObjectCcb(FileObject);

            ASSERT(CurrentFcb);
            RC = UDFCheckAccessRights(FileObject, AccessState, PtrNewFcb, PtrNewCcb, DesiredAccess, ShareAccess);
            if (!NT_SUCCESS(RC)) {
                AdPrint(("    Access/Share access check failed (Open Target)\n"));
            }

            try_return(RC);
        }

        // ****************
        // should we CREATE a new file ?
        // ****************
        if (!NT_SUCCESS(RC)) {
            if (RC == STATUS_OBJECT_NAME_NOT_FOUND ||
                RC == STATUS_OBJECT_PATH_NOT_FOUND) {
                if ( ((CreateDisposition == FILE_OPEN) ||
                    (CreateDisposition == FILE_OVERWRITE)) /*&&
                    (!StreamOpen || !StreamExists)*/ ){
                    ReturnedInformation = FILE_DOES_NOT_EXIST;
                    AdPrint(("    File doesn't exist\n"));
                    try_return(RC);
                }
            } else {
                //  Any other operation return STATUS_ACCESS_DENIED.
                AdPrint(("    Can't create due to unexpected error\n"));
                try_return(RC);
            }
            // Object was not found, create if requested
            if ((CreateDisposition != FILE_CREATE) && (CreateDisposition != FILE_OPEN_IF) &&
                 (CreateDisposition != FILE_OVERWRITE_IF) && (CreateDisposition != FILE_SUPERSEDE)) {
                AdPrint(("    File doesn't exist (2)\n"));
                ReturnedInformation = FILE_DOES_NOT_EXIST;
                try_return(RC);
            }
            // Check Volume ReadOnly attr
            if ((Vcb->VcbState & VCB_STATE_VOLUME_READ_ONLY)) {
                ReturnedInformation = 0;
                AdPrint(("    Write protected\n"));
                try_return(RC = STATUS_MEDIA_WRITE_PROTECTED);
            }
            // Check r/o + delete on close
            if (DeleteOnClose &&
               (FlagOn( IrpSp->Parameters.Create.FileAttributes, FILE_ATTRIBUTE_READONLY ))) {

                AdPrint(("    Can't create r/o file marked for deletion\n"));
                try_return(RC = STATUS_CANNOT_DELETE);
            }

            // Create a new file/directory here ...
            if (StreamOpen)
                StreamName.Buffer[StreamName.Length/sizeof(WCHAR)] = 0;
            for(TmpBuffer = LastGoodTail.Buffer; *TmpBuffer; TmpBuffer++) {
                if ((*TmpBuffer) == L'\\') {
                    AdPrint(("    Target name should not contain (back)slashes\n"));
                    try_return(RC = STATUS_OBJECT_NAME_INVALID);
                }
            }
            if (DirectoryFile &&
               ((IrpSp->Parameters.Create.FileAttributes & FILE_ATTRIBUTE_TEMPORARY) ||
                 StreamOpen || FALSE)) {
                AdPrint(("    Creation of _temporary_ directory not permited\n"));
                try_return(RC = STATUS_INVALID_PARAMETER);
            }
            // check access rights
            ASSERT(CurrentFcb);
            RC = UDFCheckAccessRights(NULL, NULL, OldRelatedFileInfo->Fcb, RelatedCcb, DirectoryFile ? FILE_ADD_SUBDIRECTORY : FILE_ADD_FILE, 0);
            if (!NT_SUCCESS(RC)) {
                AdPrint(("    Creation of File/Dir not permitted\n"));
                try_return(RC);
            }
            // Note that a FCB structure will be allocated at this time
            // and so will a CCB structure. Assume that these are called
            // PtrNewFcb and PtrNewCcb respectively.
            // Further, note that since the file is being created, no other
            // thread can have the file stream open at this time.
            RelatedFileInfo = OldRelatedFileInfo;

            RC = UDFCreateFile__(IrpContext, Vcb, IgnoreCase, &LastGoodTail, 0, 0,
                                 UdfIsExtendedFESupported(Vcb),
                                 (CreateDisposition == FILE_CREATE), RelatedFileInfo, &NewFileInfo);
            if (!NT_SUCCESS(RC)) {
                AdPrint(("    Creation error\n"));
                try_return(RC);
            }
            // Update parent object
            if ((Vcb->CompatFlags & UDF_VCB_IC_UPDATE_DIR_WRITE) &&
               NextFcb &&
               RelatedFileObject &&
               (NextFcb->FileInfo == NewFileInfo->ParentFile)) {
                    RelatedFileObject->Flags |= (FO_FILE_MODIFIED | FO_FILE_SIZE_CHANGED);
            }

            if (DirectoryFile) {
                // user wants the directory to be created
                RC = UDFRecordDirectory__(IrpContext, Vcb, NewFileInfo);
                if (!NT_SUCCESS(RC)) {
                    AdPrint(("    Can't transform to directory\n"));
                    // Undo create - flush and unlink from disk
                    if ((RC != STATUS_FILE_IS_A_DIRECTORY) &&
                       (RC != STATUS_NOT_A_DIRECTORY) &&
                       (RC != STATUS_ACCESS_DENIED)) {
                        UDFFlushFile__(IrpContext, Vcb, NewFileInfo);
                        UDFUnlinkFile__(IrpContext, Vcb, NewFileInfo, TRUE);
                    }
                    UDFCloseFile__(IrpContext, Vcb, NewFileInfo);
                    try_return(RC);
                }
            }

            if (StreamOpen && !StreamExists) {

                // PHASE 0

                // Open the newly created object (file)
                if (!(PtrNewFcb = NewFileInfo->Fcb)) {
                    // It is a first open operation
                    // Allocate new FCB
                    // Here we set FileObject pointer to NULL to avoid
                    // new CCB allocation
                    RC = UDFFirstOpenFile(IrpContext,
                                          IrpSp,
                                   Vcb,
                                   NULL, &PtrNewFcb, RelatedFileInfo, NewFileInfo,
                                   &LocalPath, &LastGoodTail, CreateDisposition);
                    if (!NT_SUCCESS(RC)) {
                        AdPrint(("    Can't perform FirstOpenFile operation for file to contain stream\n"));
                        BrutePoint();
                        if (PtrNewFcb) {
                            UDFDeleteFcb(IrpContext, PtrNewFcb);
                            PtrNewFcb = NULL;
                        }
                        NewFileInfo->Fcb = NULL;
                        try_return(RC);
                    }
                } else {
                    BrutePoint();
                }

                // Update state
                // FCB references are now handled by LCB in UDFCompleteFcbOpen
                LastGoodFileInfo = NewFileInfo;
                // Acquire FCB lock for tree traversal
                // Skip if same FCB to avoid recursive acquire leading to lock leak
                {
                    PFCB NewFcb = NewFileInfo->Fcb;
                    if (NewFcb != CurrentFcb) {
                        UDF_CHECK_PAGING_IO_RESOURCE(NewFcb);
                        UDFAcquireFcbExclusive(IrpContext, NewFcb, FALSE);
                        if (PreviousFcb && PreviousFcb != CurrentFcb) {
                            UDFReleaseResource(&PreviousFcb->FcbNonpaged->FcbResource);
                        }
                        PreviousFcb = CurrentFcb;
                        CurrentFcb = NewFcb;
                    }
                }
                // update FCB tree
                RC = MyAppendUnicodeToString(&LocalPath, L"\\");
                if (!NT_SUCCESS(RC)) try_return(RC);
                RC = MyAppendUnicodeStringToStringTag(&LocalPath, &LastGoodTail, MEM_USLOC_TAG);
                if (!NT_SUCCESS(RC)) {
                    try_return(RC);
                }
                // Note: FcbReference will be set when CCB is created in UDFCompleteFcbOpen
                PtrNewFcb->NtReqFCBFlags |= UDF_NTREQ_FCB_VALID;
                PtrNewFcb->FcbState |= UDF_FCB_VALID;

                UDFNotifyFullReportChange( Vcb, NewFileInfo->Fcb,
                                           UDFIsADirectory(NewFileInfo) ? FILE_NOTIFY_CHANGE_DIR_NAME : FILE_NOTIFY_CHANGE_FILE_NAME,
                                           FILE_ACTION_ADDED);

                // PHASE 1

                // we need to create Stream Dir
                RelatedFileInfo = NewFileInfo;
                RC = UDFCreateStreamDir__(IrpContext, Vcb, RelatedFileInfo, &NewFileInfo);
                if (!NT_SUCCESS(RC)) {
                    AdPrint(("    Can't create SDir\n"));
                    BrutePoint();
                    try_return(RC);
                }

                // normalize stream name
                StreamName.Buffer++;
                StreamName.Length-=sizeof(WCHAR);
                // Open the newly created object
                if (!(PtrNewFcb = NewFileInfo->Fcb)) {
                    // It is a first open operation
                    // Allocate new FCB
                    // Here we set FileObject pointer to NULL to avoid
                    // new CCB allocation
                    RC = UDFFirstOpenFile(IrpContext,
                                          IrpSp,
                                   Vcb,
                                   NULL, &PtrNewFcb, RelatedFileInfo, NewFileInfo,
                                   &LocalPath, &UdfData.UnicodeStrSDir, CreateDisposition);
                } else {
                    BrutePoint();
                }
                if (!NT_SUCCESS(RC)) {
                    AdPrint(("    Can't perform OpenFile operation for SDir\n"));
                    BrutePoint();
                    try_return(RC);
                }

                // Update state
                // FCB references are now handled by LCB in UDFCompleteFcbOpen
                LastGoodFileInfo = NewFileInfo;
                // Acquire FCB lock for tree traversal
                // Skip if same FCB to avoid recursive acquire leading to lock leak
                {
                    PFCB NewFcb = NewFileInfo->Fcb;
                    if (NewFcb != CurrentFcb) {
                        UDF_CHECK_PAGING_IO_RESOURCE(NewFcb);
                        UDFAcquireFcbExclusive(IrpContext, NewFcb, FALSE);
                        if (PreviousFcb && PreviousFcb != CurrentFcb) {
                            UDFReleaseResource(&PreviousFcb->FcbNonpaged->FcbResource);
                        }
                        PreviousFcb = CurrentFcb;
                        CurrentFcb = NewFcb;
                    }
                }
                // update FCB tree
                RC = MyAppendUnicodeStringToStringTag(&LocalPath, &(UdfData.UnicodeStrSDir), MEM_USLOC_TAG);
                if (!NT_SUCCESS(RC)) {
                    AdPrint(("    Can't append UNC str\n"));
                    BrutePoint();
                    try_return(RC);
                }
                // Note: FcbReference will be set when CCB is created in UDFCompleteFcbOpen
                PtrNewFcb->NtReqFCBFlags |= UDF_NTREQ_FCB_VALID;
                PtrNewFcb->FcbState |= UDF_FCB_VALID;

                // PHASE 2

                // create stream
                RelatedFileInfo = NewFileInfo;
                RC = UDFCreateFile__(IrpContext, Vcb, IgnoreCase, &StreamName, 0, 0,
                         UdfIsExtendedFESupported(Vcb), (CreateDisposition == FILE_CREATE),
                         RelatedFileInfo, &NewFileInfo);
                if (!NT_SUCCESS(RC)) {
                    AdPrint(("    Can't create Stream\n"));
                    BrutePoint();
                    try_return(RC);
                }

                // Update unwind information
                LastGoodTail = StreamName;
            }
            // NT wants ARCHIVE bit to be set on Files
            if (!DirectoryFile)
                FileAttributes |= FILE_ATTRIBUTE_ARCHIVE;
            // Open the newly created object
            if (!(PtrNewFcb = NewFileInfo->Fcb)) {
                // It is a first open operation
#ifndef IFS_40
                // Set attributes for the file ...
                UDFAttributesToUDF(UDFDirIndex(UDFGetDirIndexByFileInfo(NewFileInfo),NewFileInfo->Index),
                                   NewFileInfo->Dloc->FileEntry, FileAttributes);
#endif //IFS_40
                // Allocate new FCB
                // Here we set FileObject pointer to NULL to avoid
                // new CCB allocation
                RC = UDFFirstOpenFile(IrpContext,
                                      IrpSp,
                               Vcb,
                               FileObject, &PtrNewFcb, RelatedFileInfo, NewFileInfo,
                               &LocalPath, &LastGoodTail, CreateDisposition);
            } else {
                BrutePoint();
            }

            if (!NT_SUCCESS(RC)) {
                AdPrint(("    Can't perform OpenFile operation for file or stream\n"));
                BrutePoint();
                // Undo create - flush and unlink from disk
                if ((RC != STATUS_FILE_IS_A_DIRECTORY) &&
                   (RC != STATUS_NOT_A_DIRECTORY) &&
                   (RC != STATUS_ACCESS_DENIED)) {
                    UDFFlushFile__(IrpContext, Vcb, NewFileInfo);
                    UDFUnlinkFile__(IrpContext, Vcb, NewFileInfo, TRUE);
                }
                UDFCloseFile__(IrpContext, Vcb, NewFileInfo);
                try_return(RC);
            }

            PtrNewFcb->Header.FileSize.QuadPart =
            PtrNewFcb->Header.ValidDataLength.QuadPart = 0;
            if (AllocationSize) {
                // inform NT about size changes
                PtrNewFcb->Header.AllocationSize.QuadPart = AllocationSize;
                MmPrint(("    CcIsFileCached()\n"));
                if (CcIsFileCached(FileObject)) {
                     MmPrint(("    CcSetFileSizes()\n"));
                     BrutePoint();
                     CcSetFileSizes(FileObject, (PCC_FILE_SIZES)&PtrNewFcb->Header.AllocationSize);
                     PtrNewFcb->NtReqFCBFlags |= UDF_NTREQ_FCB_MODIFIED;
                }
            }

            // Update state
            // FCB references are now handled by LCB in UDFCompleteFcbOpen
            LastGoodFileInfo = NewFileInfo;
            // Acquire FCB lock for tree traversal
            // Skip if same FCB to avoid recursive acquire leading to lock leak
            {
                PFCB NewFcb = NewFileInfo->Fcb;
                if (NewFcb != CurrentFcb) {
                    UDF_CHECK_PAGING_IO_RESOURCE(NewFcb);
                    UDFAcquireFcbExclusive(IrpContext, NewFcb, FALSE);
                    if (PreviousFcb && PreviousFcb != CurrentFcb) {
                        UDFReleaseResource(&PreviousFcb->FcbNonpaged->FcbResource);
                    }
                    PreviousFcb = CurrentFcb;
                    CurrentFcb = NewFcb;
                }
            }

            // Set the Share Access for the file stream.
            // The FCBShareAccess field will be set by the I/O Manager.
            PtrNewCcb = UDFDecodeFileObjectCcb(FileObject);
            RC = UDFSetAccessRights(FileObject, AccessState, PtrNewFcb, PtrNewCcb, DesiredAccess, ShareAccess);

            if (!NT_SUCCESS(RC)) {
                AdPrint(("    Can't set Access Rights on Create\n"));
                BrutePoint();
                // Undo create - flush, unlink and close
                UDFFlushFile__(IrpContext, Vcb, NewFileInfo);
                UDFUnlinkFile__(IrpContext, Vcb, NewFileInfo, TRUE);
                UDFCloseFile__(IrpContext, Vcb, NewFileInfo);
                try_return(RC);
            }

#ifdef IFS_40
            // Set attributes for the file ...
            UDFAttributesToUDF(UDFDirIndex(UDFGetDirIndexByFileInfo(NewFileInfo),NewFileInfo->Index),
                               NewFileInfo->Dloc->FileEntry, FileAttributes);
            // It is rather strange for me, but NT requires us to allow
            // Create operation for r/o + WriteAccess, but denies all
            // the rest operations in this case. Thus, we should update
            // r/o flag in Fcb _after_ Access check :-/
            if (FileAttributes & FILE_ATTRIBUTE_READONLY)
                PtrNewFcb->FcbState |= UDF_FCB_READ_ONLY;
#endif //IFS_40
            // We call the notify package to report that the
            // we have added a stream.
            if (UDFIsAStream(NewFileInfo)) {
                UDFNotifyFullReportChange( Vcb, NewFileInfo->Fcb,
                                           FILE_NOTIFY_CHANGE_STREAM_NAME,
                                           FILE_ACTION_ADDED_STREAM );
            } else {
                UDFNotifyFullReportChange( Vcb, NewFileInfo->Fcb,
                                           UDFIsADirectory(NewFileInfo) ? FILE_NOTIFY_CHANGE_DIR_NAME : FILE_NOTIFY_CHANGE_FILE_NAME,
                                           FILE_ACTION_ADDED);
            }
/*#ifdef UDF_DBG
            {
                ULONG i;
                PDIR_INDEX_HDR hDirIndex = NewFileInfo->ParentFile->Dloc->DirIndex;

                for(i=0;DirIndex[i].FName.Buffer;i++) {
                    AdPrint(("%ws\n", DirIndex[i].FName.Buffer));
                }
            }
#endif*/
            ReturnedInformation = FILE_CREATED;

            try_return(RC);
        }

        } // end if (!SkipPathTraversal)

        // ****************
        // we have always STATUS_SUCCESS here
        // ****************
        ASSERT(NewFileInfo != OldRelatedFileInfo);

        RC = UDFOpenExistingFcb(IrpContext, IrpSp, Vcb, &PtrNewFcb, IgnoreCase, OpenByFileId, CreateDisposition);
        if (!NT_SUCCESS(RC)) try_return(RC);
        PtrNewCcb = UDFDecodeFileObjectCcb(FileObject);

        if (CreateDisposition == FILE_CREATE) {
            ReturnedInformation = FILE_EXISTS;
            AdPrint(("    Object name collision\n"));
            try_return(RC = STATUS_OBJECT_NAME_COLLISION);
        }

        PtrNewFcb->Header.IsFastIoPossible = UDFIsFastIoPossible(PtrNewFcb);

        // Check if caller wanted a directory only and target object
        //  is not a directory, or caller wanted a file only and target
        //  object is not a file ...
        if ((PtrNewFcb->FcbState & UDF_FCB_DIRECTORY) && ((CreateDisposition == FILE_SUPERSEDE) ||
              (CreateDisposition == FILE_OVERWRITE) || (CreateDisposition == FILE_OVERWRITE_IF) ||
              NonDirectoryFile)) {
            if (NonDirectoryFile) {
                AdPrint(("    Can't open directory as a plain file\n"));
            } else {
                AdPrint(("    Can't supersede directory\n"));
            }
            RC = STATUS_FILE_IS_A_DIRECTORY;
            try_return(RC);
        }

        if (DirectoryFile && !(PtrNewFcb->FcbState & UDF_FCB_DIRECTORY)) {
            AdPrint(("    This is not a directory\n"));
            RC = STATUS_NOT_A_DIRECTORY;
            try_return(RC);
        }

        if (DeleteOnClose && (PtrNewFcb->FcbState & UDF_FCB_READ_ONLY)) {
            AdPrint(("    Can't delete Read-Only file\n"));
            RC = STATUS_CANNOT_DELETE;
            try_return(RC);
        }
        // Check share access and fail if the share conflicts with an existing
        // open.
        ASSERT(CurrentFcb);
        RC = UDFCheckAccessRights(FileObject, AccessState, PtrNewFcb, PtrNewCcb, DesiredAccess, ShareAccess);
        if (!NT_SUCCESS(RC)) {
            AdPrint(("    Access/Share access check failed\n"));
            try_return(RC);
        }

        RestoreShareAccess = TRUE;

        if (NonDirectoryFile) {
            //  If the user wants 'write access' access to the file make sure there
            //  is not a process mapping this file as an image.  Any attempt to
            //  delete the file will be stopped in fileinfo.cpp
            //
            //  If the user wants to delete on close, we must check at this
            //  point though.
            if ((DesiredAccess & FILE_WRITE_DATA) || DeleteOnClose) {

                MmPrint(("    MmFlushImageSection();\n"));

                if (!MmFlushImageSection(&PtrNewFcb->FcbNonpaged->SegmentObject, MmFlushForWrite)) {

                    RC = DeleteOnClose ? STATUS_CANNOT_DELETE :
                                                  STATUS_SHARING_VIOLATION;
                    AdPrint(("    File is mapped or deletion in progress\n"));
                    try_return (RC);
                }
            }

            if (FlagOn(Options, FILE_NO_INTERMEDIATE_BUFFERING) &&
                /*  (PtrNewFileObject->Flags & FO_NO_INTERMEDIATE_BUFFERING) &&*/
               !(PtrNewFcb->CachedOpenHandleCount) &&
                (PtrNewFcb->FcbNonpaged->SegmentObject.DataSectionObject) ) {
                //  If this is a non-cached open, and there are no open cached
                //  handles, but there is still a data section, attempt a flush
                //  and purge operation to avoid cache coherency overhead later.
                //  We ignore any I/O errors from the flush.
                MmPrint(("    CcFlushCache()\n"));
                CcFlushCache(&PtrNewFcb->FcbNonpaged->SegmentObject, NULL, 0, NULL);
                MmPrint(("    CcPurgeCacheSection()\n"));
                CcPurgeCacheSection(&PtrNewFcb->FcbNonpaged->SegmentObject, NULL, 0, FALSE);
            }
        }

        if (DeleteOnClose && UDFIsADirectory(NewFileInfo) && !UDFIsDirEmpty__(NewFileInfo)) {
            AdPrint(("    Directory in not empry\n"));
            try_return (RC = STATUS_DIRECTORY_NOT_EMPTY);
        }

        // Get attributes for the file ...
        TmpFileAttributes =
            (USHORT)UDFAttributesToNT(UDFDirIndex(UDFGetDirIndexByFileInfo(NewFileInfo), NewFileInfo->Index),
                               NewFileInfo->Dloc->FileEntry);

        if (DeleteOnClose &&
           (TmpFileAttributes & FILE_ATTRIBUTE_READONLY)) {
            ASSERT(CurrentFcb);
            RC = UDFCheckAccessRights(NULL, NULL, OldRelatedFileInfo->Fcb, RelatedCcb, FILE_DELETE_CHILD, 0);
            if (!NT_SUCCESS(RC)) {
                AdPrint(("    Read-only. DeleteOnClose attempt failed\n"));
                try_return (RC = STATUS_CANNOT_DELETE);
            }
        }

        // If a supersede or overwrite was requested, do so now ...
        if ((CreateDisposition == FILE_SUPERSEDE) ||
           (CreateDisposition == FILE_OVERWRITE) ||
           (CreateDisposition == FILE_OVERWRITE_IF)) {
            // Attempt the operation here ...

            ASSERT(!UDFIsADirectory(NewFileInfo));

            if (CreateDisposition == FILE_SUPERSEDE) {
                BOOLEAN RestoreRO = FALSE;

                ASSERT(CurrentFcb);
                // NT wants us to allow Supersede on RO files
                if (PtrNewFcb->FcbState & UDF_FCB_READ_ONLY) {
                    // Imagine, that file is not RO and check other permissions
                    RestoreRO = TRUE;
                    PtrNewFcb->FcbState &= ~UDF_FCB_READ_ONLY;
                }
                RC = UDFCheckAccessRights(NULL, NULL, PtrNewFcb, PtrNewCcb, DELETE, 0);
                if (RestoreRO) {
                    // Restore RO state if changed
                    PtrNewFcb->FcbState |= UDF_FCB_READ_ONLY;
                }
                if (!NT_SUCCESS(RC)) {
                    AdPrint(("    Can't supersede. DELETE permission required\n"));
                    try_return (RC);
                }
            } else {
                ASSERT(CurrentFcb);
                RC = UDFCheckAccessRights(NULL, NULL, PtrNewFcb, PtrNewCcb,
                            FILE_WRITE_DATA | FILE_WRITE_EA | FILE_WRITE_ATTRIBUTES, 0);
                if (!NT_SUCCESS(RC)) {
                    AdPrint(("    Can't overwrite. Permission denied\n"));
                    try_return (RC);
                }
            }
            // Existing & requested System and Hidden bits must match
            if ( (TmpFileAttributes & (FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM)) &
                (FileAttributes ^ (FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM)) ) {
                AdPrint(("    The Hidden and/or System bits do not match\n"));
                try_return(RC = STATUS_ACCESS_DENIED);
            }

            ASSERT(CurrentFcb);

            // Truncate file and set attributes (acquires PagingIoResource internally, checks MmCanFileBeTruncated)
            RC = UDFSupersedeOrOverwriteFile(
                IrpContext,
                FileObject,
                Vcb,
                PtrNewFcb,
                NewFileInfo,
                AllocationSize,
                FileAttributes,
                (BOOLEAN)(CreateDisposition == FILE_SUPERSEDE)
            );
            if (!NT_SUCCESS(RC)) {
                try_return(RC);
            }

            ReturnedInformation = (CreateDisposition == FILE_SUPERSEDE) ? FILE_SUPERSEDED : FILE_OVERWRITTEN;
            // notify changes
            UDFNotifyFullReportChange( Vcb, NewFileInfo->Fcb,
                                       FILE_NOTIFY_CHANGE_LAST_WRITE | FILE_NOTIFY_CHANGE_ATTRIBUTES | FILE_NOTIFY_CHANGE_SIZE,
                                       FILE_ACTION_MODIFIED);

            // Update parent object
            if ((Vcb->CompatFlags & UDF_VCB_IC_UPDATE_DIR_WRITE) &&
               NextFcb &&
               RelatedFileObject &&
               (NextFcb->FileInfo == NewFileInfo->ParentFile)) {
                    RelatedFileObject->Flags |= (FO_FILE_MODIFIED | FO_FILE_SIZE_CHANGED);
            }
        } else {
            ReturnedInformation = FILE_OPENED;
        }

        // Update parent object
        if ((Vcb->CompatFlags & UDF_VCB_IC_UPDATE_DIR_READ) &&
           NextFcb &&
           RelatedFileObject &&
           (NextFcb->FileInfo == NewFileInfo->ParentFile)) {
                RelatedFileObject->Flags |= FO_FILE_FAST_IO_READ;
        }

try_exit:   NOTHING;

    } _SEH2_FINALLY {
        // Complete the request unless we are here as part of unwinding
        //  when an exception condition was encountered, OR
        //  if the request has been deferred (i.e. posted for later handling)

        if (RC != STATUS_PENDING) {
            // If any intermediate (directory) open operations were performed,
            //  implement the corresponding close (do *not* however close
            //  the target we have opened on behalf of the caller ...).

            if (NT_SUCCESS(RC) && PtrNewFcb) {
                // Update the file object such that:
                //  (a) the FsContext field points to the NTRequiredFCB field
                //       in the FCB
                //  (b) the FsContext2 field points to the CCB created as a
                //       result of the open operation

                // If write-through was requested, then mark the file object
                //  appropriately

                // directories are not cached
                // so we should prevent flush attepmts on cleanup
                if (!(PtrNewFcb->FcbState & UDF_FCB_DIRECTORY)) {

                    if (SequentialOnly &&
                       !(Vcb->CompatFlags & UDF_VCB_IC_IGNORE_SEQUENTIAL_IO)) {
                        FileObject->Flags |= FO_SEQUENTIAL_ONLY;
                        MmPrint(("        FO_SEQUENTIAL_ONLY\n"));

                        if (Vcb->TargetDeviceObject->Characteristics & FILE_REMOVABLE_MEDIA) {
                            FileObject->Flags &= ~FO_WRITE_THROUGH;
                            MmPrint(("        FILE_REMOVABLE_MEDIA + FO_SEQUENTIAL_ONLY => ~FO_WRITE_THROUGH\n"));
                        }

                        if (PtrNewFcb->FileInfo) {
                            UDFSetFileAllocMode__(PtrNewFcb->FileInfo, EXTENT_FLAG_ALLOC_SEQUENTIAL);
                        }
                    }
                    if (FlagOn(Options, FILE_NO_INTERMEDIATE_BUFFERING)) {

                        FileObject->Flags |= FO_NO_INTERMEDIATE_BUFFERING;
                        MmPrint(("        FO_NO_INTERMEDIATE_BUFFERING\n"));

                    } else {

                        FileObject->Flags |= FO_CACHE_SUPPORTED;
                        MmPrint(("        FO_CACHE_SUPPORTED\n"));
                    }
                }

                if ((DesiredAccess & FILE_EXECUTE) /*&&
                   !(PtrNewFcb->FCBFlags & UDF_FCB_DIRECTORY)*/) {
                    MmPrint(("        FO_FILE_FAST_IO_READ\n"));
                    FileObject->Flags |= FO_FILE_FAST_IO_READ;
                }

                //  Update the open and cleanup counts.  Check the fast io state here.

                UDFLockVcb(IrpContext, Vcb);

                UDFIncrementCleanupCounts(IrpContext, PtrNewFcb);
                UDFIncrementReferenceCounts(IrpContext, PtrNewFcb, 1, 1);

                if (FileObject->Flags & FO_CACHE_SUPPORTED)
                    InterlockedIncrement((PLONG)&PtrNewFcb->CachedOpenHandleCount);

                UDFUnlockVcb(IrpContext, Vcb);

                // Store some flags in CCB
                if (PtrNewCcb) {
                    // delete on close
                    if (DeleteOnClose) {
                        ASSERT(!(PtrNewFcb->FcbState & UDF_FCB_ROOT_DIRECTORY));
                        PtrNewCcb->Flags |= UDF_CCB_DELETE_ON_CLOSE;
                    }

                    // case sensetivity
                    if (IgnoreCase) {
                        // remember this for possible Rename/Move operation
                        PtrNewCcb->Flags |= CCB_FLAG_IGNORE_CASE;
                    }
                } else {
                    BrutePoint();
                }
                // it was a stream...
                if (StreamOpen)
                    FileObject->Flags |= FO_STREAM_FILE;
//                PtrNewCcb->CCBFlags |= UDF_CCB_VALID;
                // increment the number of outstanding open operations on this
                // logical volume (i.e. volume cannot be dismounted)
                InterlockedIncrement((PLONG)&Vcb->VcbReference);
                PtrNewFcb->NtReqFCBFlags |= UDF_NTREQ_FCB_VALID;
                PtrNewFcb->FcbState |= UDF_FCB_VALID;
                // Note: With LCB model, FcbReference may not always be >= RefCount
                // for intermediate files (e.g., parent of a stream)
                AdPrint(("    FCB %x, CCB %x, FO %x, Flags %x\n", PtrNewFcb, PtrNewCcb, FileObject, PtrNewFcb->FcbState));

            } else if (!NT_SUCCESS(RC)) {
                // Perform failure related post-processing now
                if (RestoreShareAccess && PtrNewFcb && FileObject) {
                    IoRemoveShareAccess(FileObject, &PtrNewFcb->ShareAccess);
                }

                if (PtrNewCcb) {
                    UDFDeleteCcb(PtrNewCcb);
                }

                if (FileObject) {
                    FileObject->FsContext2 = NULL;
                }
                // We have successfully opened LastGoodFileInfo,
                // so mark it as VALID to avoid future troubles...
                if (LastGoodFileInfo && LastGoodFileInfo->Fcb) {
                    LastGoodFileInfo->Fcb->FcbState |= UDF_FCB_VALID;
                    if (LastGoodFileInfo->Fcb) {
                        LastGoodFileInfo->Fcb->NtReqFCBFlags |= UDF_NTREQ_FCB_VALID;
                    }
                }
                // cleanup FCBs (if any)
                // Note: UDFTeardownStructures calls UDFCloseFile__ when CallCloseFile=TRUE
                // (CloseFileInfoChain is NOT called in Create finally)
                // Note: UDFTeardownStructures releases lock when FCB is removed
                ASSERT(!LastGoodFileInfo || !LastGoodFileInfo->Fcb || CurrentFcb == LastGoodFileInfo->Fcb);
                if (  Vcb && (PtrNewFcb != Vcb->RootIndexFcb) &&
                     LastGoodFileInfo ) {
                    //
                    // LOCK LEAK FIX:
                    // TeardownStructures walks up the FCB tree and acquires parent locks.
                    // If we hold PreviousFcb (= CurrentFcb->ParentFcb), TeardownStructures will:
                    //   - Recursively acquire it (OwnerCount: 1->2)
                    //   - Process and release (OwnerCount: 2->1)
                    // Our original lock is still held (count=1).
                    //
                    // Old bug: Code set PreviousFcb=NULL when RemovedFcb=TRUE, leaking our lock.
                    //
                    // Fix: Protect PreviousFcb from deletion via FcbReference++.
                    // Using FcbReference (not FcbCleanup) because TeardownStructures
                    // may decrement parent FcbReferences via LCB removal.
                    // If we used FcbCleanup++, ASSERT(FcbCleanup <= FcbReference) would fail
                    // when FcbReference is decremented to 0 but FcbCleanup is still 1.
                    // After TeardownStructures, decrement FcbReference.
                    // Let finally block release our lock normally.
                    // DON'T set PreviousFcb=NULL!
                    //
                    BOOLEAN ProtectedPreviousFcb = FALSE;
                    if (PreviousFcb && PreviousFcb != CurrentFcb) {
                        UDFLockVcb(IrpContext, Vcb);
                        PreviousFcb->FcbReference++;
                        UDFUnlockVcb(IrpContext, Vcb);
                        ProtectedPreviousFcb = TRUE;
                    }

                    BOOLEAN RemovedFcb = FALSE;
                    // LCB-based teardown: walks ParentLcbQueue to find and remove LCBs
                    UDFTeardownStructures(IrpContext, LastGoodFileInfo->Fcb, FALSE, &RemovedFcb);
                    // If FCB was removed, lock is already released by TeardownStructures
                    if (RemovedFcb) {
                        CurrentFcb = NULL;
                    }

                    // Unprotect PreviousFcb
                    if (ProtectedPreviousFcb) {
                        UDFLockVcb(IrpContext, Vcb);
                        PreviousFcb->FcbReference--;
                        UDFUnlockVcb(IrpContext, Vcb);
                    }
                    // PreviousFcb lock will be released by finally block below
                } else {
                    ASSERT(!LastGoodFileInfo);
                }
            }
        }

        // Release parent lock first (PreviousFcb before CurrentFcb)
        if (PreviousFcb && PreviousFcb != CurrentFcb) {
            UDFReleaseResource(&PreviousFcb->FcbNonpaged->FcbResource);
        }

        // Release current lock
        if (CurrentFcb) {
            UDFReleaseResource(&CurrentFcb->FcbNonpaged->FcbResource);
        }

        if (RC != STATUS_PENDING && !_SEH2_AbnormalTermination()) {

            Irp->IoStatus.Information = ReturnedInformation;

            UDFCompleteRequest(IrpContext, Irp, RC);
        }

        // free allocated tmp buffers (if any)
        if (AbsolutePathName.Buffer)
            MyFreePool__(AbsolutePathName.Buffer);
        if (LocalPath.Buffer)
            MyFreePool__(LocalPath.Buffer);
        if (TailNameBuffer)
            MyFreePool__(TailNameBuffer);

        // Release the Vcb.

        UDFReleaseVcb(IrpContext, Vcb);

    } _SEH2_END;

    return(RC);
} // end UDFCommonCreate()

/*************************************************************************
*
* Function: UDFFirstOpenFile()
*
* Description:
*   Perform first Open/Create initialization.
*
* Expected Interrupt Level (for execution) :
*
*  IRQL_PASSIVE_LEVEL
*
* Return Value: STATUS_SUCCESS/Error
*
*************************************************************************/
NTSTATUS
UDFFirstOpenFile(
    IN PIRP_CONTEXT IrpContext,
    IN PIO_STACK_LOCATION IrpSp,
    IN PVCB Vcb,                // volume control block
    IN PFILE_OBJECT PtrNewFileObject,   // I/O Mgr. created file object
   OUT PFCB* PtrNewFcb,
    IN PUDF_FILE_INFO RelatedFileInfo,
    IN PUDF_FILE_INFO NewFileInfo,
    IN PUNICODE_STRING LocalPath,
    IN PUNICODE_STRING CurName,
    IN ULONG CreateDisposition
    )
{
//    DIR_INDEX           NewFileIndex;
    PtrUDFObjectName    NewFCBName;
    NTSTATUS            RC;
    BOOLEAN             Linked = TRUE;
    PDIR_INDEX_HDR      hDirIndex;
    PDIR_INDEX_ITEM     DirIndex;
    FILE_ID FileId;
    TYPE_OF_OPEN TypeOfOpen;
    NODE_TYPE_CODE NodeTypeCode;
    BOOLEAN FcbExisted;

    ASSERT(RelatedFileInfo);

    AdPrint(("UDFFirstOpenFile\n"));

    FileId = UDFGetNTFileId(Vcb, NewFileInfo);

    if (UDFIsADirectory(NewFileInfo)) {

        TypeOfOpen = UserDirectoryOpen;
        NodeTypeCode = UDF_NODE_TYPE_INDEX;

        SetFlag(FileId.HighPart, FID_DIR_MASK);
    
    } else {
        
        TypeOfOpen = UserFileOpen;
        NodeTypeCode = UDF_NODE_TYPE_DATA;
    }

    if (!((*PtrNewFcb) = UDFCreateFcb(IrpContext, FileId, NodeTypeCode, &FcbExisted))) {

        AdPrint(("Can't allocate FCB\n"));
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    if (FcbExisted) {

        // Make link between Fcb and FileInfo
        (*PtrNewFcb)->FileInfo = NewFileInfo;
        NewFileInfo->Fcb = (*PtrNewFcb);
        (*PtrNewFcb)->ParentFcb = RelatedFileInfo->Fcb;

        return STATUS_SUCCESS;

    }

    // Allocate and set new FCB unique name (equal to absolute path name)
    if (!(NewFCBName = UDFAllocateObjectName())) return STATUS_INSUFFICIENT_RESOURCES;

    if (RelatedFileInfo && RelatedFileInfo->Fcb &&
       !(RelatedFileInfo->Fcb->FcbState & UDF_FCB_ROOT_DIRECTORY)) {
        RC = MyCloneUnicodeString(&(NewFCBName->ObjectName), &(RelatedFileInfo->Fcb->FCBName->ObjectName));
    } else {
        RC = MyInitUnicodeString(&(NewFCBName->ObjectName), L"");
    }
    if (!NT_SUCCESS(RC))
        return STATUS_INSUFFICIENT_RESOURCES;
    if ( (CurName->Buffer[0] != L':') &&
        (!LocalPath->Length ||
            ((LocalPath->Buffer[LocalPath->Length/sizeof(WCHAR)-1] != L':') /*&&
             (LocalPath->Buffer[LocalPath->Length/sizeof(WCHAR)-1] != L'\\')*/) )) {
        RC = MyAppendUnicodeToString(&(NewFCBName->ObjectName), L"\\");
        if (!NT_SUCCESS(RC)) {
            UDFReleaseObjectName(NewFCBName);
            return STATUS_INSUFFICIENT_RESOURCES;
        }
    }

    // Make link between Fcb and FileInfo
    (*PtrNewFcb)->FileInfo = NewFileInfo;
    NewFileInfo->Fcb = (*PtrNewFcb);
    (*PtrNewFcb)->ParentFcb = RelatedFileInfo->Fcb;

    if (!NewFileInfo->Dloc->CommonFcb) {
        (*PtrNewFcb)->FileInfo->Dloc->CommonFcb = (*PtrNewFcb);
        Linked = FALSE;
    } else {
        ASSERT(FALSE);
        if (!(NewFileInfo->Dloc->CommonFcb->NtReqFCBFlags & UDF_NTREQ_FCB_VALID)) {
            BrutePoint();
            UDFReleaseObjectName(NewFCBName);
            return STATUS_ACCESS_DENIED;
        }
    }

    PFCB NewFcb = *PtrNewFcb;
    // Set times
    if (!Linked) {
        UDFGetFileXTime((*PtrNewFcb)->FileInfo,
            &(NewFcb->CreationTime.QuadPart),
            &(NewFcb->LastAccessTime.QuadPart),
            &(NewFcb->ChangeTime.QuadPart),
            &(NewFcb->LastWriteTime.QuadPart) );

        // Set the allocation size for the object is specified
        NewFcb->Header.AllocationSize.QuadPart =
            UDFSysGetAllocSize(Vcb, NewFileInfo->Dloc->DataLoc.Length);
//        NewFcb->Header.AllocationSize.QuadPart = UDFGetFileAllocationSize(Vcb, NewFileInfo);
        NewFcb->Header.FileSize.QuadPart =
        NewFcb->Header.ValidDataLength.QuadPart = NewFileInfo->Dloc->DataLoc.Length;
    }
    // begin transaction
    UDFLockVcb(IrpContext, Vcb);

    RC = UDFInitializeFCB(*PtrNewFcb, Vcb, NewFCBName,
                 UDFIsADirectory(NewFileInfo) ? UDF_FCB_DIRECTORY : 0, PtrNewFileObject);

    if (!NT_SUCCESS(RC)) {

        UDFUnlockVcb(IrpContext, Vcb);
        return RC;
    }
    // Set embedded data flag if file data is stored in ICB
    if (!UDFIsADirectory(NewFileInfo) &&
        (((PFILE_ENTRY)(NewFileInfo->Dloc->FileEntry))->icbTag.flags & ICB_FLAG_ALLOC_MASK) == ICB_FLAG_AD_IN_ICB) {
        (*PtrNewFcb)->FcbState |= UDF_FCB_EMBEDDED_DATA;
    }
    // set Read-only attribute
    if (!UDFIsAStreamDir(NewFileInfo)) {
        hDirIndex = UDFGetDirIndexByFileInfo(NewFileInfo);
#ifdef UDF_DBG
        if (!hDirIndex) {
            BrutePoint();
        } else {
#endif // UDF_DBG
            if (UDFAttributesToNT(DirIndex = UDFDirIndex(hDirIndex, NewFileInfo->Index),NULL) & FILE_ATTRIBUTE_READONLY) {
                (*PtrNewFcb)->FcbState |= UDF_FCB_READ_ONLY;
            }
            MyAppendUnicodeStringToStringTag(&(NewFCBName->ObjectName), &(DirIndex->FName), MEM_USOBJ_TAG);
#ifdef UDF_DBG
        }
#endif // UDF_DBG
    } else if (RelatedFileInfo->ParentFile) {
        hDirIndex = UDFGetDirIndexByFileInfo(RelatedFileInfo);
        if (UDFAttributesToNT(DirIndex = UDFDirIndex(hDirIndex, RelatedFileInfo->Index),NULL) & FILE_ATTRIBUTE_READONLY) {
            (*PtrNewFcb)->FcbState |= UDF_FCB_READ_ONLY;
        }
        RC = MyAppendUnicodeStringToStringTag(&(NewFCBName->ObjectName), CurName, MEM_USOBJ_TAG);
//    } else {
//        BrutePoint();
    }
    // do not allocate CCB if it is internal Create/Open
    if (NT_SUCCESS(RC)) {
        if (PtrNewFileObject) {

            TYPE_OF_OPEN TypeOfOpen;

            if (UDFIsADirectory((*PtrNewFcb)->FileInfo)) {
                TypeOfOpen = UserDirectoryOpen;
            }
            else {
                TypeOfOpen = UserFileOpen;
            }

            RC = UDFCompleteFcbOpen(IrpContext, IrpSp, Vcb, PtrNewFcb, TypeOfOpen, 0, CreateDisposition);
        } else {
            RC = STATUS_SUCCESS;
        }
    }

    // Insert FCB into table only on success (matching MS pattern where
    // UdfInsertFcbTable is called at the END of initialization after
    // everything succeeds). This ensures FCB is not in table if error
    // occurs and UDFDeleteFcb is called from error path.
    if (NT_SUCCESS(RC)) {
        UDFInsertFcbIntoTable(IrpContext, *PtrNewFcb);
    }

    UDFUnlockVcb(IrpContext, Vcb);
    // end transaction

//    if (!NT_SUCCESS(RC)) return RC;

    return RC;
} // end UDFFirstOpenFile()

/*************************************************************************
*
* Function: UDFOpenFile()
*
* Description:
*   Open a file/dir for the caller.
*
* Expected Interrupt Level (for execution) :
*
*  IRQL_PASSIVE_LEVEL
*
* Return Value: STATUS_SUCCESS/Error
*
*************************************************************************/
_Requires_lock_held_(_Global_critical_region_)
NTSTATUS
UDFCompleteFcbOpen(
    _In_ PIRP_CONTEXT IrpContext,
    _In_ PIO_STACK_LOCATION IrpSp,
    _In_ PVCB Vcb,
    _Inout_ PFCB *CurrentFcb,
    _In_ TYPE_OF_OPEN TypeOfOpen,
    _In_ ULONG UserCcbFlags,
    _In_ ULONG CreateDisposition
    )
{
    NTSTATUS Status = STATUS_SUCCESS;
    BOOLEAN LockVolume = FALSE;
    PFCB Fcb = *CurrentFcb;
    PCCB Ccb = NULL;
    USHORT FileAttributes = IrpSp->Parameters.Create.FileAttributes;
    PIO_SECURITY_CONTEXT SecurityContext = IrpSp->Parameters.Create.SecurityContext;
    ACCESS_MASK AddedAccess = 0;
    ACCESS_MASK DesiredAccess = SecurityContext->DesiredAccess;
    BOOLEAN DeleteOnClose = BooleanFlagOn(IrpSp->Parameters.Create.Options, FILE_DELETE_ON_CLOSE);

    ASSERT_FCB(Fcb);

    // Expand maximum allowed to something sensible for share access checking

    if (DesiredAccess == MAXIMUM_ALLOWED) {

        if (FlagOn(Vcb->VcbState, VCB_STATE_VOLUME_READ_ONLY)) {

            DesiredAccess = FILE_ALL_ACCESS & ~((TypeOfOpen != UserVolumeOpen ?
                                                 (FILE_WRITE_ATTRIBUTES           |
                                                  FILE_WRITE_DATA                 |
                                                  FILE_WRITE_EA                   |
                                                  FILE_ADD_FILE                   |                     
                                                  FILE_ADD_SUBDIRECTORY           |
                                                  FILE_APPEND_DATA) : 0)          |
                                                FILE_DELETE_CHILD                 |
                                                DELETE                            |
                                                WRITE_DAC);
        } else {

            DesiredAccess = FILE_ALL_ACCESS & ~(WRITE_DAC | FILE_WRITE_EA);
        }

        SecurityContext->DesiredAccess = DesiredAccess;
    }

    if (CreateDisposition == FILE_SUPERSEDE) {

        SetFlag(AddedAccess, ~(DesiredAccess) & DELETE);
        SetFlag(DesiredAccess, DELETE);
        SecurityContext->DesiredAccess = DesiredAccess;

    } else if ((CreateDisposition == FILE_OVERWRITE) ||
               (CreateDisposition == FILE_OVERWRITE_IF)) {

        SetFlag(AddedAccess, ~DesiredAccess & (FILE_WRITE_DATA | FILE_WRITE_EA | FILE_WRITE_ATTRIBUTES));
        SetFlag(DesiredAccess, FILE_WRITE_DATA | FILE_WRITE_EA | FILE_WRITE_ATTRIBUTES);
        SecurityContext->DesiredAccess = DesiredAccess;
    }

    // If this a volume open and the user wants to lock the volume then
    // purge and lock the volume.

    if ((TypeOfOpen == UserVolumeOpen) &&
        !FlagOn( IrpSp->Parameters.Create.ShareAccess, FILE_SHARE_READ)) {

        // If there are open handles then fail this immediately.

        if (Vcb->VcbCleanup != 0) {

            return STATUS_SHARING_VIOLATION;
        }

        // If we can't wait then force this to be posted.

        if (!FlagOn(IrpContext->Flags, IRP_CONTEXT_FLAG_WAIT)) {

            UDFRaiseStatus(IrpContext, STATUS_CANT_WAIT);
        }

        LockVolume = TRUE;

        // Flush the volume and make sure all of the user references
        // are gone.

        Status = UDFFlushVolume(IrpContext, Vcb);

        if (!NT_SUCCESS(Status)) {

            return Status;
        }

        // Now force all of the delayed close operations to go away.

        UDFFspClose(Vcb);

        if (Vcb->VcbUserReference > Vcb->VcbResidualUserReference) {

            return STATUS_SHARING_VIOLATION;
        }
    }

    if (CreateDisposition == FILE_CREATE) {

        if (FileAttributes & FILE_ATTRIBUTE_TEMPORARY) {

            Fcb->FcbState |= FCB_STATE_TEMPORARY;
            Fcb->FileAttributes |= FILE_ATTRIBUTE_TEMPORARY;
        }
    }
    else {

        if ((Fcb->FileAttributes & FILE_ATTRIBUTE_READONLY) &&
            (Fcb->Header.NodeTypeCode != UDF_NODE_TYPE_INDEX) &&
            (DesiredAccess & (FILE_WRITE_DATA | FILE_APPEND_DATA))) {

            //return STATUS_ACCESS_DENIED;
        }

        if (DeleteOnClose && (Fcb->FileAttributes & FILE_ATTRIBUTE_READONLY)) {

            //return STATUS_CANNOT_DELETE;
        }
    }

    _SEH2_TRY {

        // create a new CCB structure
        if (!(Ccb = UDFCreateCcb())) {
            AdPrint(("Can't allocate CCB\n"));
            IrpSp->FileObject->FsContext2 = NULL;
            //
            InterlockedIncrement((PLONG)&Fcb->FcbReference);
            Status = STATUS_INSUFFICIENT_RESOURCES;
            try_return(Status);
        }

        if (AddedAccess) {

            ClearFlag(DesiredAccess, AddedAccess);
            SecurityContext->DesiredAccess = DesiredAccess;
        }

        // Update the share access.

        if (Fcb->FcbCleanup == 0) {

            IoSetShareAccess(DesiredAccess,
                             IrpSp->Parameters.Create.ShareAccess,
                             IrpSp->FileObject,
                             &Fcb->ShareAccess);

        } else {

            IoUpdateShareAccess(IrpSp->FileObject, &Fcb->ShareAccess);
        }

        // initialize the CCB
        Ccb->Fcb = Fcb;
        // initialize the CCB to point to the file object
        Ccb->FileObject = IrpSp->FileObject;

        // Acquire or create LCB to link parent directory FCB to this file FCB
        // Skip for root directory (no parent)
        // UDFAcquirePrefix will find existing LCB and increment Reference,
        // or create new LCB and increment parent's FcbReference
        if (Fcb->ParentFcb) {
            Ccb->Lcb = UDFAcquirePrefix(IrpContext,
                                        Fcb->ParentFcb,
                                        Fcb,
                                        Fcb->FileInfo ? Fcb->FileInfo->Index : 0);
        }

        // Set the file object type.
        UDFSetFileObject(IrpSp->FileObject, TypeOfOpen, Fcb, Ccb);

        //UDFLockFcb(IrpContext, Fcb);

        if (TypeOfOpen == UserFileOpen) {

            Fcb->Header.IsFastIoPossible = UDFIsFastIoPossible(Fcb);

        } else {

            Fcb->Header.IsFastIoPossible = FastIoIsNotPossible;
        }

        //UDFUnlockFcb(IrpContext, Fcb);

        // Point to the section object pointer in the non-paged Fcb.
        IrpSp->FileObject->SectionObjectPointer = &Fcb->FcbNonpaged->SegmentObject;

#ifdef UDF_DELAYED_CLOSE
        Fcb->FcbState &= ~UDF_FCB_DELAY_CLOSE;
#endif //UDF_DELAYED_CLOSE

        UDFAcquireResourceExclusive(&Fcb->FcbNonpaged->CcbListResource, TRUE);
        // insert CCB into linked list of open file object to Fcb or
        // to Vcb and do other intialization
        InsertTailList(&Fcb->NextCCB, &Ccb->NextCCB);
        InterlockedIncrement((PLONG)&Fcb->FcbReference);
        UDFReleaseResource(&Fcb->FcbNonpaged->CcbListResource);

        Ccb = NULL;

try_exit:   NOTHING;
    } _SEH2_FINALLY {

        if (Ccb) {
            // CCB was allocated but create failed - clean up
            if (Ccb->Lcb) {
                UDFRemovePrefix(IrpContext, Ccb->Lcb);
                Ccb->Lcb = NULL;
            }
            UDFReleaseCCB(Ccb);
        }

    } _SEH2_END;

    return Status;
} // end UDFOpenFile()

_When_(RelatedTypeOfOpen != UnopenedFileObject, _At_(RelatedCcb, _In_))
_When_(RelatedTypeOfOpen == UnopenedFileObject, _At_(RelatedCcb, _In_opt_))
_When_(RelatedTypeOfOpen != UnopenedFileObject, _At_(RelatedFileName, _In_))
_When_(RelatedTypeOfOpen == UnopenedFileObject, _At_(RelatedFileName, _In_opt_))
NTSTATUS
UDFNormalizeFileNames(
    _Inout_ PIRP_CONTEXT IrpContext,
    _In_ PVCB Vcb,
    _In_ BOOLEAN OpenByFileId,
    _In_ TYPE_OF_OPEN RelatedTypeOfOpen,
    PCCB RelatedCcb,
    PUNICODE_STRING RelatedFileName,
    _Inout_ PUNICODE_STRING FileName,
    _Inout_ PUNICODE_STRING RemainingName
    )

/*++

Routine Description:

    This routine is called to store the full name and upcased name into the
    filename buffer.  We only upcase the portion yet to parse.  We also
    check for a trailing backslash and lead-in double backslashes.  This
    routine also verifies the mode of the related open against the name
    currently in the filename.

Arguments:

    Vcb - Vcb for this volume.

    OpenByFileId - Indicates if the filename should be a 64 bit FileId.

    IgnoreCase - Indicates if this open is a case-insensitive operation.

    RelatedTypeOfOpen - Indicates the type of the related file object.

    RelatedCcb - Ccb for the related open.  Ignored if no relative open.

    RelatedFileName - FileName buffer for related open.  Ignored if no
        relative open.

    FileName - FileName to update in this routine.  The name should
        either be a 64-bit FileId or a Unicode string.

    RemainingName - Name with the remaining portion of the name.  This
        will begin after the related name and any separator.  For a
        non-relative open we also step over the initial separator.

Return Value:

    NTSTATUS - STATUS_SUCCESS if the names are OK, appropriate error code
        otherwise.

--*/

{
    ULONG RemainingNameLength = 0;
    ULONG RelatedNameLength = 0;
    ULONG SeparatorLength = 0;
    BOOLEAN HasColon = FALSE;

    ULONG BufferLength;

    UNICODE_STRING NewFileName;

    PAGED_CODE();

    return STATUS_SUCCESS;

    // If this is the first pass then we need to build the full name and
    // check for name compatibility.

    if (!FlagOn(IrpContext->Flags, IRP_CONTEXT_FLAG_FULL_NAME)) {

        //  Deal with the regular file name case first.

        if (!OpenByFileId) {

            //  This is here because the Win32 layer can't avoid sending me double
            //  beginning backslashes.

            if ((FileName->Length > sizeof( WCHAR )) &&
                (FileName->Buffer[1] == L'\\') &&
                (FileName->Buffer[0] == L'\\')) {

                //
                //  If there are still two beginning backslashes, the name is bogus.
                //

                if ((FileName->Length > 2 * sizeof( WCHAR )) &&
                    (FileName->Buffer[2] == L'\\')) {

                    return STATUS_OBJECT_NAME_INVALID;
                }

                //
                //  Slide the name down in the buffer.
                //

                FileName->Length -= sizeof( WCHAR );

                RtlMoveMemory( FileName->Buffer,
                               FileName->Buffer + 1,
                               FileName->Length );
            }

            //
            //  Check for a trailing backslash.  Don't strip off if only character
            //  in the full name or for relative opens where this is illegal.
            //

            if (((FileName->Length > sizeof( WCHAR)) ||
                 ((FileName->Length == sizeof( WCHAR )) && (RelatedTypeOfOpen == UserDirectoryOpen))) &&
                (FileName->Buffer[ (FileName->Length/2) - 1 ] == L'\\')) {

                SetFlag(IrpContext->Flags, IRP_CONTEXT_FLAG_TRAIL_BACKSLASH);
                FileName->Length -= sizeof( WCHAR );
            }

            //
            //  Remember the length we need for this portion of the name.
            //

            RemainingNameLength = FileName->Length;

            //
            //  If this is a related file object then we verify the compatibility
            //  of the name in the file object with the relative file object.
            //

            if (RelatedTypeOfOpen != UnopenedFileObject) {

                //
                //  If the filename length was zero then it must be legal.
                //  If there are characters then check with the related
                //  type of open.
                //

                if (FileName->Length != 0) {

                    //
                    //  The name length must always be zero for a volume open.
                    //

                    if (RelatedTypeOfOpen <= UserVolumeOpen) {

                        return STATUS_INVALID_PARAMETER;

                    // The remaining name cannot begin with a backslash.

                    } else if (FileName->Buffer[0] == L'\\') {

                        return STATUS_INVALID_PARAMETER;

                        // If the related file is a user file then there
                        // is no file with this path.

                    } else if (RelatedTypeOfOpen == UserFileOpen &&
                               Vcb->UdfRevision < 0x200) {

                        return STATUS_OBJECT_PATH_NOT_FOUND;

                    } else if (FileName->Buffer[0] == L':') {

                        HasColon = TRUE;
                    }
                }

                // Remember the length of the related name when building
                // the full name.  We leave the RelatedNameLength and
                // SeparatorLength at zero if the relative file is opened
                // by Id.

                if (!FlagOn(RelatedCcb->Flags, CCB_FLAG_OPEN_BY_ID)) {

                    // Add a separator if the name length is non-zero
                    // unless the relative Fcb is at the root.

                    if ((FileName->Length != 0) &&
                        (RelatedCcb->Fcb != Vcb->RootIndexFcb)) {

                        if (!HasColon) {
                            SeparatorLength = sizeof(WCHAR);
                        }
                    }

                    RelatedNameLength = RelatedFileName->Length;
                }

            //  The full name is already in the filename.  It must either
            //  be length 0 or begin with a backslash.

            } else if (FileName->Length != 0) {

                if (FileName->Buffer[0] != L'\\') {

                    return STATUS_INVALID_PARAMETER;
                }

                //
                //  We will want to trim the leading backslash from the
                //  remaining name we return.
                //

                RemainingNameLength -= sizeof(WCHAR);
                SeparatorLength = sizeof(WCHAR);
            }

            //  Now see if the buffer is large enough to hold the full name.

            BufferLength = RelatedNameLength + SeparatorLength + RemainingNameLength;

            //  Check for an overflow of the maximum filename size.

            if (BufferLength > MAXUSHORT) {

                return STATUS_INVALID_PARAMETER;
            }

            //  Now see if we need to allocate a new buffer.

            if (FileName->MaximumLength < BufferLength) {

                NewFileName.Buffer = (PWCH)FsRtlAllocatePoolWithTag(PagedPool,
                                                                    BufferLength,
                                                                    TAG_FILE_NAME);

                NewFileName.MaximumLength = (USHORT) BufferLength;

            } else {

                NewFileName.Buffer = FileName->Buffer;
                NewFileName.MaximumLength = FileName->MaximumLength;
            }

            //  If there is a related name then we need to slide the remaining bytes up and
            //  insert the related name.  Otherwise the name is in the correct position
            //  already.

            if (RelatedNameLength != 0) {

                //
                //  Store the remaining name in its correct position.
                //

                if (RemainingNameLength != 0) {

                    RtlMoveMemory(Add2Ptr( NewFileName.Buffer, RelatedNameLength + SeparatorLength, PVOID),
                                  FileName->Buffer,
                                  RemainingNameLength);
                }

                RtlCopyMemory( NewFileName.Buffer,
                               RelatedFileName->Buffer,
                               RelatedNameLength );

                //
                //  Add the separator if needed.
                //

                if (SeparatorLength != 0) {

                    WCHAR separatorChar = (RelatedTypeOfOpen == UserDirectoryOpen) ? L'\\' : L':';
                    *(Add2Ptr(NewFileName.Buffer, RelatedNameLength, PWCHAR)) = separatorChar;
                }

                // Update the filename value we got from the user.

                if (NewFileName.Buffer != FileName->Buffer) {

                    if (FileName->Buffer != NULL) {

                        UDFFreePool((PVOID*)&FileName->Buffer);
                    }

                    FileName->Buffer = NewFileName.Buffer;
                    FileName->MaximumLength = NewFileName.MaximumLength;
                }

                //
                //  Copy the name length to the user's filename.
                //

                FileName->Length = (USHORT)(RelatedNameLength + SeparatorLength + RemainingNameLength);
            }

            // Now update the remaining name to parse.

            RemainingName->MaximumLength =
            RemainingName->Length = (USHORT)RemainingNameLength;

            RemainingName->Buffer = Add2Ptr(FileName->Buffer,
                                            RelatedNameLength + SeparatorLength,
                                            PWCHAR);

            //  For the open by file Id case we verify the name really contains
            //  a 64 bit value.

        } else {

            //
            //  Check for validity of the buffer.
            //

            if (FileName->Length != sizeof( FILE_ID )) {

                return STATUS_INVALID_PARAMETER;
            }
        }

        SetFlag(IrpContext->Flags, IRP_CONTEXT_FLAG_FULL_NAME);

        // If we are in the retry path then the full name is already in the
        // file object name.  If this is a case-sensitive operation then
        // we need to upcase the name from the end of any related file name already stored
        // there.

    } else {

        // Assume there is no relative name.

        *RemainingName = *FileName;

        // Nothing to do if the name length is zero.

        if (RemainingName->Length != 0) {

            //  If there is a relative name then we need to walk past it.

            if (RelatedTypeOfOpen != UnopenedFileObject) {

                // Nothing to walk past if the RelatedCcb is opened by FileId.

                if (!FlagOn( RelatedCcb->Flags, CCB_FLAG_OPEN_BY_ID )) {

                    //  Related file name is a proper prefix of the full name.
                    //  We step over the related name and if we are then
                    //  pointing at a separator character we step over that.

                    RemainingName->Buffer = Add2Ptr(RemainingName->Buffer,
                                                    RelatedFileName->Length,
                                                    PWCHAR);

                    RemainingName->Length -= RelatedFileName->Length;
                }
            }

            // If we are pointing at a separator character then step past that.

            if (RemainingName->Length != 0) {

                if (*(RemainingName->Buffer) == L'\\') {

                    RemainingName->Buffer = Add2Ptr(RemainingName->Buffer,
                                                    sizeof(WCHAR),
                                                    PWCHAR);

                    RemainingName->Length -= sizeof(WCHAR);
                }
            }
        }
    }

#pragma prefast(push)
#pragma prefast(suppress:26030, "RemainingName->FileName.Buffer = FileName.Buffer + (RelatedNameLength + SeparatorLength); FileName.MaximumLength < (RelatedNameLength + SeparatorLength + RemainingNameLength).")
    return STATUS_SUCCESS;
#pragma prefast(pop)
}
