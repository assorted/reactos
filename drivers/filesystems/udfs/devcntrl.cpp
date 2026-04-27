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
NTAPI
UDFDevCtrlCompletionRoutine (
    IN PDEVICE_OBJECT DeviceObject,
    IN PIRP Irp,
    IN PVOID Contxt
    );

NTSTATUS
UDFDvdTransferKey (
    IN PIRP_CONTEXT IrpContext,
    IN PIRP Irp,
    IN PFCB Fcb
    )
{
    // TODO: Impl
    NTSTATUS Status = STATUS_NOT_IMPLEMENTED;

    UDFCompleteRequest(IrpContext, Irp, Status);

    return Status;
}

NTSTATUS
UDFDvdReadStructure (
    IN PIRP_CONTEXT IrpContext,
    IN PIRP Irp,
    IN PFCB Fcb
    )
{
    // TODO: Impl
    NTSTATUS Status = STATUS_NOT_IMPLEMENTED;

    UDFCompleteRequest(IrpContext, Irp, Status);

    return Status;
}

BOOLEAN
UdfIsVolumeModifyingScsiOp(
    IN UCHAR OperationCode
    )
{
    switch (OperationCode)
    {
    case SCSIOP_BLANK:
    case SCSIOP_CLOSE_TRACK_SESSION:
    case SCSIOP_ERASE:
    case SCSIOP_FORMAT_UNIT:
    case SCSIOP_RESERVE_TRACK_RZONE:
    case SCSIOP_SYNCHRONIZE_CACHE:
    case SCSIOP_SET_READ_AHEAD:
    case SCSIOP_SEND_VOLUME_TAG:
    case SCSIOP_WRITE:
    case SCSIOP_WRITE6:
    case SCSIOP_WRITE12:
    case SCSIOP_WRITE_VERIFY:
        return TRUE;
    default:
        break;
    }

    return FALSE;
}

NTSTATUS
UDFCommonDevControl(PIRP_CONTEXT IrpContext, PIRP Irp)
{
    BOOLEAN DeviceAcquired = FALSE;
    BOOLEAN IsOpticalWriteRModeActive = FALSE;
    PIO_STACK_LOCATION IrpSp = IoGetCurrentIrpStackLocation(Irp);
    PCDB Cdb = NULL;
    PVCB Vcb;
    PFCB Fcb;
    PCCB Ccb;
    NTSTATUS Status;
    TYPE_OF_OPEN TypeOfOpen;
    ULONG IoControlCode;

    PAGED_CODE();

    // Extract and decode the file object.

    TypeOfOpen = UDFDecodeFileObject(IrpSp->FileObject, &Fcb, &Ccb);

    Vcb = Fcb->Vcb;

    ASSERT_CCB(Ccb);
    ASSERT_FCB(Fcb);
    ASSERT_VCB(Vcb);

    IoControlCode = IrpSp->Parameters.DeviceIoControl.IoControlCode;

    if (FlagOn(Vcb->VcbState, VCB_STATE_RMW_INITIALIZED) ||
        FlagOn(Vcb->VcbState, VCB_STATE_SEQUENCE_CACHE)) {

        IsOpticalWriteRModeActive = TRUE;
    }

    if (TypeOfOpen == UserFileOpen) {

        UDF_CHECK_PAGING_IO_RESOURCE(Fcb);
        UDFAcquireFcbShared(IrpContext, Fcb, FALSE);

        _SEH2_TRY {

            UDFVerifyFcbOperation(IrpContext, Fcb, Ccb);

            if (IsOpticalWriteRModeActive)
            {
                UDFAcquireDeviceShared(IrpContext, Vcb, NULL);
                DeviceAcquired = TRUE;
            }

            switch (IrpSp->Parameters.DeviceIoControl.IoControlCode) {
            case IOCTL_STORAGE_SET_READ_AHEAD:

                Status = STATUS_SUCCESS;

                UDFCompleteRequest(IrpContext, Irp, Status);
                break;

            case IOCTL_DVD_READ_KEY:
            case IOCTL_DVD_SEND_KEY:

                Status = UDFDvdTransferKey(IrpContext, Irp, Fcb);
                break;
            
            case IOCTL_DVD_READ_STRUCTURE:

                Status = UDFDvdReadStructure(IrpContext, Irp, Fcb);
                break;

            default:

                Status = STATUS_INVALID_PARAMETER;
                UDFCompleteRequest(IrpContext, Irp, Status);
                break;
            }

        } _SEH2_FINALLY {

            if (DeviceAcquired)
                UDFReleaseDevice(IrpContext, Vcb, NULL);

            UDFReleaseFcb(IrpContext, Fcb);
        } _SEH2_END;

        return Status;
    }

    if (TypeOfOpen != UserVolumeOpen) {

        UDFCompleteRequest(IrpContext, Irp, STATUS_INVALID_PARAMETER);
        return STATUS_INVALID_PARAMETER;
    }

    switch (IoControlCode) {
    case IOCTL_VOLSNAP_FLUSH_AND_HOLD_WRITES:

        UDFCompleteRequest(IrpContext, Irp, STATUS_NOT_SUPPORTED);
        UDFPrint(("UDFCommonDevControl -> %08lx\n", STATUS_NOT_SUPPORTED));
        return STATUS_NOT_SUPPORTED;

    case IOCTL_CDROM_DISK_TYPE:

        // Verify the Vcb in this case to detect if the volume has changed.

        UDFVerifyVcb(IrpContext, Vcb);

        // Check the size of the output buffer.
 
        if (IrpSp->Parameters.DeviceIoControl.OutputBufferLength < sizeof(CDROM_DISK_DATA)) {

            UDFCompleteRequest(IrpContext, Irp, STATUS_BUFFER_TOO_SMALL);
            return STATUS_BUFFER_TOO_SMALL;
        }

        ((PCDROM_DISK_DATA) Irp->AssociatedIrp.SystemBuffer)->DiskData = CDROM_DISK_DATA_TRACK;

        Irp->IoStatus.Information = sizeof(CDROM_DISK_DATA);
        UDFCompleteRequest(IrpContext, Irp, STATUS_SUCCESS);
        return STATUS_SUCCESS;

    case IOCTL_CDROM_EJECT_MEDIA:
    case IOCTL_STORAGE_EJECT_MEDIA:
    case IOCTL_DISK_EJECT_MEDIA:

        if (!FlagOn(Vcb->VcbState, VCB_STATE_PNP_NOTIFICATION)) {

            UDFAcquireVcbExclusive(IrpContext, Vcb, FALSE);

            if (Vcb->VcbCondition == VcbMounted && FALSE /*IsWritableOpticalMedia()*/) {

                if (FlagOn(Vcb->VcbState, VCB_STATE_SEQUENCE_CACHE)) {

                    FsRtlNotifyVolumeEvent(IrpSp->FileObject, FSRTL_VOLUME_PREPARING_EJECT);
                    //UdfSeqCacheCloseSession(IrpContext, 0);

                } else {

                    //UdfStopBackgroundFormat(IrpContext, Vcb);
                }
            }

            UDFReleaseVcb(IrpContext, Vcb);
        }

        break;

    case IOCTL_SCSI_PASS_THROUGH:
    case IOCTL_SCSI_PASS_THROUGH_DIRECT:
    case IOCTL_SCSI_PASS_THROUGH_EX:
    case IOCTL_SCSI_PASS_THROUGH_DIRECT_EX:

        //  If someone is issuing a format unit command underneath us, then make
        //  sure we mark the device as needing verification when they close their
        //  handle.

        if ((!FlagOn(IrpSp->FileObject->Flags, FO_FILE_MODIFIED) ||
            !FlagOn(Ccb->Flags, CCB_FLAG_SENT_FORMAT_UNIT)) &&
            (Irp->AssociatedIrp.SystemBuffer != NULL)) {

            Cdb = NULL;

            //  If this is a 32 bit application running on 64 bit then thunk the
            //  input structures to grab the Cdb.

#if defined (_WIN64)
            if (IoIs32bitProcess(Irp)) {

                if ((IrpSp->Parameters.DeviceIoControl.IoControlCode == IOCTL_SCSI_PASS_THROUGH) ||
                    (IrpSp->Parameters.DeviceIoControl.IoControlCode == IOCTL_SCSI_PASS_THROUGH_DIRECT)) {

                    if (IrpSp->Parameters.DeviceIoControl.InputBufferLength >= sizeof(SCSI_PASS_THROUGH32)) {

                        Cdb = (PCDB)((PSCSI_PASS_THROUGH32)(Irp->AssociatedIrp.SystemBuffer))->Cdb;
                    }
                }
                else {

                    if (IrpSp->Parameters.DeviceIoControl.InputBufferLength >= sizeof(SCSI_PASS_THROUGH32_EX)) {

                        Cdb = (PCDB)((PSCSI_PASS_THROUGH32_EX)(Irp->AssociatedIrp.SystemBuffer))->Cdb;
                    }
                }
            }
            else {
#endif
                if ((IrpSp->Parameters.DeviceIoControl.IoControlCode == IOCTL_SCSI_PASS_THROUGH) ||
                    (IrpSp->Parameters.DeviceIoControl.IoControlCode == IOCTL_SCSI_PASS_THROUGH_DIRECT)) {

                    if (IrpSp->Parameters.DeviceIoControl.InputBufferLength >= sizeof(SCSI_PASS_THROUGH)) {

                        Cdb = (PCDB)((PSCSI_PASS_THROUGH)(Irp->AssociatedIrp.SystemBuffer))->Cdb;
                    }
                }
                else {

                    if (IrpSp->Parameters.DeviceIoControl.InputBufferLength >= sizeof(SCSI_PASS_THROUGH_EX)) {

                        Cdb = (PCDB)((PSCSI_PASS_THROUGH_EX)(Irp->AssociatedIrp.SystemBuffer))->Cdb;
                    }
                }

#if defined (_WIN64)
            }
#endif
            if (Cdb != NULL && UdfIsVolumeModifyingScsiOp(Cdb->AsByte[0])) {

                if (Cdb->AsByte[0] == SCSIOP_BLANK ||
                    Cdb->AsByte[0] == SCSIOP_FORMAT_UNIT ||
                    Cdb->AsByte[0] == SCSIOP_CLOSE_TRACK_SESSION) {

                    SetFlag(Ccb->Flags, CCB_FLAG_SENT_FORMAT_UNIT);
                }

                SetFlag(IrpSp->FileObject->Flags, FO_FILE_MODIFIED);
            }
        }

        break;

    case IOCTL_DISK_COPY_DATA:

        // We cannot allow this IOCTL to be sent unless the volume is locked,
        // since this IOCTL allows direct writing of data to the volume.
        // We do allow kernel callers to force access via a flag.  A handle that
        // issued a dismount can send this IOCTL as well.

        if (!FlagOn(Vcb->VcbState, VCB_STATE_LOCKED) &&
            !FlagOn(IrpSp->Flags, SL_FORCE_DIRECT_WRITE) &&
            !FlagOn(Ccb->Flags, CCB_FLAG_DISMOUNT_ON_CLOSE)) {

            UDFCompleteRequest(IrpContext, Irp, STATUS_ACCESS_DENIED);
            UDFPrint(("UDFCommonDevControl -> %08lx\n", STATUS_ACCESS_DENIED));
            return STATUS_ACCESS_DENIED;
        }
        break;
    }

    IoCopyCurrentIrpStackLocationToNext(Irp);

    IoSetCompletionRoutine(Irp,
                           UDFDevCtrlCompletionRoutine,
                           NULL,
                           TRUE,
                           TRUE,
                           TRUE);

    if (IsOpticalWriteRModeActive) {

        UDFAcquireDeviceShared(IrpContext, Vcb, NULL);
        DeviceAcquired = TRUE;
    }

    Status = IoCallDriver(Vcb->TargetDeviceObject, Irp);

    if (DeviceAcquired)
        UDFReleaseDevice(IrpContext, Vcb, NULL);

    UDFCompleteRequest(IrpContext, NULL, STATUS_SUCCESS);
    return Status;
}

NTSTATUS
NTAPI
UDFDevCtrlCompletionRoutine (
    IN PDEVICE_OBJECT DeviceObject,
    IN PIRP Irp,
    IN PVOID Contxt
    )

{
    //  Add the hack-o-ramma to fix formats.

    if (Irp->PendingReturned) {

        IoMarkIrpPending(Irp);
    }

    return STATUS_SUCCESS;

    UNREFERENCED_PARAMETER(DeviceObject);
    UNREFERENCED_PARAMETER(Contxt);
}
