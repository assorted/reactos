////////////////////////////////////////////////////////////////////
// Copyright (C) Alexander Telyatnikov, Ivan Keliukh, Yegor Anchishkin, SKIF Software, 1999-2013. Kiev, Ukraine
// All rights reserved
// This file was released under the GPLv2 on June 2015.
////////////////////////////////////////////////////////////////////
/*************************************************************************
*
* File: Write.cpp
*
* Module: UDF File System Driver (Kernel mode execution only)
*
* Description:
*   Contains code to handle the "Write" dispatch entry point.
*
*************************************************************************/

#include            "udffs.h"

// define the file specific bug-check id
#define         UDF_BUG_CHECK_ID                UDF_FILE_WRITE

/*************************************************************************
*
* Function: UDFCommonWrite()
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
UDFCommonWrite(
    PIRP_CONTEXT IrpContext,
    PIRP             Irp)
{
    NTSTATUS                RC = STATUS_SUCCESS;
    PIO_STACK_LOCATION      IrpSp = NULL;
    LARGE_INTEGER           ByteOffset;
    ULONG                   WriteLength = 0, TruncatedLength = 0;
    SIZE_T                  NumberBytesWritten = 0;
    PFILE_OBJECT            FileObject = NULL;
    TYPE_OF_OPEN TypeOfOpen;
    PFCB                    Fcb = NULL;
    PCCB                    Ccb = NULL;
    PVCB                    Vcb = NULL;
    PVOID                   SystemBuffer = NULL;
    PIRP                    TopIrp;

    LONGLONG                ASize;
    LONGLONG                OldVDL;

    BOOLEAN                 PagingIoResourceAcquired = FALSE;
    BOOLEAN                 MainResourceAcquired = FALSE;
    BOOLEAN                 VcbAcquired = FALSE;

    BOOLEAN                 MainResourceAcquiredExclusive = FALSE;
    BOOLEAN                 MainResourceCanDemoteToShared = FALSE;

    BOOLEAN                 CacheLocked = FALSE;

    BOOLEAN                 CanWait = FALSE;
    BOOLEAN                 PagingIo = FALSE;
    BOOLEAN                 NonCachedIo = FALSE;
    BOOLEAN                 SynchronousIo = FALSE;
    BOOLEAN                 IsThisADeferredWrite = FALSE;
    BOOLEAN                 WriteToEOF = FALSE;
    BOOLEAN                 FileSizesChanged = FALSE;
    BOOLEAN                 RecursiveWriteThrough = FALSE;
    BOOLEAN                 WriteFileSizeToDirNdx = FALSE;
    BOOLEAN                 ZeroBlock = FALSE;
    BOOLEAN                 ZeroBlockDone = FALSE;

    TmPrint(("UDFCommonWrite: irp %x\n", Irp));

    _SEH2_TRY {


        TopIrp = IoGetTopLevelIrp();

        switch((ULONG_PTR)TopIrp) {
        case FSRTL_FSP_TOP_LEVEL_IRP:
            UDFPrint(("  FSRTL_FSP_TOP_LEVEL_IRP\n"));
            break;
        case FSRTL_CACHE_TOP_LEVEL_IRP:
            UDFPrint(("  FSRTL_CACHE_TOP_LEVEL_IRP\n"));
            break;
        case FSRTL_MOD_WRITE_TOP_LEVEL_IRP:
            UDFPrint(("  FSRTL_MOD_WRITE_TOP_LEVEL_IRP\n"));
            break;
        case FSRTL_FAST_IO_TOP_LEVEL_IRP:
            UDFPrint(("  FSRTL_FAST_IO_TOP_LEVEL_IRP\n"));
            BrutePoint();
            break;
        case NULL:
            UDFPrint(("  NULL TOP_LEVEL_IRP\n"));
            break;
        default:
            if (TopIrp == Irp) {
                UDFPrint(("  TOP_LEVEL_IRP\n"));
            } else {
                UDFPrint(("  RECURSIVE_IRP, TOP = %x\n", TopIrp));
            }
            break;
        }

        // First, get a pointer to the current I/O stack location
        IrpSp = IoGetCurrentIrpStackLocation(Irp);
        ASSERT(IrpSp);
        MmPrint(("    Enter Irp, MDL=%x\n", Irp->MdlAddress));

        FileObject = IrpSp->FileObject;
        ASSERT(FileObject);

        // If this is a request at IRQL DISPATCH_LEVEL, then post the request
        if (IrpSp->MinorFunction & IRP_MN_DPC) {
            try_return(RC = STATUS_PENDING);
        }

        // Decode the file object and verify we support read on this.  It
        // must be a user file, stream file or volume file (for a data disk).

        TypeOfOpen = UDFDecodeFileObject(IrpSp->FileObject, &Fcb, &Ccb);

        Vcb = Fcb->Vcb;

        ASSERT_CCB(Ccb);
        ASSERT_FCB(Fcb);
        ASSERT_VCB(Vcb);

        if (Fcb->FcbState & UDF_FCB_DELETED) {
            ASSERT(FALSE);
            try_return(RC = STATUS_TOO_LATE);
        }

        // is this operation allowed ?
        if (Vcb->VcbState & VCB_STATE_MEDIA_WRITE_PROTECT) {
            try_return(RC = STATUS_ACCESS_DENIED);
        }
        Vcb->VcbState |= UDF_VCB_SKIP_EJECT_CHECK;

        // Disk based file systems might decide to verify the logical volume
        //  (if required and only if removable media are supported) at this time
        // As soon as Tray is locked, we needn't call UDFVerifyVcb()

        ByteOffset = IrpSp->Parameters.Write.ByteOffset;

        CanWait = (IrpContext->Flags & IRP_CONTEXT_FLAG_WAIT) ? TRUE : FALSE;
        PagingIo = (Irp->Flags & IRP_PAGING_IO) ? TRUE : FALSE;
        NonCachedIo = (Irp->Flags & IRP_NOCACHE) ? TRUE : FALSE;
        SynchronousIo = (FileObject->Flags & FO_SYNCHRONOUS_IO) ? TRUE : FALSE;
        UDFPrint(("    Flags: %s; %s; %s; %s; Irp(W): %8.8x\n",
                      CanWait ? "Wt" : "nw", PagingIo ? "Pg" : "np",
                      NonCachedIo ? "NonCached" : "Cached", SynchronousIo ? "Snc" : "Asc",
                      Irp->Flags));

        // Get some of the parameters supplied to us
        WriteLength = IrpSp->Parameters.Write.Length;
        if (WriteLength == 0) {
            // a 0 byte write can be immediately succeeded
            if (SynchronousIo && !PagingIo && NT_SUCCESS(RC)) {
                // NT expects changing CurrentByteOffset to zero in this case
                FileObject->CurrentByteOffset.QuadPart = 0;
            }
            try_return(RC);
        }

        // If this is the normal file we have to check for
        // write access according to the current state of the file locks.
        if (!PagingIo &&
            Fcb->FileLock != NULL &&
            !FsRtlCheckLockForWriteAccess(Fcb->FileLock, Irp) ) {

                try_return( RC = STATUS_FILE_LOCK_CONFLICT );
        }

        // **********
        // Is this a write of the volume itself ?
        // **********
        if (Fcb == Fcb->Vcb->VolumeDasdFcb) {
            // Yup, we need to send this on to the disk driver after
            //  validation of the offset and length.

            if (!CanWait)
                try_return(RC = STATUS_PENDING);
            // I dislike the idea of writing to not locked media
            if (!(Vcb->VcbState & VCB_STATE_LOCKED)) {
                try_return(RC = STATUS_ACCESS_DENIED);
            }

            if (IrpContext->Flags & UDF_IRP_CONTEXT_FLUSH2_REQUIRED) {

                UDFPrint(("  UDF_IRP_CONTEXT_FLUSH2_REQUIRED\n"));
                IrpContext->Flags &= ~UDF_IRP_CONTEXT_FLUSH2_REQUIRED;


#ifdef UDF_DELAYED_CLOSE
                UDFFspClose(Vcb);
#endif //UDF_DELAYED_CLOSE

            }

            // Acquire the volume resource exclusive
            UDFAcquireResourceExclusive(&(Vcb->VcbResource), TRUE);
            VcbAcquired = TRUE;

            // I dislike the idea of writing to mounted media too, but M$ has another point of view...
            if (Vcb->VcbCondition == VcbMounted) {
                // flush system cache
                UDFFlushVolume(IrpContext, Vcb);
            }

            // Forward the request to the lower level driver
            // Lock the callers buffer
            if (!NT_SUCCESS(RC = UDFLockUserBuffer(IrpContext, WriteLength, IoReadAccess))) {
                try_return(RC);
            }
            SystemBuffer = UDFMapUserBuffer(Irp);
            if (!SystemBuffer)
                try_return(RC = STATUS_INVALID_USER_BUFFER);

            // Make sure, that volume will never be quick-remounted
            // It is very important for ChkUdf utility.
            Vcb->SerialNumber--;
            // Perform actual Write
            RC = UDFTWrite(IrpContext, Vcb, SystemBuffer, WriteLength,
                           (ULONG)(ByteOffset.QuadPart >> Vcb->SectorShift),
                           &NumberBytesWritten);
            UDFUnlockCallersBuffer(IrpContext, Irp, SystemBuffer);
            try_return(RC);
        }

        if (Vcb->VcbState & VCB_STATE_VOLUME_READ_ONLY) {
            try_return(RC = STATUS_ACCESS_DENIED);
        }

        // back pressure for very smart and fast system cache ;)
        if (!NonCachedIo) {
            // cached IO
            if (Vcb->VerifyCtx.QueuedCount ||
               Vcb->VerifyCtx.ItemCount >= UDF_MAX_VERIFY_CACHE) {
                UDFVVerify(Vcb, UFD_VERIFY_FLAG_WAIT);
            }
        } else {
            if (Vcb->VerifyCtx.ItemCount > UDF_SYS_CACHE_STOP_THR) {
                UDFVVerify(Vcb, UFD_VERIFY_FLAG_WAIT);
            }
        }

        // The FSD (if it is a "nice" FSD) should check whether it is
        // convenient to allow the write to proceed by utilizing the
        // CcCanIWrite() function call. If it is not convenient to perform
        // the write at this time, we should defer the request for a while.
        // The check should not however be performed for non-cached write
        // operations. To determine whether we are retrying the operation
        // or now, use Flags in the IrpContext structure we have created

        IsThisADeferredWrite = BooleanFlagOn(IrpContext->Flags, IRP_CONTEXT_FLAG_DEFERRED_WRITE);

        if (!NonCachedIo &&
            !CcCanIWrite(FileObject, WriteLength, CanWait, IsThisADeferredWrite)) {

            // Cache Manager and/or the VMM does not want us to perform
            // the write at this time. Post the request.

            SetFlag(IrpContext->Flags, IRP_CONTEXT_FLAG_DEFERRED_WRITE);

            CcDeferWrite(FileObject, UDFDeferredWriteCallBack, IrpContext, Irp, WriteLength, IsThisADeferredWrite);
            try_return(RC = STATUS_PENDING);
        }

        // We can continue. Check whether this write operation is targeted
        // to a directory object in which case the UDF FSD will disallow
        // the write request.
        if (Fcb->FcbState & UDF_FCB_DIRECTORY) {
            RC = STATUS_INVALID_DEVICE_REQUEST;
            try_return(RC);
        }

        // Validate start offset and length supplied.
        // Here is a special check that determines whether the caller wishes to
        // begin the write at current end-of-file (whatever the value of that
        // offset might be)
        if (ByteOffset.HighPart == (LONG)0xFFFFFFFF) {
            if (ByteOffset.LowPart == FILE_WRITE_TO_END_OF_FILE) {
                WriteToEOF = TRUE;
                ByteOffset = Fcb->Header.FileSize;
            } else
            if (ByteOffset.LowPart == FILE_USE_FILE_POINTER_POSITION) {
                ByteOffset = FileObject->CurrentByteOffset;
            }
        }

        // Check if this volume has already been shut down.  If it has, fail
        // this write request.
        if (Vcb->VcbState & VCB_STATE_SHUTDOWN) {
            try_return(RC = STATUS_TOO_LATE);
        }

        // Paging I/O write operations are special. If paging i/o write
        // requests begin beyond end-of-file, the request should be no-oped
        // If paging i/o
        // requests extend beyond current end of file, they should be truncated
        // to current end-of-file.
        if (PagingIo && (WriteToEOF || ((ByteOffset.QuadPart + WriteLength) > Fcb->Header.FileSize.QuadPart))) {
            if (ByteOffset.QuadPart > Fcb->Header.FileSize.QuadPart) {
                TruncatedLength = 0;
            } else {
                TruncatedLength = (ULONG)(Fcb->Header.FileSize.QuadPart - ByteOffset.QuadPart);
            }
            if (!TruncatedLength) try_return(RC = STATUS_SUCCESS);
        } else {
            TruncatedLength = WriteLength;
        }

        // There are certain complications that arise when the same file stream
        // has been opened for cached and non-cached access. The FSD is then
        // responsible for maintaining a consistent view of the data seen by
        // the caller.
        // If this happens to be a non-buffered I/O, we should __try to flush the
        // cached data (if some other file object has already initiated caching
        // on the file stream). We should also __try to purge the cached
        // information though the purge will probably fail if the file has been
        // mapped into some process' virtual address space
        // WARNING !!! we should not flush data beyond valid data length
        if (NonCachedIo &&
            !PagingIo &&
            Fcb->FcbNonpaged->SegmentObject.DataSectionObject &&
            TruncatedLength &&
            (ByteOffset.QuadPart < Fcb->Header.FileSize.QuadPart)) {

            // Try to acquire the FCB MainResource exclusively
            if (!UDFAcquireResourceExclusive(&Fcb->FcbNonpaged->FcbResource, CanWait)) {
                try_return(RC = STATUS_PENDING);
            }
            MainResourceAcquired = TRUE;

            //  We hold PagingIo exclusive around the flush and CcPurgeCacheSection to fix a
            //  cache coherency problem.
            UDFAcquireResourceExclusive(&Fcb->FcbNonpaged->FcbPagingIoResource, TRUE);
            PagingIoResourceAcquired = TRUE;

            // Flush and then attempt to purge the cache
            if ((ByteOffset.QuadPart + TruncatedLength) > Fcb->Header.FileSize.QuadPart) {
                NumberBytesWritten = TruncatedLength;
            } else {
                NumberBytesWritten = (ULONG)(Fcb->Header.FileSize.QuadPart - ByteOffset.QuadPart);
            }

            MmPrint(("    CcFlushCache()\n"));
            CcFlushCache(&Fcb->FcbNonpaged->SegmentObject, &ByteOffset, NumberBytesWritten, &Irp->IoStatus);

            // If the flush failed, return error to the caller
            if (!NT_SUCCESS(RC = Irp->IoStatus.Status)) {
                NumberBytesWritten = 0;
                try_return(RC);
            }

            // Attempt the purge
            MmPrint(("    CcPurgeCacheSection()\n"));
            BOOLEAN SuccessfulPurge = CcPurgeCacheSection(&Fcb->FcbNonpaged->SegmentObject, &ByteOffset,
                                                           NumberBytesWritten, FALSE);
            NumberBytesWritten = 0;

            UDFReleaseResource(&Fcb->FcbNonpaged->FcbPagingIoResource);
            PagingIoResourceAcquired = FALSE;

            // We are finished with our flushing and purging
            if (!SuccessfulPurge) {
                try_return(RC = STATUS_PURGE_FAILED);            
            }

            MainResourceCanDemoteToShared = TRUE;
        }

        // Determine if we were called by the lazywriter.
        // We reuse 'IsThisADeferredWrite' here to decrease stack usage
        IsThisADeferredWrite = (Fcb->LazyWriteThread == PsGetCurrentThread());

        // Acquire the appropriate FCB resource
        if (PagingIo) {

            if (!UDFAcquireResourceShared(&Fcb->FcbNonpaged->FcbPagingIoResource, TRUE)) {
                try_return(RC = STATUS_PENDING);
            }
            PagingIoResourceAcquired = TRUE;

            ASSERT(NonCachedIo);

        } else {
            // Try to acquire the FCB MainResource shared
            if (NonCachedIo) {
                if (!MainResourceAcquired) {
                    if (!UDFAcquireSharedWaitForExclusive(&Fcb->FcbNonpaged->FcbResource, CanWait)) {
                        try_return(RC = STATUS_PENDING);
                    }
                    MainResourceAcquired = TRUE;
                }
            } else {
                if (!MainResourceAcquired) {
                    UDF_CHECK_PAGING_IO_RESOURCE(Fcb);
                    if (!UDFAcquireResourceShared(&Fcb->FcbNonpaged->FcbResource, CanWait)) {
                        try_return(RC = STATUS_PENDING);
                    }
                    MainResourceAcquired = TRUE;
                }
            }
        }

        //  Set the flag indicating if Fast I/O is possible
        Fcb->Header.IsFastIoPossible = UDFIsFastIoPossible(Fcb);
/*        if (Fcb->CommonFCBHeader.IsFastIoPossible == FastIoIsPossible) {
            Fcb->CommonFCBHeader.IsFastIoPossible = FastIoIsQuestionable;
        }*/

        if ((Irp->Flags & IRP_SYNCHRONOUS_PAGING_IO) &&
             (IrpContext->Flags & UDF_IRP_CONTEXT_NOT_TOP_LEVEL)) {

            //  This clause determines if the top level request was
            //  in the FastIo path.
            if ((ULONG_PTR)TopIrp > FSRTL_MAX_TOP_LEVEL_IRP_FLAG &&
                NodeType(TopIrp) == IO_TYPE_IRP) {

                PIO_STACK_LOCATION IrpStack;
                ASSERT( TopIrp->Type == IO_TYPE_IRP );
                IrpStack = IoGetCurrentIrpStackLocation(TopIrp);

                //  Finally this routine detects if the Top irp was a
                //  write to this file and thus we are the writethrough.
                if ((IrpStack->MajorFunction == IRP_MJ_WRITE) &&
                    (IrpStack->FileObject->FsContext == FileObject->FsContext)) {

                    RecursiveWriteThrough = TRUE;
                    IrpContext->Flags |= IRP_CONTEXT_FLAG_WRITE_THROUGH;
                }
            }
        }

        //  Here is the deal with ValidDataLength and FileSize:
        //
        //  Rule 1: PagingIo is never allowed to extend file size.
        //
        //  Rule 2: Only the top level requestor may extend Valid
        //          Data Length.  This may be paging IO, as when a
        //          a user maps a file, but will never be as a result
        //          of cache lazy writer writes since they are not the
        //          top level request.
        //
        //  Rule 3: If, using Rules 1 and 2, we decide we must extend
        //          file size or valid data, we take the Fcb exclusive.

        // Check whether the current request will extend the file size,
        // or the valid data length (if the FSD supports the concept of a
        // valid data length associated with the file stream). In either case,
        // inform the Cache Manager at this time using CcSetFileSizes() about
        // the new file length. Note that real FSD implementations will have to
        // first allocate enough on-disk space at this point (before they
        // inform the Cache Manager about the new size) to ensure that the write
        // will subsequently not fail due to lack of disk space.

        OldVDL = Fcb->Header.ValidDataLength.QuadPart;
        ZeroBlock = (ByteOffset.QuadPart > OldVDL);

        if (!PagingIo &&
            !RecursiveWriteThrough &&
            !IsThisADeferredWrite) {

            BOOLEAN ExtendFS;

            ExtendFS = (ByteOffset.QuadPart + TruncatedLength > Fcb->Header.FileSize.QuadPart);

            if ( WriteToEOF || ZeroBlock || ExtendFS) {
                // we are extending the file;

                if (!CanWait)
                    try_return(RC = STATUS_PENDING);
//                CanWait = TRUE;

                // Try to acquire the FCB MainResource exclusively
                if (!MainResourceAcquiredExclusive) {

                    UDFReleaseResource(&Fcb->FcbNonpaged->FcbResource);
                    MainResourceAcquired = FALSE;

                    UDF_CHECK_PAGING_IO_RESOURCE(Fcb);
                    if (!UDFAcquireResourceExclusive(&Fcb->FcbNonpaged->FcbResource, CanWait)) {
                        try_return(RC = STATUS_PENDING);
                    }
                    MainResourceAcquired = TRUE;
                }

                UDFAcquireResourceExclusive(&Fcb->FcbNonpaged->FcbPagingIoResource, TRUE);
                PagingIoResourceAcquired = TRUE;

                if (ExtendFS) {
                    RC = UDFResizeFile__(IrpContext, Vcb, Fcb->FileInfo, ByteOffset.QuadPart + TruncatedLength);

                    if (!NT_SUCCESS(RC)) {
                        try_return(RC);
                    }
                    FileSizesChanged = TRUE;
                    // ... and inform the Cache Manager about it

                    Fcb->Header.FileSize.QuadPart = ByteOffset.QuadPart + TruncatedLength;
                    Fcb->Header.AllocationSize.QuadPart = UDFGetFileAllocationSize(Vcb, Fcb->FileInfo);
                    if (!Vcb->LowFreeSpace) {
                        Fcb->Header.AllocationSize.QuadPart += (PAGE_SIZE*9-1);
                    } else {
                        Fcb->Header.AllocationSize.QuadPart += (PAGE_SIZE-1);
                    }
                    Fcb->Header.AllocationSize.LowPart &= ~(PAGE_SIZE-1);
                }

                UDFReleaseResource(&Fcb->FcbNonpaged->FcbPagingIoResource);
                PagingIoResourceAcquired = FALSE;

                UDFPrint(("UDFCommonWrite: Set size %x (alloc size %x)\n", ByteOffset.LowPart + TruncatedLength, Fcb->Header.AllocationSize.LowPart));
                if (CcIsFileCached(FileObject)) {
                    if (ExtendFS) {
                        MmPrint(("    CcSetFileSizes()\n"));
                        CcSetFileSizes(FileObject, (PCC_FILE_SIZES)&Fcb->Header.AllocationSize);
                        Fcb->NtReqFCBFlags |= UDF_NTREQ_FCB_MODIFIED;
                    }
                    // Attempt to Zero newly added fragment
                    // and ignore the return code
                    // This should be done to inform cache manager
                    // that given extent has no cached data
                    // (Otherwise, CM sometimes thinks that it has)
                    if (ZeroBlock) {
                        Fcb->NtReqFCBFlags |= UDF_NTREQ_FCB_MODIFIED;
                        ThPrint(("    UDFZeroDataEx(1)\n"));
                        UDFZeroData(Vcb,
                                    FileObject,
                                    OldVDL,
                                    Fcb->Header.FileSize.QuadPart - OldVDL,
                                    CanWait);
#ifdef UDF_DBG
                        ZeroBlockDone = TRUE;
#endif //UDF_DBG
                    }
                }
            }

        }

#ifdef UDF_DISABLE_SYSTEM_CACHE_MANAGER
        NonCachedIo = TRUE;
#endif
        if (Fcb && Fcb->FileInfo && Fcb->FileInfo->Dloc) {
            AdPrint(("UDFCommonWrite: DataLoc %x, Mapping %x\n", Fcb->FileInfo->Dloc->DataLoc, Fcb->FileInfo->Dloc->DataLoc.Mapping));
        }

        //  Branch here for cached vs non-cached I/O
        if (!NonCachedIo) {

            // The caller wishes to perform cached I/O. Initiate caching if
            // this is the first cached I/O operation using this file object
            if (!FileObject->PrivateCacheMap) {

                // This is the first cached I/O operation. You must ensure
                // that the FCB Header contains valid sizes at this time
                UDFPrint(("UDFCommonWrite: Init system cache\n"));
                MmPrint(("    CcInitializeCacheMap()\n"));
                CcInitializeCacheMap(FileObject, (PCC_FILE_SIZES)&Fcb->Header.AllocationSize,
                    FALSE,      // We will not utilize pin access for this file
                    &(UdfData.CacheMgrCallBacks), // callbacks
                    Fcb);       // The context used in callbacks
                MmPrint(("    CcSetReadAheadGranularity()\n"));
                CcSetReadAheadGranularity(FileObject, READ_AHEAD_GRANULARITY);
            }

            if (ZeroBlock && !ZeroBlockDone) {
                ThPrint(("    UDFZeroDataEx(2)\n"));
                UDFZeroData(Vcb,
                            FileObject,
                            OldVDL,
                            ByteOffset.QuadPart + TruncatedLength - OldVDL,
                            CanWait);
                if (ByteOffset.LowPart & (PAGE_SIZE-1)) {
                }
            }

            WriteFileSizeToDirNdx = (IrpContext->Flags & IRP_CONTEXT_FLAG_WRITE_THROUGH) ?
                                    TRUE : FALSE;
            // Check and see if this request requires a MDL returned to the caller
            if (IrpSp->MinorFunction & IRP_MN_MDL) {
                // Caller does want a MDL returned. Note that this mode
                // implies that the caller is prepared to block
                MmPrint(("    CcPrepareMdlWrite()\n"));
//                CcPrepareMdlWrite(FileObject, &ByteOffset, TruncatedLength, &(Irp->MdlAddress), &(Irp->IoStatus));
//                NumberBytesWritten = Irp->IoStatus.Information;
//                RC = Irp->IoStatus.Status;

                NumberBytesWritten = 0;
                RC = STATUS_INVALID_PARAMETER;

                try_return(RC);
            }

            // This is a regular run-of-the-mill cached I/O request. Let the
            // Cache Manager worry about it!
            // First though, we need a buffer pointer (address) that is valid

            // We needn't call CcZeroData 'cause udf_info.cpp will care about it
            SystemBuffer = UDFMapUserBuffer(Irp);
            if (!SystemBuffer)
                try_return(RC = STATUS_INVALID_USER_BUFFER);
            ASSERT(SystemBuffer);
            Fcb->NtReqFCBFlags |= UDF_NTREQ_FCB_MODIFIED;

            MmPrint(("    CcCopyWrite()\n"));
            if (!CcCopyWrite(FileObject, &(ByteOffset), TruncatedLength, CanWait, SystemBuffer)) {
                // The caller was not prepared to block and data is not immediately
                // available in the system cache
                // Mark Irp Pending ...
                try_return(RC = STATUS_PENDING);
            }

            UDFUnlockCallersBuffer(IrpContext, Irp, SystemBuffer);
            // We have the data
            RC = STATUS_SUCCESS;
            NumberBytesWritten = TruncatedLength;

            try_return(RC);

        } else {

            MmPrint(("    Write NonCachedIo\n"));

            // We needn't call CcZeroData here (like in Fat driver)
            // 'cause we've already done it above
            // (see call to UDFZeroDataEx() )
            if (!RecursiveWriteThrough &&
                !IsThisADeferredWrite &&
                (OldVDL < ByteOffset.QuadPart)) {
#ifdef UDF_DBG
                    ASSERT(!ZeroBlockDone);
#endif //UDF_DBG
                    UDFZeroData(Vcb,
                                FileObject,
                                OldVDL,
                                ByteOffset.QuadPart - OldVDL,
                                CanWait);
            }
            if (OldVDL < (ByteOffset.QuadPart + TruncatedLength)) {
                Fcb->Header.ValidDataLength.QuadPart = ByteOffset.QuadPart + TruncatedLength;
            }

            // Successful check will cause WCache lock
            if (!CanWait && UDFIsFileCached__(Vcb, Fcb->FileInfo, ByteOffset.QuadPart, TruncatedLength, TRUE)) {
                UDFPrint(("UDFCommonWrite: Cached => CanWait\n"));
                CacheLocked = TRUE;
                CanWait = TRUE;
            }
            // Send the request to lower level drivers
            if (!CanWait) {
                UDFPrint(("UDFCommonWrite: Post physical write %x bytes at %x\n", TruncatedLength, ByteOffset.LowPart));

                try_return(RC = STATUS_PENDING);
            }

            // Lock the callers buffer

            if (!NT_SUCCESS(RC = UDFLockUserBuffer(IrpContext, TruncatedLength, IoReadAccess))) {
                try_return(RC);
            }

            SystemBuffer = UDFMapUserBuffer(Irp);
            if (!SystemBuffer) {
                try_return(RC = STATUS_INVALID_USER_BUFFER);
            }
            Fcb->NtReqFCBFlags |= UDF_NTREQ_FCB_MODIFIED;
            RC = UDFWriteFile__(IrpContext, Vcb, Fcb->FileInfo, ByteOffset.QuadPart, TruncatedLength,
                           CacheLocked, (PCHAR)SystemBuffer, &NumberBytesWritten);

            UDFUnlockCallersBuffer(IrpContext, Irp, SystemBuffer);

            WriteFileSizeToDirNdx = TRUE;

            try_return(RC);
        }

try_exit:   NOTHING;

    } _SEH2_FINALLY {

        if (CacheLocked) {
            WCacheEODirect__(&(Vcb->FastCache), Vcb);
        }

        if (RC == STATUS_PENDING) {

            // Release any resources acquired here ...
            if (PagingIoResourceAcquired) {
                UDFReleaseResource(&Fcb->FcbNonpaged->FcbPagingIoResource);
            }

            if (MainResourceAcquired) {
                UDF_CHECK_PAGING_IO_RESOURCE(Fcb);
                UDFReleaseResource(&Fcb->FcbNonpaged->FcbResource);
            }

            if (VcbAcquired) {
                UDFReleaseResource(&Vcb->VcbResource);
            }

        } else {
            // For synchronous I/O, the FSD must maintain the current byte offset
            // Do not do this however, if I/O is marked as paging-io
            if (SynchronousIo && !PagingIo && NT_SUCCESS(RC)) {
                FileObject->CurrentByteOffset.QuadPart = ByteOffset.QuadPart + NumberBytesWritten;
            }
            // If the write completed successfully and this was not a paging-io
            // operation, set a flag in the CCB that indicates that a write was
            // performed and that the file time should be updated at cleanup
            if (NT_SUCCESS(RC) && !PagingIo) {
                // If the file size was changed, set a flag in the FCB indicating that
                // this occurred.
				SetFlag(FileObject->Flags, FO_FILE_MODIFIED);
                
				if (FileSizesChanged) {
                    if (!WriteFileSizeToDirNdx) {
						
                        FileObject->Flags |= FO_FILE_SIZE_CHANGED;
                    } else {
						
                        ASize = UDFGetFileAllocationSize(Vcb, Fcb->FileInfo);
                        UDFSetFileSizeInDirNdx(Vcb, Fcb->FileInfo, &ASize);
						
						if (UDFIsAStream(Fcb->FileInfo)) {

                            UDFNotifyFullReportChange(Vcb,
                                                      Fcb,
                                                      FILE_NOTIFY_CHANGE_STREAM_SIZE,
                                                      FILE_ACTION_MODIFIED_STREAM);
                        } else {

                            UDFNotifyFullReportChange(Vcb,
                                                      Fcb,
                                                      FILE_NOTIFY_CHANGE_SIZE,
                                                      FILE_ACTION_MODIFIED);
						}
                    }
                }
                // Update ValidDataLength
                if (!IsThisADeferredWrite) {

                    if (Fcb->Header.ValidDataLength.QuadPart < (ByteOffset.QuadPart + NumberBytesWritten)) {

                        Fcb->Header.ValidDataLength.QuadPart =
                            min(Fcb->Header.FileSize.QuadPart,
                                ByteOffset.QuadPart + NumberBytesWritten);

                        if (NonCachedIo && CcIsFileCached(FileObject)) {
                            CcSetFileSizes(FileObject, (PCC_FILE_SIZES)&Fcb->Header.AllocationSize);
                        }
                    }
                }
            }

            // Release any resources acquired here ...
            if (PagingIoResourceAcquired) {
                UDFReleaseResource(&Fcb->FcbNonpaged->FcbPagingIoResource);
            }

            if (MainResourceAcquired) {
                UDF_CHECK_PAGING_IO_RESOURCE(Fcb);
                UDFReleaseResource(&Fcb->FcbNonpaged->FcbResource);
            }

            if (VcbAcquired) {
                UDFReleaseResource(&Vcb->VcbResource);
            }
            // If the request failed, and we had done some nasty stuff like
            // extending the file size (including informing the Cache Manager
            // about the new file size), and allocating on-disk space etc., undo
            // it at this time.

            // Can complete the IRP here if no exception was encountered
            if (!_SEH2_AbnormalTermination() &&
               Irp) {
                Irp->IoStatus.Status = RC;
                Irp->IoStatus.Information = NumberBytesWritten;
            }
        }
    } _SEH2_END; // end of "__finally" processing

    // Post IRP if required

    if (RC == STATUS_PENDING) {

        RC = UDFFsdPostRequest(IrpContext, Irp);
    }
    else {

        UDFCompleteRequest(IrpContext, Irp, RC);
    }

    UDFPrint(("\n"));
    return(RC);
} // end UDFCommonWrite()

/*************************************************************************
*
* Function: UDFDeferredWriteCallBack()
*
* Description:
*   Invoked by the cache manager in the context of a worker thread.
*   Typically, you can simply post the request at this point (just
*   as you would have if the original request could not block) to
*   perform the write in the context of a system worker thread.
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
UDFDeferredWriteCallBack(
    IN PVOID Context1,          // Should be IrpContext
    IN PVOID Context2           // Should be Irp
    )
{
    UDFPrint(("UDFDeferredWriteCallBack\n"));
    // We should typically simply post the request to our internal
    // queue of posted requests (just as we would if the original write
    // could not be completed because the caller could not block).
    // Once we post the request, return from this routine. The write
    // will then be retried in the context of a system worker thread
    UDFAddToWorkque((PIRP_CONTEXT)Context1, (PIRP)Context2);

} // end UDFDeferredWriteCallBack()

/*************************************************************************
*
*************************************************************************/

#define USE_CcCopyWrite_TO_ZERO

VOID
UDFPurgeCacheEx_(
    PFCB                Fcb,
    LONGLONG            Offset,
    LONGLONG            Length,
//#ifndef ALLOW_SPARSE
    BOOLEAN             CanWait,
//#endif //ALLOW_SPARSE
    PVCB                Vcb,
    PFILE_OBJECT        FileObject
    )
{
    ULONG Off_l;
#ifdef USE_CcCopyWrite_TO_ZERO
    ULONG PgLen;
#endif //USE_CcCopyWrite_TO_ZERO

    // We'll just purge cache section here,
    // without call to CcZeroData()
    // 'cause udf_info.cpp will care about it

#define PURGE_BLOCK_SZ 0x10000000

    // NOTE: if FS engine doesn't suport
    // sparse/unrecorded areas, CcZeroData must be called
    // In this case we'll see some recursive WRITE requests

    _SEH2_TRY {
        MmPrint(("    UDFPurgeCacheEx_():  Offs: %I64x, ", Offset));
        MmPrint((" Len: %lx\n", Length));
        SECTION_OBJECT_POINTERS* SectionObject = &Fcb->FcbNonpaged->SegmentObject;
        if (Length) {
            LONGLONG Offset0, OffsetX, VDL;

            Offset0 = Offset;
            if ((Off_l = ((ULONG)Offset0 & (PAGE_SIZE-1)))) {
                //                 Offset, Offset0
                //                 v
                // ...|dddddddddddd00000|....
                //    |<- Off_l ->|
#ifndef USE_CcCopyWrite_TO_ZERO
                *((PULONG)&Offset0) &= ~(PAGE_SIZE-1);
                MmPrint(("    CcFlushCache(s) Offs %I64x, Len %x\n", Offset0, Off_l));
                CcFlushCache( SectionObject, (PLARGE_INTEGER)&Offset0, Off_l, NULL );
#else //USE_CcCopyWrite_TO_ZERO
                // ...|ddddd000000000000|....
                //          |<- PgLen ->|
                PgLen = PAGE_SIZE - Off_l; /*(*((PULONG)&Offset) & (PAGE_SIZE-1))*/
                //
                if (PgLen > Length)
                    PgLen = (ULONG)Length;

                MmPrint(("    ZeroCache (CcWrite) Offs %I64x, Len %x\n", Offset, PgLen));
#ifdef DBG
                if (FileObject && Vcb) {

                    ASSERT(CanWait);
#endif //DBG
                    if (PgLen) {
                        if (SectionObject->SharedCacheMap) {
                            CcCopyWrite(FileObject, (PLARGE_INTEGER)&Offset, PgLen, TRUE || CanWait, Vcb->ZBuffer);
                        }
                        Offset += PgLen;
                        Length -= PgLen;
                    }
#ifdef DBG
                } else {
                    MmPrint(("    Can't use CcWrite to zero cache\n"));
                }
#endif //DBG
#endif //USE_CcCopyWrite_TO_ZERO
            }
            VDL = Fcb->Header.ValidDataLength.QuadPart;
            OffsetX = Offset+Length;
            if ((Off_l = ((ULONG)OffsetX & (PAGE_SIZE-1)))) {

                if (OffsetX < VDL) {
#ifndef USE_CcCopyWrite_TO_ZERO
                    Off_l = ( (ULONG)(VDL-OffsetX) > PAGE_SIZE ) ?
                        (PAGE_SIZE - Off_l) :
                        ((ULONG)(VDL-OffsetX));
                    *((PULONG)&OffsetX) &= ~(PAGE_SIZE-1);
                    MmPrint(("    CcFlushCache(e) Offs %I64x, Len %x\n", OffsetX, Off_l));
                    CcFlushCache( SectionObject, (PLARGE_INTEGER)&OffsetX, Off_l, NULL );
#else //USE_CcCopyWrite_TO_ZERO
                    if (VDL - OffsetX > PAGE_SIZE) {
                        PgLen = (ULONG)OffsetX & ~(PAGE_SIZE-1);
                    } else {
                        PgLen = (ULONG)(VDL - OffsetX) & ~(PAGE_SIZE-1);
                    }
                    // ...|000000000000ddddd|....
                    //    |<- PgLen ->|
                    MmPrint(("    ZeroCache (CcWrite - 2) Offs %I64x, Len %x\n", OffsetX, PgLen));
#ifdef DBG
                    if (FileObject && Vcb) {
                        ASSERT(CanWait);
#endif //DBG
                        if (SectionObject->SharedCacheMap) {
                            CcCopyWrite(FileObject, (PLARGE_INTEGER)&OffsetX, PgLen, TRUE || CanWait, Vcb->ZBuffer);
                        }
                        Length -= PgLen;
#ifdef DBG
                    } else {
                        MmPrint(("    Can't use CcWrite to zero cache (2)\n"));
                    }
#endif //DBG
#endif //USE_CcCopyWrite_TO_ZERO
                }
            }
#ifndef USE_CcCopyWrite_TO_ZERO
            do
#else //USE_CcCopyWrite_TO_ZERO
            while(Length)
#endif //USE_CcCopyWrite_TO_ZERO
            {
                MmPrint(("    CcPurgeCacheSection()\n"));
                if (PURGE_BLOCK_SZ > Length) {
                    CcPurgeCacheSection(SectionObject, (PLARGE_INTEGER)&Offset,
                                                (ULONG)Length, FALSE);
    /*
                    NtReqFcb->CommonFCBHeader.ValidDataLength.QuadPart += Length;
                    ASSERT(NtReqFcb->CommonFCBHeader.ValidDataLength.QuadPart <=
                           NtReqFcb->CommonFCBHeader.FileSize.QuadPart);
                    MmPrint(("    CcFlushCache()\n"));
                    CcFlushCache( SectionObject, (PLARGE_INTEGER)&Offset, (ULONG)Length, NULL );
    */
#ifndef ALLOW_SPARSE
        //            UDFZeroFile__(
#endif //ALLOW_SPARSE
                    break;
                } else {
                    CcPurgeCacheSection(SectionObject, (PLARGE_INTEGER)&Offset,
                                                PURGE_BLOCK_SZ, FALSE);
    /*
                    NtReqFcb->CommonFCBHeader.ValidDataLength.QuadPart += PURGE_BLOCK_SZ;
                    ASSERT(NtReqFcb->CommonFCBHeader.ValidDataLength.QuadPart <=
                           NtReqFcb->CommonFCBHeader.FileSize.QuadPart);
                    MmPrint(("    CcFlushCache()\n"));
                    CcFlushCache( SectionObject, (PLARGE_INTEGER)&Offset, (ULONG)Length, NULL );
    */
#ifndef ALLOW_SPARSE
        //            UDFZeroFile__(
#endif //ALLOW_SPARSE
                    Length -= PURGE_BLOCK_SZ;
                    Offset += PURGE_BLOCK_SZ;
                }
            }
#ifndef USE_CcCopyWrite_TO_ZERO
            while(Length);
#endif //USE_CcCopyWrite_TO_ZERO
            if (VDL < Offset)
                Fcb->Header.ValidDataLength.QuadPart = Offset;
        }
    } _SEH2_EXCEPT(EXCEPTION_EXECUTE_HANDLER) {
        BrutePoint();
    } _SEH2_END;
} // end UDFPurgeCacheEx_()

BOOLEAN
UDFZeroData (
    IN PVCB Vcb,
    IN PFILE_OBJECT FileObject,
    IN ULONG StartingZero,
    IN ULONG ByteCount,
    BOOLEAN CanWait
    )

/*++

    **** Temporary function - Remove when CcZeroData is capable of handling
    non sector aligned requests.

--*/
{
    LARGE_INTEGER ZeroStart = {0,0};
    LARGE_INTEGER BeyondZeroEnd = {0,0};

    BOOLEAN Finished;

    PAGED_CODE();

    ULONG LBS = Vcb->SectorSize;

    ZeroStart.LowPart = (StartingZero + (LBS - 1)) & ~(LBS - 1);

    //
    //  Detect overflow if we were asked to zero in the last sector of the file,
    //  which must be "zeroed" already (or we're in trouble).
    //
    
    if (StartingZero != 0 && ZeroStart.LowPart == 0) {
        
        return TRUE;
    }

    //
    //  Note that BeyondZeroEnd can take the value 4gb.
    //
    
    BeyondZeroEnd.QuadPart = ((ULONGLONG) StartingZero + ByteCount + (LBS - 1))
                             & (~((LONGLONG) LBS - 1));

    //
    //  If we were called to just zero part of a sector we are in trouble.
    //
    
    if ( ZeroStart.QuadPart == BeyondZeroEnd.QuadPart ) {

        return TRUE;
    }

    Finished = CcZeroData( FileObject,
                           &ZeroStart,
                           &BeyondZeroEnd,
                           CanWait );

    return Finished;
}
