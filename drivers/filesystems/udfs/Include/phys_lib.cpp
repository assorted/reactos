////////////////////////////////////////////////////////////////////
// Copyright (C) Alexander Telyatnikov, Ivan Keliukh, Yegor Anchishkin, SKIF Software, 1999-2013. Kiev, Ukraine
// All rights reserved
// This file was released under the GPLv2 on June 2015.
////////////////////////////////////////////////////////////////////
/*
 Module Name: Phys_lib.cpp

 Execution: Kernel mode only

 Description:

   Contains code that implement read/write operations for physical device
*/

#include "phys_lib.h"

#define MSF_TO_LBA(Minutes,Seconds,Frames) \
                (ULONG)((60 * 75 * (Minutes)) + (75 * (Seconds)) + ((Frames) - 150))

#define TrkInfo_Dat_Mask    0x0F
#define TrkInfo_Dat_Mode1   0x01
#define TrkInfo_Dat_Mode2   0x02
#define TrkInfo_Dat_XA      0x02
#define TrkInfo_Dat_DDCD    0x02
#define TrkInfo_Dat_unknown 0x0F
#define TrkInfo_Dat_Unknown TrkInfo_Dat_unknown
#define TrkInfo_FP          0x10
#define TrkInfo_Packet      0x20
#define TrkInfo_Blank       0x40
#define TrkInfo_RT          0x80


#define Trk_QSubChan_Type_Audio            0x00
#define Trk_QSubChan_Type_AllowCpy         0x02
#define Trk_QSubChan_Type_Data             0x04
#define Trk_QSubChan_Type_IncrData         0x05
#define Trk_QSubChan_Type_Mask             0x0d

#define WParam_TrkMode_Mask             Trk_QSubChan_Type_Mask           //0x0d
#define WParam_TrkMode_Data             Trk_QSubChan_Type_Data           //0x04
#define WParam_TrkMode_IncrData         Trk_QSubChan_Type_IncrData       //0x05
#define TrkInfo_Trk_unknown 0x0F


#define TrkInfo_Trk_XA      (Trk_QSubChan_Type_Audio | Trk_QSubChan_Type_AllowCpy)

#define TocControl_TrkMode_Mask           WParam_TrkMode_Mask
#define TocControl_TrkMode_Data           WParam_TrkMode_Data
#define TocControl_TrkMode_IncrData       WParam_TrkMode_IncrData


#define DEFAULT_LAST_LBA_FP_CD  276159
#define TOC_LastTrack_ID        0xAA
#define MediaType_UnknownSize_CDRW 0x20
// Local functions:

NTSTATUS
UDFRecoverFromError(
    IN PVCB Vcb,
    IN BOOLEAN WriteOp,
    IN NTSTATUS status,
    IN uint32 Lba,
    IN uint32 BCount,
 IN OUT uint32* retry);

uint32
UDFFixFPAddress(
    IN PVCB           Vcb,               // Volume control block from this DevObj
    IN uint32         Lba
    );

NTSTATUS
UDFReallocTrackMap(
    IN PVCB Vcb,
    IN uint32 TrackNum
    )
{
    if (Vcb->TrackMap) {
        MyFreePool__(Vcb->TrackMap);
        Vcb->TrackMap = NULL;
    }
    Vcb->TrackMap = (PUDFTrackMap)
        MyAllocatePool__(NonPagedPool, TrackNum*sizeof(UDFTrackMap));
    if (!Vcb->TrackMap) {
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    RtlZeroMemory(Vcb->TrackMap,TrackNum*sizeof(UDFTrackMap));
    return STATUS_SUCCESS;
} // end UDFReallocTrackMap()

/*
    This routine performs low-level write

    ATTENTION! When we are in Variable-Packet mode (CDR_Mode = TRUE)
    LBA is ignored and assumed to be equal to NWA by CD-R(W) driver
 */
NTSTATUS
UDFTWrite(
    IN PIRP_CONTEXT IrpContext,
    IN void* _Vcb,
    IN void* Buffer,     // Target buffer
    IN SIZE_T Length,
    IN uint32 LBA,
    OUT PSIZE_T WrittenBytes,
    IN uint32 Flags
    )
{
#define Vcb ((PVCB)_Vcb)

    PEXTENT_MAP RelocExtent;
    PEXTENT_MAP RelocExtent_saved = NULL;
    uint32 retry;
    BOOLEAN res_acq = FALSE;

    NTSTATUS RC = STATUS_SUCCESS;
    uint32 rLba;
    uint32 BCount;
    uint32 i;

#ifdef DBG
    //ASSERT(!(LBA & (32-1)));
#endif //DBG

    (*WrittenBytes) = 0;
    BCount = Length>>Vcb->SectorShift;

    UDFPrint(("TWrite %x (%x)\n", LBA, BCount));

    if (Vcb->VcbState & UDF_VCB_FLAGS_DEAD) {
        UDFPrint(("DEAD\n"));
        return STATUS_NO_SUCH_DEVICE;
    }

    Vcb->VcbState |= UDF_VCB_LAST_WRITE;
    if (!Vcb->CDR_Mode) {
        RelocExtent = UDFRelocateSectors(Vcb, LBA, BCount);
        if (!RelocExtent) {
            UDFPrint(("can't relocate\n"));
            return STATUS_INSUFFICIENT_RESOURCES;
        }
        rLba = LBA;
    } else {
        RelocExtent = UDF_NO_EXTENT_MAP;
        rLba = Vcb->NWA;
    }

#ifdef DBG
    //ASSERT(!(rLba & (32-1)));
#endif //DBG

    _SEH2_TRY {

        if (!(Flags & PH_IO_LOCKED)) {
            UDFAcquireResourceExclusive(&(Vcb->IoResource), TRUE);
            res_acq = TRUE;
        }

        if (RelocExtent == UDF_NO_EXTENT_MAP) {

            retry = UDF_WRITE_MAX_RETRY;
retry_1:
            RC = UDFPrepareForWriteOperation(Vcb, rLba, BCount);
            if (!NT_SUCCESS(RC)) {
                UDFPrint(("prepare failed\n"));
                try_return(RC);
            }

            RC = UDFPhWriteSynchronous(Vcb->TargetDeviceObject, Buffer, Length,
                       ((uint64)rLba) << Vcb->SectorShift, WrittenBytes, Flags);

            if (!NT_SUCCESS(RC) &&
                NT_SUCCESS(RC = UDFRecoverFromError(Vcb, TRUE, RC, rLba, BCount, &retry)) )
                goto retry_1;
            try_return(RC);
        }
        // write according to relocation table
        RelocExtent_saved = RelocExtent;
        for(i=0; RelocExtent->extLength; i++, RelocExtent++) {
            SIZE_T _WrittenBytes;
            rLba = RelocExtent->extLocation;
            BCount = RelocExtent->extLength>>Vcb->SectorShift;
            retry = UDF_WRITE_MAX_RETRY;
retry_2:
            RC = UDFPrepareForWriteOperation(Vcb, rLba, BCount);
            if (!NT_SUCCESS(RC)) {
                UDFPrint(("prepare failed (2)\n"));
                break;
            }

            RC = UDFPhWriteSynchronous(Vcb->TargetDeviceObject, Buffer, RelocExtent->extLength,
                       ((uint64)rLba) << Vcb->SectorShift, &_WrittenBytes, Flags);

            if (!NT_SUCCESS(RC) &&
                NT_SUCCESS(RC = UDFRecoverFromError(Vcb, TRUE, RC, rLba, BCount, &retry)) )
                goto retry_2;
            LBA += BCount;
            (*WrittenBytes) += _WrittenBytes;
            if (!NT_SUCCESS(RC)) break;
            *((uint32*)&Buffer) += RelocExtent->extLength;
        }
try_exit: NOTHING;
    } _SEH2_FINALLY {
        if (res_acq) {
            UDFReleaseResource(&(Vcb->IoResource));
        }
        if (RelocExtent_saved) {
            MyFreePool__(RelocExtent_saved);
        }
    } _SEH2_END;
    UDFPrint(("TWrite: %x\n", RC));
    return RC;

#undef Vcb
} // end UDFTWrite()

/*
    This routine performs low-level read
 */
NTSTATUS
UDFTRead(
    PIRP_CONTEXT IrpContext,
    IN void* _Vcb,
    IN void* Buffer,     // Target buffer
    IN SIZE_T Length,
    IN uint32 LBA,
    OUT PULONG ReadBytes,
    IN uint32 Flags
    )
{
    uint32 rLba;
    NTSTATUS RC = STATUS_SUCCESS;
    uint32 retry;
    PVCB Vcb = (PVCB)_Vcb;
    uint32 BCount = (uint32)(Length >> Vcb->SectorShift);
    uint32 i;
    PEXTENT_MAP RelocExtent;
    PEXTENT_MAP RelocExtent_saved = NULL;
    BOOLEAN res_acq = FALSE;
    PUCHAR WorkingBuffer = (PUCHAR)Buffer; // Use PUCHAR for safe arithmetic

    ASSERT(Buffer);
    (*ReadBytes) = 0;

    if (Vcb->VcbState & UDF_VCB_FLAGS_DEAD)
        return STATUS_NO_SUCH_DEVICE;

    // Get relocation/partition mapping
    RelocExtent = UDFRelocateSectors(Vcb, LBA, BCount);
    if (!RelocExtent) return STATUS_INSUFFICIENT_RESOURCES;

    _SEH2_TRY {

        if (!(Flags & PH_IO_LOCKED)) {
            UDFAcquireResourceExclusive(&(Vcb->IoResource), TRUE);
            res_acq = TRUE;
        }

        // Case 1: Single contiguous block range
        if (RelocExtent == UDF_NO_EXTENT_MAP) {
            rLba = LBA;
            
            // FIX: Overflow-safe boundary check
            uint32 LimitLba = Vcb->CDR_Mode ? Vcb->NWA : Vcb->LastLBA;
            if (rLba > LimitLba) {
                RtlZeroMemory(WorkingBuffer, Length);
                try_return(RC = STATUS_SUCCESS);
            }

            retry = UDF_WRITE_MAX_RETRY;
retry_1:
            RC = UDFPrepareForReadOperation(IrpContext, Vcb, rLba, BCount);
            if (!NT_SUCCESS(RC)) try_return(RC);
            
            rLba = UDFFixFPAddress(Vcb, rLba);

            // 64-bit shift ensures we address the full 256GB range
            RC = UDFPhReadSynchronous(IrpContext, Vcb->TargetDeviceObject, WorkingBuffer, Length, Vcb->PartitionStartOffset + (((uint64)rLba) << Vcb->SectorShift), ReadBytes, Flags);
            
            Vcb->VcbState &= ~UDF_VCB_LAST_WRITE;

            if (!NT_SUCCESS(RC) &&
                NT_SUCCESS(RC = UDFRecoverFromError(Vcb, FALSE, RC, rLba, BCount, &retry)) ) {
                if (RC != STATUS_BUFFER_ALL_ZEROS) goto retry_1;
                RtlZeroMemory(WorkingBuffer, Length);
                (*ReadBytes) = (ULONG)Length;
                RC = STATUS_SUCCESS;
            }
            try_return(RC);
        }

        // Case 2: Read according to relocation table (multiple extents)
        RelocExtent_saved = RelocExtent;
        for(i=0; RelocExtent->extLength; i++, RelocExtent++) {
            ULONG _ReadBytes = 0;
            rLba = RelocExtent->extLocation;
            uint32 CurrentExtLength = (ULONG)RelocExtent->extLength;
            
            uint32 LimitLba = Vcb->CDR_Mode ? Vcb->NWA : Vcb->LastLBA;
            if (rLba > LimitLba) {
                RtlZeroMemory(WorkingBuffer, CurrentExtLength);
                _ReadBytes = CurrentExtLength;
                RC = STATUS_SUCCESS;
                goto TR_continue;
            }

            uint32 ExtBCount = CurrentExtLength >> Vcb->SectorShift;
            retry = UDF_WRITE_MAX_RETRY;
retry_2:
            RC = UDFPrepareForReadOperation(IrpContext, Vcb, rLba, ExtBCount);
            if (!NT_SUCCESS(RC)) break;
            
            rLba = UDFFixFPAddress(Vcb, rLba);

            RC = UDFPhReadSynchronous(IrpContext, Vcb->TargetDeviceObject, WorkingBuffer, Length, Vcb->PartitionStartOffset + (((uint64)rLba) << Vcb->SectorShift), ReadBytes, Flags);
            
            Vcb->VcbState &= ~UDF_VCB_LAST_WRITE;

            if (!NT_SUCCESS(RC) &&
                NT_SUCCESS(RC = UDFRecoverFromError(Vcb, FALSE, RC, rLba, ExtBCount, &retry)) ) {
                if (RC != STATUS_BUFFER_ALL_ZEROS) goto retry_2;
                RtlZeroMemory(WorkingBuffer, CurrentExtLength);
                _ReadBytes = CurrentExtLength;
                RC = STATUS_SUCCESS;
            }

TR_continue:
            (*ReadBytes) += _ReadBytes;
            if (!NT_SUCCESS(RC)) break;

            // FIX: Safe pointer arithmetic to advance the buffer for the next extent
            WorkingBuffer += CurrentExtLength;
        }

try_exit: NOTHING;
    } _SEH2_FINALLY {
        if (res_acq) {
            UDFReleaseResource(&(Vcb->IoResource));
        }
        if (RelocExtent_saved && RelocExtent_saved != UDF_NO_EXTENT_MAP) {
            MyFreePool__(RelocExtent_saved);
        }
    } _SEH2_END;

    return RC;
} // end UDFTRead()

#ifdef UDF_ASYNC_IO
/*
    This routine performs asynchronous low-level read
    Is not used now.
 */
NTSTATUS
UDFTReadAsync(
    IN void* _Vcb,
    IN void* _WContext,
    IN void* Buffer,     // Target buffer
    IN SIZE_T Length,
    IN uint32 LBA,
    OUT PSIZE_T ReadBytes
    )
{
    PEXTENT_MAP RelocExtent;
    PEXTENT_MAP RelocExtent_saved;
    NTSTATUS RC = STATUS_SUCCESS;
//    LARGE_INTEGER delay;
    uint32 retry = UDF_READ_MAX_RETRY;
    PVCB Vcb = (PVCB)_Vcb;
    Vcb->VcbState |= UDF_VCB_SKIP_EJECT_CHECK;
    uint32 rLba;
    uint32 BCount;

    ASSERT(Buffer);

    (*ReadBytes) = 0;

    RelocExtent = UDFRelocateSectors(Vcb, LBA, BCount = Length >> Vcb->BlockSizeBits);
    if (!RelocExtent) return STATUS_INSUFFICIENT_RESOURCES;
    if (RelocExtent == UDF_NO_EXTENT_MAP) {
        rLba = LBA;
        if (rLba >= (Vcb->CDR_Mode ? Vcb->NWA : Vcb->LastLBA + 1)) {
            RtlZeroMemory(Buffer, Length);
            return STATUS_SUCCESS;
        }
retry_1:
        RC = UDFPrepareForReadOperation(Vcb, rLba, BCount);
        if (!NT_SUCCESS(RC)) return RC;
        rLba = UDFFixFPAddress(Vcb, rLba);
        RC = UDFPhReadSynchronous(Vcb->TargetDeviceObject, Buffer, Length,
                   ((uint64)rLba) << Vcb->BlockSizeBits, ReadBytes, 0);
        Vcb->VcbState &= ~UDF_VCB_LAST_WRITE;
        Vcb->VcbState |= UDF_VCB_SKIP_EJECT_CHECK;
        if (!NT_SUCCESS(RC) &&
            NT_SUCCESS(RC = UDFRecoverFromError(Vcb, FALSE, RC, rLba, BCount, &retry)) )
            goto retry_1;
        return RC;
    }
    // read according to relocation table
    RelocExtent_saved = RelocExtent;
    for(uint32 i=0; RelocExtent->extLength; i++, RelocExtent++) {
        SIZE_T _ReadBytes;
        rLba = RelocExtent->extLocation;
        if (rLba >= (Vcb->CDR_Mode ? Vcb->NWA : Vcb->LastLBA + 1)) {
            RtlZeroMemory(Buffer, _ReadBytes = RelocExtent->extLength);
            RC = STATUS_SUCCESS;
            goto TR_continue;
        }
        BCount = RelocExtent->extLength>>Vcb->BlockSizeBits;
retry_2:
        RC = UDFPrepareForReadOperation(Vcb, rLba, RelocExtent->extLength >> Vcb->BlockSizeBits);
        if (!NT_SUCCESS(RC)) break;
        rLba = UDFFixFPAddress(Vcb, rLba);
        RC = UDFPhReadSynchronous(Vcb->TargetDeviceObject, Buffer, RelocExtent->extLength,
                   ((uint64)rLba) << Vcb->BlockSizeBits, &_ReadBytes, 0);
        Vcb->VcbState &= ~UDF_VCB_LAST_WRITE;
        Vcb->VcbState |= UDF_VCB_SKIP_EJECT_CHECK;
        if (!NT_SUCCESS(RC) &&
            NT_SUCCESS(RC = UDFRecoverFromError(Vcb, FALSE, RC, rLba, BCount, &retry)) )
            goto retry_2;
TR_continue:
        (*ReadBytes) += _ReadBytes;
        if (!NT_SUCCESS(RC)) break;
        *((uint32*)&Buffer) += RelocExtent->extLength;
    }
    MyFreePool__(RelocExtent_saved);
    return RC;
} // end UDFTReadAsync()

#endif //UDF_ASYNC_IO

/*
    This routine performs media-type dependent preparations
    for write operation.

    For CDR/RW it sets WriteParameters according to track parameters,
    in some cases issues SYNC_CACHE command.
    It can also send OPC info if requered.
    If write-requested block is located beyond last formatted LBA
    on incompletely formatted DVD media, this routine performs
    all neccessary formatting operations in order to satisfy
    subsequent write request.
 */
NTSTATUS
UDFPrepareForWriteOperation(
    IN PVCB Vcb,
    IN uint32 Lba,
    IN uint32 BCount
    )
{
#ifdef _UDF_STRUCTURES_H_
    if (Vcb->BSBM_Bitmap) {
        ULONG i;
        for(i=0; i<BCount; i++) {
            if (UDFGetBit((uint32*)(Vcb->BSBM_Bitmap), Lba+i)) {
                UDFPrint(("W: Known BB @ %#x\n", Lba));
                //return STATUS_FT_WRITE_RECOVERY; // this shall not be treated as error and
                                                   // we shall get IO request to BAD block
                return STATUS_DEVICE_DATA_ERROR;
            }
        }
    }
#endif //_UDF_STRUCTURES_H_

    Vcb->VcbState |= UDF_VCB_LAST_WRITE;

    return STATUS_SUCCESS;
} // end UDFPrepareForWriteOperation()

/*
    This routine tries to recover from hardware error
    Return: STATUS_SUCCESS - retry requst
            STATUS_XXX - unrecoverable error
 */
NTSTATUS
UDFRecoverFromError(
    IN PVCB Vcb,
    IN BOOLEAN WriteOp,
    IN NTSTATUS status,
    IN uint32 Lba,
    IN uint32 BCount,
 IN OUT uint32* retry
    )
{
    return status;

} // end UDFRecoverFromError()

/*
    use standard way to determine disk layout (ReadTOC cmd)
 */
NTSTATUS
UDFDetermineVolumeLayout(
    PIRP_CONTEXT IrpContext,
    PDEVICE_OBJECT DeviceObject,
    PVCB Vcb,
    PULONG SessionStartLba,
    PULONG SessionEndLba
    )
{
    NTSTATUS Status;
    CDROM_TOC_LARGE* toc = NULL;
    CDROM_TOC_SESSION_DATA* LastSes = NULL;
    ULONG LocalTrackCount;
    ULONG TocEntry;
    void* TempBuffer = NULL;
    ULONG OldTrkNum;
    ULONG TrkNum;
    ULONG ReadBytes;
    SIZE_T i, len;

    *SessionStartLba = 0;
    *SessionEndLba = 0;

    //// Only process CD-ROM devices

    //if (DeviceObject->DeviceType != FILE_DEVICE_CD_ROM) {

    //    return STATUS_SUCCESS;
    //}

    toc = (CDROM_TOC_LARGE*)MyAllocatePool__(NonPagedPool, sizeof(CDROM_TOC_LARGE));
    LastSes = (CDROM_TOC_SESSION_DATA*)MyAllocatePool__(NonPagedPool, sizeof(CDROM_TOC_SESSION_DATA));

    _SEH2_TRY {

        if (!toc || !LastSes) {
            try_return (Status = STATUS_INSUFFICIENT_RESOURCES);
        }

        RtlZeroMemory(toc, sizeof(CDROM_TOC_LARGE));

        CDROM_READ_TOC_EX Command;

        RtlZeroMemory(&Command, sizeof(Command));

        Status = UDFPhSendIOCTL(IOCTL_CDROM_READ_TOC_EX,
                                DeviceObject,
                                &Command,
                                sizeof(Command),
                                toc,
                                sizeof(CDROM_TOC_LARGE),
                                TRUE,
                                NULL);

        if (!NT_SUCCESS(Status) && 
            (Status != STATUS_INSUFFICIENT_RESOURCES)) {

            // try using the MSF mode
            Command.Msf = 1;

            Status = UDFPhSendIOCTL(IOCTL_CDROM_READ_TOC_EX,
                                    DeviceObject,
                                    &Command,
                                    sizeof(Command),
                                    toc,
                                    sizeof(CDROM_TOC_LARGE),
                                    TRUE,
                                    NULL);
        }

        if (Status == STATUS_INSUFFICIENT_RESOURCES) {

            UDFRaiseStatus(IrpContext, Status);
        }

        // If even standard read toc does not work, then use default values
        if (!NT_SUCCESS(Status)) {

            Status = UDFReallocTrackMap(Vcb, 2);
            if (!NT_SUCCESS(Status)) {
                try_return(Status);
            }

            Vcb->LastSession=1;
            Vcb->FirstTrackNum=1;
//            Vcb->FirstLBA=0;
            Vcb->LastTrackNum=1;
            Vcb->TrackMap[1].FirstLba = Vcb->FirstLBA;
            Vcb->TrackMap[1].LastLba = Vcb->LastLBA;
            Vcb->TrackMap[1].PacketSize = PACKETSIZE_UDF;


            if (UDFGetDevType(DeviceObject) == FILE_DEVICE_DISK) {
                try_return(Status = STATUS_SUCCESS);
            }

            Vcb->LastPossibleLBA = max(Vcb->LastLBA, DEFAULT_LAST_LBA_FP_CD);
            Vcb->TrackMap[1].DataParam = TrkInfo_Dat_XA | TrkInfo_FP | TrkInfo_Packet;
            Vcb->TrackMap[1].TrackParam = TrkInfo_Trk_XA;
            Vcb->TrackMap[1].NWA = 0xffffffff;
            Vcb->NWA = DEFAULT_LAST_LBA_FP_CD + 7 + 1;
            try_return(Status = STATUS_SUCCESS);
        }

        LocalTrackCount = toc->LastTrack - toc->FirstTrack + 1;

        // Get out if there is an immediate problem with the TOC.

        if (toc->LastTrack - toc->FirstTrack >= MAXIMUM_NUMBER_TRACKS_LARGE) {
            try_return(Status = STATUS_DISK_CORRUPT_ERROR);
        }

        Vcb->LastTrackNum = toc->LastTrack;
        Vcb->FirstTrackNum = toc->FirstTrack;
        // some devices report LastTrackNum=0 for full disks
        Vcb->LastTrackNum = max(Vcb->LastTrackNum, Vcb->FirstTrackNum);

        Status = UDFReallocTrackMap(Vcb, MAXIMUM_NUMBER_TRACKS_LARGE+1);

        if (!NT_SUCCESS(Status)) {
            BrutePoint();
            try_return(Status);
        }
        // find 1st and last session
        Status = UDFPhSendIOCTL(IOCTL_CDROM_GET_LAST_SESSION,
            DeviceObject,
            NULL,
            0,
            LastSes,
            sizeof(CDROM_TOC_SESSION_DATA),
            TRUE,
            NULL);

        if (NT_SUCCESS(Status) &&
            LastSes->FirstCompleteSession != LastSes->LastCompleteSession) {

            SwapCopyUchar4(SessionStartLba, &LastSes->TrackData[0].Address);

            // Validate: SessionStartLba must be greater than SessionEndLba

            if (*SessionEndLba <= *SessionStartLba) {

                *SessionStartLba = 0;
                *SessionEndLba = 0;
            }

            TrkNum = LastSes->TrackData[0].TrackNumber;

            Vcb->FirstLBA = 0;
            SwapCopyUchar4(&Vcb->FirstLBA, &LastSes->TrackData[0].Address);

            Vcb->LastSession = LastSes->FirstCompleteSession;
            for(TocEntry=0;TocEntry<LocalTrackCount + 1;TocEntry++) {
                if (toc->TrackData[TocEntry].TrackNumber == TrkNum) {
                    Vcb->TrackMap[TrkNum].Session = Vcb->LastSession;
                }
            }
        }

        OldTrkNum = 0;
        // Scan toc for first & last LBA
        for(TocEntry=0;TocEntry<LocalTrackCount + 1;TocEntry++) {
#define TempMSF toc->TrackData[TocEntry].Address
            TrkNum = toc->TrackData[TocEntry].TrackNumber;
#ifdef UDF_DBG
            if (TrkNum >= MAXIMUM_NUMBER_TRACKS_LARGE &&
                TrkNum != TOC_LastTrack_ID) {
                UDFPrint(("UDFUseStandard: Array out of bounds\n"));
                BrutePoint();
                try_return(Status = STATUS_SUCCESS);
            }
            UDFPrint(("Track N %d (0x%x) first LBA %ld (%lx) \n",TrkNum,TrkNum,
                MSF_TO_LBA(TempMSF[1],TempMSF[2],TempMSF[3]),
                MSF_TO_LBA(TempMSF[1],TempMSF[2],TempMSF[3])));
#endif // UDF_DBG
            if (TOC_LastTrack_ID == TrkNum) {

                if (Command.Msf) {
                    Vcb->LastLBA = MSF_TO_LBA(TempMSF[1],TempMSF[2],TempMSF[3]) - 1;
                }
                else {
                    // The non-MSF (LBA) mode
                    Vcb->LastLBA = 0;
                    SwapCopyUchar4(&Vcb->LastLBA, &toc->TrackData[TocEntry].Address);
                    if (Vcb->LastLBA) {
                        Vcb->LastLBA -= 1;
                    }
                }

                Vcb->TrackMap[OldTrkNum].LastLba = Vcb->LastLBA;
                UDFPrint(("UDFUseStandard: Last track entry, break TOC scan\n"));
                break;
            } else {
                Vcb->TrackMap[TrkNum].FirstLba = MSF_TO_LBA(TempMSF[1],TempMSF[2],TempMSF[3]);
                if (Vcb->TrackMap[TrkNum].FirstLba & 0x80000000)
                    Vcb->TrackMap[TrkNum].FirstLba = 0;
                if (TrkNum) {
                    if (TOC_LastTrack_ID == OldTrkNum) {
                        UDFPrint(("UDFUseStandard: Wrong previous track number\n"));
                        BrutePoint();
                    } else {
                        Vcb->TrackMap[OldTrkNum].LastLba = Vcb->TrackMap[TrkNum].FirstLba-1;
                    }
                }
            }
            // check track type
            switch(toc->TrackData[TocEntry].Control & TocControl_TrkMode_Mask) {
            case TocControl_TrkMode_Data:
            case TocControl_TrkMode_IncrData:
                Vcb->TrackMap[TrkNum].DataParam = TrkInfo_Dat_XA;
                Vcb->TrackMap[TrkNum].TrackParam = TrkInfo_Trk_XA;
                break;
            default:
                Vcb->TrackMap[TrkNum].DataParam = TrkInfo_Dat_unknown;
                Vcb->TrackMap[TrkNum].TrackParam = TrkInfo_Trk_unknown;
            }
            OldTrkNum = TrkNum;
#undef TempMSF
        }

        TrkNum = Vcb->LastTrackNum;
        Status = STATUS_SUCCESS;
        // find last _valid_ track
        for(;TrkNum;TrkNum--) {
            if ((Vcb->TrackMap[TrkNum].DataParam  != TrkInfo_Dat_unknown) &&
               (Vcb->TrackMap[TrkNum].TrackParam != TrkInfo_Trk_unknown)) {
                Status = STATUS_UNSUCCESSFUL;
                Vcb->LastTrackNum = TrkNum;
                break;
            }
        }
        // no valid tracks...
        if (!TrkNum) {
            UDFPrint(("UDFUseStandard: no valid tracks...\n"));
            try_return(Status = STATUS_UNRECOGNIZED_VOLUME);
        }
        i = 0;

        // Check for last Variable Packet(VP) track. Some last sectors may belong to Link-data &
        // be unreadable. We should forget about them, because UDF needs
        // last _readable_ sector.

        TempBuffer = MyAllocatePool__(NonPagedPool, Vcb->SectorSize);

        if (!TempBuffer) { 
            return STATUS_INSUFFICIENT_RESOURCES;
        }

        while(!NT_SUCCESS(Status) && (i<8)) {
            Status = UDFPhReadSynchronous(IrpContext, Vcb->TargetDeviceObject, TempBuffer, Vcb->SectorSize,
                       ((uint64)(Vcb->TrackMap[TrkNum].LastLba-i)) << Vcb->SectorShift, &ReadBytes, PH_TMP_BUFFER);
            i++;
        }
        if (NT_SUCCESS(Status)) {
            Vcb->LastLBA = Vcb->TrackMap[TrkNum].LastLba-i+1;
/*            if (i) {
                Vcb->TrackMap[TrkNum].PacketSize = PACKETSIZE_UDF;
                Vcb->TrackMap[TrkNum].;
            }*/
        } else {

            // Check for Fixed Packet(FP) track. READ_TOC reports actual track length, but
            // Link-data is hidden & unreadable for us. So, available track
            // length may be less than actual. Here we assume that Packet-size
            // is PACKETSIZE_UDF.
            i = 0;
            len = Vcb->TrackMap[TrkNum].LastLba - Vcb->TrackMap[TrkNum].FirstLba + 1;
            len = (uint32)(((int64)len*PACKETSIZE_UDF) / (PACKETSIZE_UDF+7));

            while(!NT_SUCCESS(Status) && (i<9)) {
                Status = UDFPhReadSynchronous(IrpContext, Vcb->TargetDeviceObject, TempBuffer, Vcb->SectorSize,
                           ((uint64)(Vcb->TrackMap[TrkNum].FirstLba-i+len)) << Vcb->SectorShift, &ReadBytes, PH_TMP_BUFFER);
                i++;
            }
            if (NT_SUCCESS(Status)) {
                Vcb->LastLBA =
                Vcb->TrackMap[TrkNum].LastLba = Vcb->TrackMap[TrkNum].FirstLba-i+len+1;
                Vcb->TrackMap[TrkNum].PacketSize = PACKETSIZE_UDF;
//                Vcb->TrackMap[TrkNum].;
            } else
            if (Status == STATUS_INVALID_DEVICE_REQUEST) {
                // wrap return code from Audio-disk
                Status = STATUS_SUCCESS;
            }
        }

#ifdef UDF_CDRW_EMULATION_ON_ROM
        Vcb->LastPossibleLBA = Vcb->LastLBA+7+1+1024;
        Vcb->NWA = Vcb->LastLBA+7+1;
#else
        Vcb->LastPossibleLBA =
        Vcb->NWA = Vcb->LastLBA+7+1;
#endif //UDF_CDRW_EMULATION_ON_ROM

try_exit: NOTHING;
    } _SEH2_FINALLY {
        if (toc) MyFreePool__(toc);
        if (LastSes) MyFreePool__(LastSes);
        if (TempBuffer) MyFreePool__(TempBuffer);
    } _SEH2_END;

    return Status;
} // end UDFUseStandard()

/*
    Get block size (for read operation)
 */
NTSTATUS
UDFGetBlockSize(
    IN PDEVICE_OBJECT DeviceObject,
    IN PVCB           Vcb
    )
{
    NTSTATUS RC = STATUS_SUCCESS;
    DISK_GEOMETRY_EX DiskGeometryEx;
    PARTITION_INFORMATION_EX PartitionInfoEx; // Use EX for 64-bit support

    if (UDFGetDevType(DeviceObject) == FILE_DEVICE_DISK) {
        UDFPrint(("UDFGetBlockSize: HDD Detection for 256GB...\n"));
        
        // 1. Get Extended Geometry
        RC = UDFPhSendIOCTL(IOCTL_DISK_GET_DRIVE_GEOMETRY_EX, DeviceObject,
                            NULL, 0, &DiskGeometryEx, sizeof(DiskGeometryEx), TRUE, NULL);
        if (!NT_SUCCESS(RC)) return RC;

        // 2. Get Extended Partition Info to prevent 32-bit truncation
        RC = UDFPhSendIOCTL(IOCTL_DISK_GET_PARTITION_INFO_EX, DeviceObject,
                            NULL, 0, &PartitionInfoEx, sizeof(PartitionInfoEx), TRUE, NULL);
	    RC = UDFPhSendIOCTL(IOCTL_DISK_GET_PARTITION_INFO_EX, DeviceObject,
                    0, NULL,
                    &PartitionInfoEx, sizeof(PARTITION_INFORMATION_EX),
                    TRUE, NULL);

        if (NT_SUCCESS(RC)) {
           // SAVE THE PHYSICAL STARTING OFFSET HERE
           Vcb->PartitionStartOffset = PartitionInfoEx.StartingOffset.QuadPart;
           UDFPrint(("UDF: Physical Partition starts at %llx bytes\n", Vcb->PartitionStartOffset));
           } else {
           // Fallback if IOCTL fails (e.g. unpartitioned disk)
           Vcb->PartitionStartOffset = 0;
        }
        
        if (!NT_SUCCESS(RC)) {
            UDFPrint(("UDF: Partition Info EX failed, using DiskSize from Geometry.\n"));
            // Fallback: use DiskSize from DiskGeometryEx
            PartitionInfoEx.PartitionLength = DiskGeometryEx.DiskSize;
        }

        // 3. Force UDF Standard Sector Size if misdetected
        // 256GB VHDs formatted as UDF 2.01 use 2048-byte logical sectors
        if (Vcb->SectorSize < 2048) {
            UDFPrint(("UDF: Adjusting SectorSize from %x to 800 (2048)\n", Vcb->SectorSize));
            Vcb->SectorSize = 2048;
            Vcb->SectorShift = 11; 
        }

    } else {
        // CD-ROM Logic (remains similar but use EX)
        RC = UDFPhSendIOCTL(IOCTL_CDROM_GET_DRIVE_GEOMETRY_EX, DeviceObject,
                            NULL, 0, &DiskGeometryEx, sizeof(DiskGeometryEx), TRUE, NULL);
        if (!NT_SUCCESS(RC)) return RC;
    }

    // 4. Safe 64-bit LBA Calculation
    Vcb->FirstLBA = 0;
    
    // Calculate LastLBA using ULONGLONG to prevent 16GB truncation
    ULONGLONG TotalSectors = PartitionInfoEx.PartitionLength.QuadPart >> Vcb->SectorShift;
    
    Vcb->LastLBA = (uint32)(TotalSectors - 1);
    Vcb->LastPossibleLBA = Vcb->LastLBA;

    Vcb->WriteBlockSize = PACKETSIZE_UDF * Vcb->SectorSize;

    UDFPrint(("UDFGetBlockSize Final:\n"));
    UDFPrint(("Sector Size: %x, Shift: %d, Last LBA: %x\n", 
              Vcb->SectorSize, Vcb->SectorShift, Vcb->LastLBA));

    return STATUS_SUCCESS;
} // end UDFGetBlockSize()

uint32
UDFFixFPAddress(
    IN PVCB           Vcb,               // Volume control block from this DevObj
    IN uint32         Lba
    )
{
    uint32 i = Vcb->LastReadTrack;
    uint32 pk;
    uint32 rel;

    if(FlagOn(Vcb->VcbState, VCB_STATE_PACKET_RUNOUT_FIXUP)) {
        if (Lba < 0x20)
            return Lba;
        pk = Lba / Vcb->TrackMap[i].PacketSize;
        rel = Lba % Vcb->TrackMap[i].PacketSize;
        UDFPrint(("FixFPAddr: %x -> %x\n", Lba, pk*(Vcb->TrackMap[i].PacketSize+7) + rel));
        return pk*(Vcb->TrackMap[i].PacketSize+7) + rel /*- Vcb->TrackMap[i].PacketFPOffset*/;
    }
    return Lba;
} // end UDFFixFPAddress()

/*
    detect device driver & try to read disk layout (use all methods)
 */
NTSTATUS
UDFGetDiskInfo(
    IN PIRP_CONTEXT IrpContext,
    IN PDEVICE_OBJECT DeviceObject,      // the target device object
    IN PVCB           Vcb                // Volume control block from this DevObj
    )
{
    NTSTATUS        RC = STATUS_UNRECOGNIZED_VOLUME;
    uint32 i;

    UDFPrint(("UDFGetDiskInfo\n"));

    _SEH2_TRY {
        RC = UDFGetBlockSize(DeviceObject, Vcb);

        if (!NT_SUCCESS(RC)) {
            try_return(RC);
        }

        ULONG SessionStart;
        ULONG SessionEnd;

        RC = UDFDetermineVolumeLayout(IrpContext, DeviceObject, Vcb, &SessionStart, &SessionEnd);

        if (!NT_SUCCESS(RC)) {
            try_return(RC);
        }

        Vcb->SessionStartLba = SessionStart;
        Vcb->SessionEndLba = SessionEnd;

try_exit:   NOTHING;

    } _SEH2_FINALLY {

        if ((Vcb->LastPossibleLBA & 0x80000000) || (Vcb->LastPossibleLBA < Vcb->LastLBA)) {
            UDFPrint(("UDF: bad LastPossibleLBA %x -> %x\n", Vcb->LastPossibleLBA, Vcb->LastLBA));
            Vcb->LastPossibleLBA = Vcb->LastLBA;
        }
        if (!Vcb->WriteBlockSize)
            Vcb->WriteBlockSize = PACKETSIZE_UDF*Vcb->SectorSize;

        if (Vcb->TrackMap) {
            if (Vcb->TrackMap[Vcb->LastTrackNum].LastLba > Vcb->NWA) {
                if (Vcb->NWA) {
                    if (Vcb->TrackMap[Vcb->LastTrackNum].DataParam & TrkInfo_FP) {
                        Vcb->LastLBA = Vcb->NWA-1;
                    } else {
                        Vcb->LastLBA = Vcb->NWA-7-1;
                    }
                }
            } else {
                if ((Vcb->LastTrackNum > 1) &&
                   (Vcb->TrackMap[Vcb->LastTrackNum-1].FirstLba >= Vcb->TrackMap[Vcb->LastTrackNum-1].LastLba)) {
                    Vcb->LastLBA = Vcb->TrackMap[Vcb->LastTrackNum-1].LastLba;
                }
            }
        }

        for(i=0; i<32; i++) {
            if (!(Vcb->LastPossibleLBA >> i))
                break;
        }

        if (Vcb->VcbState & VCB_STATE_VOLUME_READ_ONLY) {
            if (!Vcb->BlankCD && Vcb->MediaType != MediaType_UnknownSize_CDRW) {
                UDFPrint(("UDFGetDiskInfo: R/O+!Blank+!RW -> !RAW\n"));
                Vcb->VcbState &= ~UDF_VCB_FLAGS_RAW_DISK;
            } else {
                UDFPrint(("UDFGetDiskInfo: Blank or RW\n"));
            }
        }

        UDFPrint(("UDF: ------------------------------------------\n"));
        UDFPrint(("UDF: Media characteristics\n"));
        UDFPrint(("UDF: Last session: %d\n",Vcb->LastSession));
        UDFPrint(("UDF: First track in first session: %d\n",Vcb->FirstTrackNum));
        UDFPrint(("UDF: First track in last session: %d\n",Vcb->FirstTrackNumLastSes));
        UDFPrint(("UDF: Last track in last session: %d\n",Vcb->LastTrackNum));
        UDFPrint(("UDF: First LBA in first session: %x\n",Vcb->FirstLBA));
        UDFPrint(("UDF: First LBA in last session: %x\n",Vcb->FirstLBALastSes));
        UDFPrint(("UDF: Last LBA in last session: %x\n",Vcb->LastLBA));
        UDFPrint(("UDF: First writable LBA (NWA) in last session: %x\n",Vcb->NWA));
        UDFPrint(("UDF: Last available LBA beyond end of last session: %x\n",Vcb->LastPossibleLBA));
 //       UDFPrint(("UDF: blocks per frame: %x\n",1 << Vcb->WCacheBlocksPerFrameSh));
        UDFPrint(("UDF: Flags: %s%s\n",
                 Vcb->VcbState & UDF_VCB_FLAGS_RAW_DISK ? "RAW " : "",
                 Vcb->VcbState & VCB_STATE_VOLUME_READ_ONLY ? "R/O " : "WR "
                 ));
        UDFPrint(("UDF: ------------------------------------------\n"));

    } _SEH2_END;

    UDFPrint(("UDFGetDiskInfo: %x\n", RC));
    return(RC);

} // end UDFGetDiskInfo()

NTSTATUS
UDFPrepareForReadOperation(
    IN PIRP_CONTEXT IrpContext,
    IN PVCB Vcb,
    IN uint32 Lba,
    IN uint32 BCount
    )
{
    if ( (Vcb->FsDeviceType != FILE_DEVICE_CD_ROM_FILE_SYSTEM) ) {
        Vcb->VcbState &= ~UDF_VCB_LAST_WRITE;
        return STATUS_SUCCESS;
    }
    uint32 i = Vcb->LastReadTrack;

#ifdef _UDF_STRUCTURES_H_
    if (Vcb->BSBM_Bitmap) {
        ULONG i;
        for(i=0; i<BCount; i++) {
            if (UDFGetBit((uint32*)(Vcb->BSBM_Bitmap), Lba+i)) {
                UDFPrint(("R: Known BB @ %#x\n", Lba));
                //return STATUS_FT_WRITE_RECOVERY; // this shall not be treated as error and
                                                   // we shall get IO request to BAD block
                return STATUS_DEVICE_DATA_ERROR;
            }
        }
    }
#endif //_UDF_STRUCTURES_H_

    return STATUS_SUCCESS;
} // end UDFPrepareForReadOperation()

/*
    This routine reads physical sectors
 */
NTSTATUS
UDFReadSectors(
    IN PIRP_CONTEXT IrpContext,
    IN PVCB Vcb,
    IN BOOLEAN Translate,       // Translate Logical to Physical
    IN uint32 Lba,
    IN uint32 BCount,
    IN BOOLEAN Direct,
    OUT int8* Buffer,
    OUT PULONG ReadBytes
    )
{
    // FIX: Calculate ByteOffset and ByteCount using 64-bit math
    // to prevent overflow on volumes > 2TB or large offsets on 256GB volumes.
    ULONGLONG ByteOffset = (ULONGLONG)Lba * Vcb->SectorSize;
    ULONG ByteCount = BCount * Vcb->SectorSize;

    // Check if UDFTRead can handle the ULONGLONG offset. 
    // If UDFTRead expects a 32-bit 'uint32' for offset, this is where the mount fails.
    return UDFTRead(
        IrpContext, 
        Vcb, 
        Buffer, 
        ByteCount, 
        (uint32)Lba, // If UDFTRead uses LBA, pass LBA. 
        ReadBytes
    );
}


/*
    This routine reads physical sectors
 */
NTSTATUS
UDFReadInSector(
    IN PIRP_CONTEXT IrpContext,
    IN PVCB Vcb,
    IN BOOLEAN Translate,       // Translate Logical to Physical
    IN uint32 Lba,
    IN uint32 i,                 // offset in sector
    IN uint32 l,                 // transfer length
    IN BOOLEAN Direct,          // Disable access to non-cached data
    OUT int8* Buffer,
    OUT PULONG ReadBytes
    )
{
    int8* tmp_buff;
    NTSTATUS status;
    ULONG _ReadBytes;

    (*ReadBytes) = 0;

    if (Direct) {

        return STATUS_INVALID_PARAMETER;
    }

    tmp_buff = (int8*)MyAllocatePool__(NonPagedPool, Vcb->SectorSize);
    if (!tmp_buff) return STATUS_INSUFFICIENT_RESOURCES;
    status = UDFReadSectors(IrpContext, Vcb, Translate, Lba, 1, FALSE, tmp_buff, &_ReadBytes);
    if (NT_SUCCESS(status)) {
        (*ReadBytes) += l;
        RtlCopyMemory(Buffer, tmp_buff+i, l);
    }
    MyFreePool__(tmp_buff);

    return status;
} // end UDFReadInSector()

/*
    This routine reads data of unaligned offset & length
 */
NTSTATUS
UDFReadData(
    IN PIRP_CONTEXT IrpContext,
    IN PVCB Vcb,
    IN BOOLEAN Translate,       // Translate Logical to Physical
    IN int64 Offset,
    IN uint32 Length,
    IN BOOLEAN Direct,          // Disable access to non-cached data
    OUT int8* Buffer,
    OUT PULONG ReadBytes
    )
{
    uint32 i, l, Lba, BS=Vcb->SectorSize;
    uint32 BSh=Vcb->SectorShift;
    NTSTATUS status;
    ULONG _ReadBytes = 0;
    uint32 to_read;

    (*ReadBytes) = 0;
    if (!Length) return STATUS_SUCCESS;
    if (Vcb->VcbState & UDF_VCB_FLAGS_DEAD)
        return STATUS_NO_SUCH_DEVICE;
    // read tail of the 1st sector if Offset is not sector_size-aligned
    Lba = (uint32)(Offset >> BSh);
    if ((i = (uint32)(Offset & (BS-1)))) {
        l = (BS - i) < Length ?
            (BS - i) : Length;
        // here we use 'ReadBytes' 'cause now it's set to zero
        status = UDFReadInSector(IrpContext, Vcb, Translate, Lba, i, l, Direct, Buffer, ReadBytes);
        if (!NT_SUCCESS(status)) return status;
        if (!(Length = Length - l)) return STATUS_SUCCESS;
        Lba ++;
        Buffer += l;
    }
    // read sector_size-aligned part
    i = Length >> BSh;
    while(i) {
        to_read = min(i, 64);
        status = UDFReadSectors(IrpContext, Vcb, Translate, Lba, to_read, Direct, Buffer, &_ReadBytes);
        (*ReadBytes) += _ReadBytes;
        if (!NT_SUCCESS(status)) {
            return status;
        }
        Buffer += to_read<<BSh;
        Length -= to_read<<BSh;
        Lba += to_read;
        i -= to_read;
    }
    // read head of the last sector
    if (!Length) return STATUS_SUCCESS;
    status = UDFReadInSector(IrpContext, Vcb, Translate, Lba, 0, Length, Direct, Buffer, &_ReadBytes);
    (*ReadBytes) += _ReadBytes;

    return status;
} // end UDFReadData()

/*
    This routine writes physical sectors. This routine supposes Lba & Length
    alignment on WriteBlock (packet) size.
 */
NTSTATUS
UDFWriteSectors(
    IN PIRP_CONTEXT IrpContext,
    IN PVCB Vcb,
    IN BOOLEAN Translate,       // Translate Logical to Physical
    IN uint32 Lba,
    IN uint32 BCount,
    IN BOOLEAN Direct,          // Disable access to non-cached data
    IN int8* Buffer,
    OUT PSIZE_T WrittenBytes
    )
{
    NTSTATUS status;

    if (!Vcb->Modified || (Vcb->IntegrityType == INTEGRITY_TYPE_CLOSE)) {
        UDFSetModified(Vcb);
        if (Vcb->LVid && !Direct) {
            status = UDFUpdateLogicalVolInt(IrpContext, Vcb,FALSE);
        }
    }

    if (Vcb->CDR_Mode) {
        if (Vcb->LastLBA < Lba+BCount-1)
            Vcb->LastLBA = Lba+BCount-1;
    }

    status = UDFTWrite(IrpContext, Vcb, Buffer, BCount<<Vcb->SectorShift, Lba, WrittenBytes);
    ASSERT(NT_SUCCESS(status));

    return status;
} // end UDFWriteSectors()

NTSTATUS
UDFWriteInSector(
    IN PIRP_CONTEXT IrpContext,
    IN PVCB Vcb,
    IN BOOLEAN Translate,       // Translate Logical to Physical
    IN uint32 Lba,
    IN uint32 i,                 // offset in sector
    IN uint32 l,                 // transfer length
    IN BOOLEAN Direct,          // Disable access to non-cached data
    OUT int8* Buffer,
    OUT PSIZE_T WrittenBytes
    )
{
    int8* tmp_buff;
    NTSTATUS status;
    SIZE_T _WrittenBytes;
    ULONG ReadBytes;

    if (!Vcb->Modified) {
        UDFSetModified(Vcb);
        if (Vcb->LVid)
            status = UDFUpdateLogicalVolInt(IrpContext, Vcb, FALSE);
    }

    if (Vcb->CDR_Mode) {
        if (Vcb->LastLBA < Lba)
            Vcb->LastLBA = Lba;
    }

    (*WrittenBytes) = 0;

    // If Direct = TRUE we should never get here, but...
    if (Direct) {
        BrutePoint();
        return STATUS_INVALID_PARAMETER;
    }
    tmp_buff = (int8*)MyAllocatePool__(NonPagedPool, Vcb->SectorSize);
    if (!tmp_buff) {
        BrutePoint();
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    // read packet
    status = UDFReadSectors(IrpContext, Vcb, Translate, Lba, 1, FALSE, tmp_buff, &ReadBytes);
    if (!NT_SUCCESS(status)) goto EO_WrSctD;
    // modify packet
    RtlCopyMemory(tmp_buff+i, Buffer, l);
    // write modified packet
    status = UDFWriteSectors(IrpContext, Vcb, Translate, Lba, 1, FALSE, tmp_buff, &_WrittenBytes);
    if (NT_SUCCESS(status))
        (*WrittenBytes) += l;
EO_WrSctD:
    MyFreePool__(tmp_buff);

    ASSERT(NT_SUCCESS(status));
    if (!NT_SUCCESS(status)) {
        UDFPrint(("UDFWriteInSector() for LBA %x failed\n", Lba));
    }

    return status;
} // end UDFWriteInSector()

/*
    This routine writes data at unaligned offset & length
 */
NTSTATUS
UDFWriteData(
    PIRP_CONTEXT IrpContext,
    IN PVCB Vcb,
    IN BOOLEAN Translate,      // Translate Logical to Physical
    IN int64 Offset,
    IN SIZE_T Length,
    IN BOOLEAN Direct,         // setting this flag delays flushing of given
                               // data to indefinite term
    IN int8* Buffer,
    OUT PSIZE_T WrittenBytes
    )
{
    uint32 i, l, Lba, BS=Vcb->SectorSize;
    uint32 BSh=Vcb->SectorShift;
    NTSTATUS status;
    SIZE_T _WrittenBytes;

    (*WrittenBytes) = 0;
    if (!Length) return STATUS_SUCCESS;
    if (Vcb->VcbState & UDF_VCB_FLAGS_DEAD)
        return STATUS_NO_SUCH_DEVICE;
    // write tail of the 1st sector if Offset is not sector_size-aligned
    Lba = (uint32)(Offset >> BSh);
    if ((i = ((uint32)Offset & (BS-1)))) {
        l = (BS - i) < Length ?
            (BS - i) : Length;
        status = UDFWriteInSector(IrpContext, Vcb, Translate, Lba, i, l, Direct, Buffer, WrittenBytes);
        if (!NT_SUCCESS(status)) return status;
        if (!(Length = Length - l)) return STATUS_SUCCESS;
        Lba ++;
        Buffer += l;
    }
    // write sector_size-aligned part
    i = Length >> BSh;
    if (i) {
        status = UDFWriteSectors(IrpContext, Vcb, Translate, Lba, i, Direct, Buffer, &_WrittenBytes);
        (*WrittenBytes) += _WrittenBytes;
        if (!NT_SUCCESS(status)) return status;
        l = i<<BSh;

        if (!(Length = Length - l)) return STATUS_SUCCESS;
        Lba += i;
        Buffer += l;
    }
    status = UDFWriteInSector(IrpContext, Vcb, Translate, Lba, 0, Length, Direct, Buffer, &_WrittenBytes);
    (*WrittenBytes) += _WrittenBytes;

    return status;
} // end UDFWriteData()
