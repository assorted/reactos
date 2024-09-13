////////////////////////////////////////////////////////////////////
// Copyright (C) Alexander Telyatnikov, Ivan Keliukh, Yegor Anchishkin, SKIF Software, 1999-2013. Kiev, Ukraine
// All rights reserved
// This file was released under the GPLv2 on June 2015.
////////////////////////////////////////////////////////////////////
/*************************************************************************
*
* File: Devcntrl.cpp
*
* Module: UDF File System Driver (Kernel mode execution only)
*
* Description:
*   Contains code to handle the "Device IOCTL" dispatch entry point.
*
*************************************************************************/

#include "udffs.h"

#define UDF_CURRENT_BUILD 123456789

// define the file specific bug-check id
#ifdef UDF_BUG_CHECK_ID
#undef UDF_BUG_CHECK_ID
#endif
#define         UDF_BUG_CHECK_ID                UDF_FILE_DEVICE_CONTROL

#ifndef OBSOLETE_IOCTL_CDROM_GET_CONTROL
#define OBSOLETE_IOCTL_CDROM_GET_CONTROL  CTL_CODE(IOCTL_CDROM_BASE, 0x000D, METHOD_BUFFERED, FILE_READ_ACCESS)
#endif // OBSOLETE_IOCTL_CDROM_GET_CONTROL

NTSTATUS
UDFProcessLicenseKey(
    PIRP_CONTEXT IrpContext,
    PIRP             Irp
    );

/*#if(_WIN32_WINNT < 0x0400)
#define IOCTL_REDIR_QUERY_PATH   CTL_CODE(FILE_DEVICE_NETWORK_FILE_SYSTEM, 99, METHOD_NEITHER, FILE_ANY_ACCESS)

typedef struct _QUERY_PATH_REQUEST {
    ULONG PathNameLength;
    PIO_SECURITY_CONTEXT SecurityContext;
    WCHAR FilePathName[1];
} QUERY_PATH_REQUEST, *PQUERY_PATH_REQUEST;

typedef struct _QUERY_PATH_RESPONSE {
    ULONG LengthAccepted;
} QUERY_PATH_RESPONSE, *PQUERY_PATH_RESPONSE;

#endif*/


/*************************************************************************
*
* Function: UDFDeviceControl()
*
* Description:
*   The I/O Manager will invoke this routine to handle a Device IOCTL
*   request
*
* Expected Interrupt Level (for execution) :
*
*  IRQL_PASSIVE_LEVEL (invocation at higher IRQL will cause execution
*   to be deferred to a worker thread context)
*
* Return Value: STATUS_SUCCESS/Error
*
*************************************************************************/
NTSTATUS
NTAPI
UDFDeviceControl(
    PDEVICE_OBJECT          DeviceObject,       // the logical volume device object
    PIRP                    Irp)                // I/O Request Packet
{
    NTSTATUS            RC = STATUS_SUCCESS;
    PIRP_CONTEXT IrpContext = NULL;
    BOOLEAN             AreWeTopLevel = FALSE;

    TmPrint(("UDFDeviceControl: \n"));

    FsRtlEnterFileSystem();
    ASSERT(DeviceObject);
    ASSERT(Irp);

    // set the top level context
    AreWeTopLevel = UDFIsIrpTopLevel(Irp);
    //ASSERT(!UDFIsFSDevObj(DeviceObject));

    _SEH2_TRY {

        // get an IRP context structure and issue the request
        IrpContext = UDFAllocateIrpContext(Irp, DeviceObject);
        if(IrpContext) {
            RC = UDFCommonDeviceControl(IrpContext, Irp);
        } else {
            RC = STATUS_INSUFFICIENT_RESOURCES;
            Irp->IoStatus.Status = RC;
            Irp->IoStatus.Information = 0;
            // complete the IRP
            IoCompleteRequest(Irp, IO_DISK_INCREMENT);
        }

    } _SEH2_EXCEPT(UDFExceptionFilter(IrpContext, _SEH2_GetExceptionInformation())) {

        RC = UDFExceptionHandler(IrpContext, Irp);

        UDFLogEvent(UDF_ERROR_INTERNAL_ERROR, RC);
    } _SEH2_END;

    if (AreWeTopLevel) {
        IoSetTopLevelIrp(NULL);
    }

    FsRtlExitFileSystem();

    return(RC);
} // end UDFDeviceControl()


/*************************************************************************
*
* Function: UDFCommonDeviceControl()
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
UDFCommonDeviceControl(
    PIRP_CONTEXT PtrIrpContext,
    PIRP             Irp
    )
{
    NTSTATUS                RC = STATUS_SUCCESS;
    PIO_STACK_LOCATION      IrpSp = NULL;
//    PIO_STACK_LOCATION      PtrNextIoStackLocation = NULL;
    PFILE_OBJECT            FileObject = NULL;
    PFCB                    Fcb = NULL;
    PCCB                    Ccb = NULL;
    PVCB                    Vcb = NULL;
    BOOLEAN                 CompleteIrp = FALSE;
    ULONG                   IoControlCode = 0;
//    PVOID                   BufferPointer = NULL;
    BOOLEAN                 AcquiredVcb = FALSE;
    BOOLEAN                 FSDevObj;
    BOOLEAN                 UnsafeIoctl = TRUE;
    UCHAR                   ScsiCommand;
    PCDB                    Cdb;
    PCHAR                   CdbData;
    PCHAR                   ModeSelectData;

    UDFPrint(("UDFCommonDeviceControl\n"));

    _SEH2_TRY {
        // First, get a pointer to the current I/O stack location
        IrpSp = IoGetCurrentIrpStackLocation(Irp);
        ASSERT(IrpSp);

        // Get the IoControlCode value
        IoControlCode = IrpSp->Parameters.DeviceIoControl.IoControlCode;

        FileObject = IrpSp->FileObject;
        ASSERT(FileObject);

        FSDevObj = UDFIsFSDevObj(PtrIrpContext->TargetDeviceObject);

        if(FSDevObj) {

            ASSERT(FALSE);
            CompleteIrp = TRUE;
            try_return(RC = STATUS_INVALID_PARAMETER);
        } else {
            Ccb = (PCCB)FileObject->FsContext2;
            if(!Ccb) {
                UDFPrint(("  !Ccb\n"));
                goto ioctl_do_default;
            }
            ASSERT(Ccb);
            Fcb = Ccb->Fcb;
            ASSERT(Fcb);

            // Check if the IOCTL is suitable for this type of File
            if (Fcb->NodeIdentifier.NodeTypeCode == UDF_NODE_TYPE_VCB) {
                // Everything is acceptable for Volume
                Vcb = (PVCB)(Fcb);
            } else {
                Vcb = Fcb->Vcb;
                CompleteIrp = TRUE;
                UDFPrint(("UDFCommonDeviceControl: STATUS_INVALID_PARAMETER %x for File/Dir Obj\n", IoControlCode));
                try_return(RC = STATUS_INVALID_PARAMETER);
            }
            // check 'safe' IOCTLs
            switch (IoControlCode) {
            case IOCTL_CDROM_RAW_READ:

            case IOCTL_CDROM_GET_DRIVE_GEOMETRY:
            case IOCTL_DISK_GET_DRIVE_GEOMETRY:
            case IOCTL_DISK_GET_PARTITION_INFO:
            case IOCTL_DISK_GET_DRIVE_LAYOUT:

            case IOCTL_CDROM_GET_DRIVE_GEOMETRY_EX:
            case IOCTL_DISK_GET_PARTITION_INFO_EX:
            case IOCTL_DISK_GET_DRIVE_LAYOUT_EX:
            case IOCTL_DISK_GET_DRIVE_GEOMETRY_EX:

            case IOCTL_STORAGE_CHECK_VERIFY:
            case IOCTL_STORAGE_CHECK_VERIFY2:
            case IOCTL_DISK_CHECK_VERIFY:
            case IOCTL_CDROM_CHECK_VERIFY:

            case IOCTL_CDROM_LOAD_MEDIA:
            case IOCTL_DISK_LOAD_MEDIA:
            case IOCTL_STORAGE_LOAD_MEDIA:
            case IOCTL_STORAGE_LOAD_MEDIA2:

            case IOCTL_CDROM_GET_CONFIGURATION:
            case IOCTL_CDROM_GET_LAST_SESSION:
            case IOCTL_CDROM_READ_TOC:
            case IOCTL_CDROM_READ_TOC_EX:
            case IOCTL_CDROM_PLAY_AUDIO_MSF:
            case IOCTL_CDROM_READ_Q_CHANNEL:
            case IOCTL_CDROM_PAUSE_AUDIO:
            case IOCTL_CDROM_RESUME_AUDIO:
            case IOCTL_CDROM_SEEK_AUDIO_MSF:
            case IOCTL_CDROM_STOP_AUDIO:
#if (NTDDI_VERSION < NTDDI_WS03)
            case IOCTL_CDROM_GET_CONTROL:
#else
            case OBSOLETE_IOCTL_CDROM_GET_CONTROL:
#endif
            case IOCTL_CDROM_GET_VOLUME:
            case IOCTL_CDROM_SET_VOLUME:

            case IOCTL_DISK_GET_MEDIA_TYPES:
            case IOCTL_STORAGE_GET_MEDIA_TYPES:
            case IOCTL_STORAGE_GET_MEDIA_TYPES_EX:

            case IOCTL_DISK_IS_WRITABLE:
            case IOCTL_DVD_READ_STRUCTURE:

            case IOCTL_MOUNTDEV_QUERY_DEVICE_NAME:

            case FSCTL_IS_VOLUME_DIRTY:

                UnsafeIoctl = FALSE;
                break;
            }

            if(IoControlCode != IOCTL_CDROM_DISK_TYPE) {
                UDFAcquireResourceShared(&(Vcb->VCBResource), TRUE);
            } else {
                UDFAcquireResourceExclusive(&(Vcb->VCBResource), TRUE);
            }
            AcquiredVcb = TRUE;
        }

        UDFPrint(("UDF Irp %x, ctx %x, DevIoCtl %x\n", Irp, PtrIrpContext, IoControlCode));

        // We may wish to allow only   volume open operations.
        switch (IoControlCode) {

        case IOCTL_SCSI_PASS_THROUGH_DIRECT:
        case IOCTL_SCSI_PASS_THROUGH:

            if(!Irp->AssociatedIrp.SystemBuffer)
                goto ioctl_do_default;

            if(IoControlCode == IOCTL_SCSI_PASS_THROUGH_DIRECT) {
                Cdb = (PCDB)&(((PSCSI_PASS_THROUGH_DIRECT)(Irp->AssociatedIrp.SystemBuffer))->Cdb);
                CdbData = (PCHAR)(((PSCSI_PASS_THROUGH_DIRECT)(Irp->AssociatedIrp.SystemBuffer))->DataBuffer);
            } else {
                Cdb = (PCDB)&(((PSCSI_PASS_THROUGH)(Irp->AssociatedIrp.SystemBuffer))->Cdb);
                if(((PSCSI_PASS_THROUGH)(Irp->AssociatedIrp.SystemBuffer))->DataBufferOffset) {
                    CdbData = ((PCHAR)Cdb) +
                              ((PSCSI_PASS_THROUGH)(Irp->AssociatedIrp.SystemBuffer))->DataBufferOffset;
                } else {
                    CdbData = NULL;
                }
            }
            ScsiCommand = Cdb->AsByte[0];

            if(ScsiCommand == SCSIOP_WRITE) {
                UDFPrint(("Write10, LBA %2.2x%2.2x%2.2x%2.2x\n",
                         Cdb->CDB10.LogicalBlockByte0,
                         Cdb->CDB10.LogicalBlockByte1,
                         Cdb->CDB10.LogicalBlockByte2,
                         Cdb->CDB10.LogicalBlockByte3
                         ));
            } else
            if(ScsiCommand == SCSIOP_WRITE12) {
                UDFPrint(("Write12, LBA %2.2x%2.2x%2.2x%2.2x\n",
                         Cdb->CDB12.LogicalBlock[0],
                         Cdb->CDB12.LogicalBlock[1],
                         Cdb->CDB12.LogicalBlock[2],
                         Cdb->CDB12.LogicalBlock[3]
                         ));
            } else {
            }

            switch(ScsiCommand) {
            case SCSIOP_MODE_SELECT: {
//                PMODE_PARAMETER_HEADER ParamHdr = (PMODE_PARAMETER_HEADER)CdbData;
                ModeSelectData = CdbData+4;
                switch(ModeSelectData[0]) {
                case MODE_PAGE_WRITE_PARAMETERS:
                case MODE_PAGE_MRW:
                    UDFPrint(("Unsafe MODE_SELECT_6 via pass-through (%2.2x)\n", ModeSelectData[0]));
                    goto unsafe_direct_scsi_cmd;
                }
                break; }

            case SCSIOP_MODE_SELECT10: {
//                PMODE_PARAMETER_HEADER10 ParamHdr = (PMODE_PARAMETER_HEADER10)CdbData;
                ModeSelectData = CdbData+8;
                switch(ModeSelectData[0]) {
                case MODE_PAGE_WRITE_PARAMETERS:
                case MODE_PAGE_MRW:
                    UDFPrint(("Unsafe MODE_SELECT_10 via pass-through (%2.2x)\n", ModeSelectData[0]));
                    goto unsafe_direct_scsi_cmd;
                }
                break; }

            case SCSIOP_RESERVE_TRACK_RZONE:
            case SCSIOP_SEND_CUE_SHEET:
            case SCSIOP_SEND_DVD_STRUCTURE:
            case SCSIOP_CLOSE_TRACK_SESSION:
            case SCSIOP_FORMAT_UNIT:
            case SCSIOP_WRITE6:
            case SCSIOP_WRITE:
            case SCSIOP_BLANK:
            case SCSIOP_WRITE12:
            case SCSIOP_SET_STREAMING:
                UDFPrint(("UDF Direct media modification via pass-through (%2.2x)\n", ScsiCommand));
unsafe_direct_scsi_cmd:
                if(Vcb->VcbCondition != VcbMounted)
                    goto ioctl_do_default;

                UDFPrint(("Forget this volume\n"));
                // Acquire Vcb resource (Shared -> Exclusive)
                UDFInterlockedIncrement((PLONG)&(Vcb->VCBOpenCount));
                UDFReleaseResource(&(Vcb->VCBResource));

                if(!(Vcb->VCBFlags & UDF_VCB_FLAGS_RAW_DISK)) {
                    UDFCloseAllSystemDelayedInDir(Vcb, Vcb->RootDirFCB->FileInfo);
                }
#ifdef UDF_DELAYED_CLOSE
                //  Acquire exclusive access to the Vcb.
                UDFCloseAllDelayed(Vcb);
#endif //UDF_DELAYED_CLOSE

                UDFAcquireResourceExclusive(&(Vcb->VCBResource), TRUE);
                AcquiredVcb = TRUE;
                UDFInterlockedDecrement((PLONG)&(Vcb->VCBOpenCount));

                UDFDoDismountSequence(Vcb, FALSE);
                Vcb->MediaLockCount = 0;

                if (Vcb->VcbCondition != VcbDismountInProgress) {

                    Vcb->VcbCondition = VcbInvalid;
                }

                Vcb->WriteSecurity = FALSE;

                // Release the Vcb resource.
                UDFReleaseResource(&(Vcb->VCBResource));
                AcquiredVcb = FALSE;

                // Make sure, that volume will never be quick-remounted
                // It is very important for ChkUdf utility and
                // some CD-recording libraries
                Vcb->SerialNumber--;

                UDFPrint(("Forgotten\n"));

                goto notify_media_change;

            case SCSIOP_START_STOP_UNIT:
            case SCSIOP_MEDIUM_REMOVAL:
                UDFPrint(("UDF Medium/Tray control IOCTL via pass-through\n"));
            }
            goto ioctl_do_default;

notify_media_change:
/*            Vcb->VCBFlags |= UDF_VCB_FLAGS_UNSAFE_IOCTL;
            // Make sure, that volume will never be quick-remounted
            // It is very important for ChkUdf utility and
            // some CD-recording libraries
            Vcb->SerialNumber--;
*/          goto ioctl_do_default;

        case FSCTL_ALLOW_EXTENDED_DASD_IO:

            UDFPrint(("UDFUserFsCtrlRequest: FSCTL_ALLOW_EXTENDED_DASD_IO\n"));
            // DASD i/o is always permitted
            // So, no-op this call
            RC = STATUS_SUCCESS;

            Irp->IoStatus.Information = 0;
            Irp->IoStatus.Status = STATUS_SUCCESS;
            CompleteIrp = TRUE;
            break;

        case FSCTL_IS_VOLUME_DIRTY:

            UDFPrint(("UDFUserFsCtrlRequest: FSCTL_IS_VOLUME_DIRTY\n"));
            // DASD i/o is always permitted
            // So, no-op this call
            RC = UDFIsVolumeDirty(PtrIrpContext, Irp);
            CompleteIrp = TRUE;
            break;

        case IOCTL_STORAGE_EJECT_MEDIA:
        case IOCTL_DISK_EJECT_MEDIA:
        case IOCTL_CDROM_EJECT_MEDIA: {

            UDFPrint(("UDF Reset/Eject request\n"));
            goto ioctl_do_default;
        }
        case IOCTL_CDROM_DISK_TYPE: {

            UDFPrint(("UDF Cdrom Disk Type\n"));
            CompleteIrp = TRUE;
            //  Verify the Vcb in this case to detect if the volume has changed.
            Irp->IoStatus.Information = 0;
            RC = UDFVerifyVcb(PtrIrpContext,Vcb);
            if(!NT_SUCCESS(RC))
                try_return(RC);

            //  Check the size of the output buffer.
            if (IrpSp->Parameters.DeviceIoControl.OutputBufferLength < sizeof(CDROM_DISK_DATA))
                try_return(RC = STATUS_BUFFER_TOO_SMALL);

            //  Copy the data from the Vcb.
            ((CDROM_DISK_DATA*)Irp->AssociatedIrp.SystemBuffer)->DiskData = CDROM_DISK_DATA_TRACK;

            Irp->IoStatus.Information = sizeof(CDROM_DISK_DATA);
            RC = STATUS_SUCCESS;
            break;
        }

        case IOCTL_STORAGE_MEDIA_REMOVAL:
        case IOCTL_DISK_MEDIA_REMOVAL:
        case IOCTL_CDROM_MEDIA_REMOVAL: {
            UDFPrint(("UDF Lock/Unlock\n"));
            PPREVENT_MEDIA_REMOVAL buffer = (PPREVENT_MEDIA_REMOVAL)(Irp->AssociatedIrp.SystemBuffer);
            if(!buffer) {
                if(Vcb->VcbCondition != VcbMounted) {
                    UDFPrint(("!mounted\n"));
                    goto ioctl_do_default;
                }
                UDFPrint(("abort\n"));
                CompleteIrp = TRUE;
                Irp->IoStatus.Information = 0;
                UnsafeIoctl = FALSE;
                RC = STATUS_INVALID_PARAMETER;
                break;
            }
            if(!buffer->PreventMediaRemoval &&
               !Vcb->MediaLockCount) {

                UDFPrint(("!locked + unlock req\n"));
                if(Vcb->VcbCondition != VcbMounted) {
                    UDFPrint(("!mounted\n"));
                    goto ioctl_do_default;
                }
#if 0
                // Acquire Vcb resource (Shared -> Exclusive)
                UDFInterlockedIncrement((PLONG)&(Vcb->VCBOpenCount));
                UDFReleaseResource(&(Vcb->VCBResource));

#ifdef UDF_DELAYED_CLOSE
                //  Acquire exclusive access to the Vcb.
                UDFCloseAllDelayed(Vcb);
#endif //UDF_DELAYED_CLOSE

                UDFAcquireResourceExclusive(&(Vcb->VCBResource), TRUE);
                UDFInterlockedDecrement((PLONG)&(Vcb->VCBOpenCount));

                UDFDoDismountSequence(Vcb, FALSE);
                Vcb->MediaLockCount = 0;
                // Release the Vcb resource.
                UDFReleaseResource(&(Vcb->VCBResource));
                AcquiredVcb = FALSE;
#else
                // just ignore
#endif
ignore_lock:
                UDFPrint(("ignore lock/unlock\n"));
                CompleteIrp = TRUE;
                Irp->IoStatus.Information = 0;
                RC = STATUS_SUCCESS;
                break;
            }
            if(buffer->PreventMediaRemoval) {
                UDFPrint(("lock req\n"));
                Vcb->MediaLockCount++;
                Vcb->VCBFlags |= UDF_VCB_FLAGS_MEDIA_LOCKED;
                UnsafeIoctl = FALSE;
            } else {
                UDFPrint(("unlock req\n"));
                if(Vcb->MediaLockCount) {
                    UDFPrint(("lock count %d\n", Vcb->MediaLockCount));
                    UnsafeIoctl = FALSE;
                    Vcb->MediaLockCount--;
                }
            }
            if(Vcb->VcbCondition != VcbMounted) {
                UDFPrint(("!mounted\n"));
                goto ioctl_do_default;
            }
            goto ignore_lock;
        }
        default:

            UDFPrint(("default processing Irp %x, ctx %x, DevIoCtl %x\n", Irp, PtrIrpContext, IoControlCode));
ioctl_do_default:

            // make sure volume is Sync'ed BEFORE sending unsafe IOCTL
            if(Vcb && UnsafeIoctl) {
                if(AcquiredVcb) {
                    UDFReleaseResource(&(Vcb->VCBResource));
                    AcquiredVcb = FALSE;
                }
                UDFAcquireResourceExclusive(&(Vcb->VCBResource), TRUE);
                AcquiredVcb = TRUE;
                UDFFlushLogicalVolume(NULL, NULL, Vcb, 0);
                UDFReleaseResource(&(Vcb->VCBResource));
                AcquiredVcb = FALSE;
                UDFPrint(("  sync'ed\n"));
            }

            if(AcquiredVcb) {
                UDFReleaseResource(&(Vcb->VCBResource));
                AcquiredVcb = FALSE;
            }

            // Invoke the lower level driver in the chain.
            //PtrNextIoStackLocation = IoGetNextIrpStackLocation(Irp);
            //*PtrNextIoStackLocation = *IrpSp;
            IoSkipCurrentIrpStackLocation(Irp);
/*
            // Set a completion routine.
            IoSetCompletionRoutine(Irp, UDFDevIoctlCompletion, PtrIrpContext, TRUE, TRUE, TRUE);
            // Send the request.
*/
            RC = IoCallDriver(Vcb->TargetDeviceObject, Irp);
            if(!CompleteIrp) {
                // since now we do not use IoSetCompletionRoutine()
                UDFReleaseIrpContext(PtrIrpContext);
            }
            break;
        }

        if(Vcb && UnsafeIoctl) {
            UDFPrint(("  set UnsafeIoctl\n"));
            Vcb->VCBFlags |= UDF_VCB_FLAGS_UNSAFE_IOCTL;
        }

try_exit: NOTHING;

    } _SEH2_FINALLY {

        if(AcquiredVcb) {
            UDFReleaseResource(&(Vcb->VCBResource));
            AcquiredVcb = FALSE;
        }

        if (!_SEH2_AbnormalTermination() &&
            CompleteIrp) {
            UDFPrint(("  complete Irp %x, ctx %x, status %x, iolen %x\n",
                Irp, PtrIrpContext, RC, Irp->IoStatus.Information));
            Irp->IoStatus.Status = RC;
            // complete the IRP
            IoCompleteRequest(Irp, IO_DISK_INCREMENT);
            // Release the IRP context
            UDFReleaseIrpContext(PtrIrpContext);
        }
    } _SEH2_END;

    return(RC);
} // end UDFCommonDeviceControl()


/*************************************************************************
*
* Function: UDFDevIoctlCompletion()
*
* Description:
*   Completion routine.
*
* Expected Interrupt Level (for execution) :
*
*  IRQL_PASSIVE_LEVEL
*
* Return Value: STATUS_SUCCESS
*
*************************************************************************/
NTSTATUS
NTAPI
UDFDevIoctlCompletion(
   PDEVICE_OBJECT          PtrDeviceObject,
   PIRP                    Irp,
   VOID                    *Context)
{
/*    PIO_STACK_LOCATION      IrpSp = NULL;
    ULONG                   IoControlCode = 0;*/
    PIRP_CONTEXT IrpContext = (PIRP_CONTEXT)Context;

    UDFPrint(("UDFDevIoctlCompletion Irp %x, ctx %x\n", Irp, Context));
    if (Irp->PendingReturned) {
        UDFPrint(("  IoMarkIrpPending\n"));
        IoMarkIrpPending(Irp);
    }

    UDFReleaseIrpContext(IrpContext);
/*    if(Irp->IoStatus.Status == STATUS_SUCCESS) {
        IrpSp = IoGetCurrentIrpStackLocation(Irp);
        IoControlCode = IrpSp->Parameters.DeviceIoControl.IoControlCode;

        switch(IoControlCode) {
        case IOCTL_CDRW_RESET_DRIVER: {
            Vcb->MediaLockCount = 0;
        }
        }
    }*/

    return STATUS_SUCCESS;
} // end UDFDevIoctlCompletion()


/*************************************************************************
*
* Function: UDFHandleQueryPath()
*
* Description:
*   Handle the MUP request.
*
* Expected Interrupt Level (for execution) :
*
*  IRQL_PASSIVE_LEVEL
*
* Return Value: STATUS_SUCCESS
*
*************************************************************************/
/*NTSTATUS UDFHandleQueryPath(
VOID           *BufferPointer)
{
    NTSTATUS                    RC = STATUS_SUCCESS;
    PQUERY_PATH_REQUEST RequestBuffer = (PQUERY_PATH_REQUEST)BufferPointer;
    PQUERY_PATH_RESPONSE    ReplyBuffer = (PQUERY_PATH_RESPONSE)BufferPointer;
    ULONG                       LengthOfNameToBeMatched = RequestBuffer->PathNameLength;
    ULONG                       LengthOfMatchedName = 0;
    WCHAR                *NameToBeMatched = RequestBuffer->FilePathName;

    UDFPrint(("UDFHandleQueryPath\n"));
    // So here we are. Simply check the name supplied.
    // We can use whatever algorithm we like to determine whether the
    // sent in name is acceptable.
    // The first character in the name is always a "\"
    // If we like the name sent in (probably, we will like a subset
    // of the name), set the matching length value in LengthOfMatchedName.

    // if (FoundMatch) {
    //      ReplyBuffer->LengthAccepted = LengthOfMatchedName;
    // } else {
    //      RC = STATUS_OBJECT_NAME_NOT_FOUND;
    // }

    return(RC);
}*/
