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

#define MRW_DMA_OFFSET           0x500
#define MRW_DA_SIZE              (136*32)
#define MRW_SA_SIZE              (8*32)
#define MRW_DMA_SEGMENT_SIZE     (MRW_DA_SIZE+MRW_SA_SIZE)

// Local functions:

NTSTATUS
UDFSetCaching(
    IN PVCB Vcb
    );

OSSTATUS
UDFRecoverFromError(
    IN PVCB Vcb,
    IN BOOLEAN WriteOp,
    IN OSSTATUS status,
    IN uint32 Lba,
    IN uint32 BCount,
 IN OUT uint32* retry);

#ifdef _BROWSE_UDF_

uint32
UDFFixFPAddress(
    IN PVCB           Vcb,               // Volume control block from this DevObj
    IN uint32         Lba
    );

#endif //_BROWSE_UDF_

OSSTATUS
UDFReallocTrackMap(
    IN PVCB Vcb,
    IN uint32 TrackNum
    )
{
#ifdef _BROWSE_UDF_
    if(Vcb->TrackMap) {
        MyFreePool__(Vcb->TrackMap);
        Vcb->TrackMap = NULL;
    }
    Vcb->TrackMap = (PUDFTrackMap)
        MyAllocatePool__(NonPagedPool, TrackNum*sizeof(UDFTrackMap));
    if(!Vcb->TrackMap) {
        return STATUS_INSUFFICIENT_RESOURCES;
    }
#endif //_BROWSE_UDF_
    RtlZeroMemory(Vcb->TrackMap,TrackNum*sizeof(UDFTrackMap));
    return STATUS_SUCCESS;
} // end UDFReallocTrackMap()

#ifdef _BROWSE_UDF_


OSSTATUS
__fastcall
UDFTIOVerify(
    IN void* _Vcb,
    IN void* Buffer,     // Target buffer
    IN SIZE_T Length,
    IN uint32 LBA,
    OUT PSIZE_T IOBytes,
    IN uint32 Flags
    )
{
    OSSTATUS RC = STATUS_SUCCESS;
    uint32 i, j;
    SIZE_T mask;
    uint32 lba0, len, lba1;
    PUCHAR tmp_buff;
    PUCHAR p;
    PCHAR cached_block;
    SIZE_T tmp_wb;
    BOOLEAN need_remap;
    OSSTATUS final_RC = STATUS_SUCCESS;
    BOOLEAN zero;
    BOOLEAN non_zero;
    BOOLEAN packet_ok;
    BOOLEAN free_tmp = FALSE;
    BOOLEAN single_packet = FALSE;

#define Vcb ((PVCB)_Vcb)
    // ATTENTION! Do not touch bad block bitmap here, since it describes PHYSICAL addresses WITHOUT remapping,
    // while here we work with LOGICAL addresses

    if(Vcb->VerifyCtx.ItemCount > UDF_MAX_VERIFY_CACHE) {
        UDFVVerify(Vcb, 0/*UFD_VERIFY_FLAG_WAIT*/);
    }

    UDFAcquireResourceExclusive(&(Vcb->IoResource), TRUE);
    Flags |= PH_IO_LOCKED;

    tmp_wb = (SIZE_T)_Vcb;
    if(Flags & PH_EX_WRITE) {
        UDFPrint(("IO-Write-Verify\n"));
        RC = UDFTWrite(_Vcb, Buffer, Length, LBA, &tmp_wb, Flags | PH_VCB_IN_RETLEN);
    } else {
        UDFPrint(("IO-Read-Verify\n"));
        RC = UDFTRead(_Vcb, Buffer, Length, LBA, &tmp_wb, Flags | PH_VCB_IN_RETLEN);
    }
    (*IOBytes) = tmp_wb;

    switch(RC) {
    default:
        UDFReleaseResource(&(Vcb->IoResource));
        return RC;
    case STATUS_FT_WRITE_RECOVERY:
    case STATUS_DEVICE_DATA_ERROR:
    case STATUS_IO_DEVICE_ERROR:
        break;
        /* FALL THROUGH */
    } // end switch(RC)

    if(!Vcb->SparingCount ||
       !Vcb->SparingCountFree ||
       Vcb->CDR_Mode) {
        UDFPrint(("Can't remap\n"));
        UDFReleaseResource(&(Vcb->IoResource));
        return RC;
    }

    if(Flags & PH_EX_WRITE) {
        UDFPrint(("Write failed, try relocation\n"));
    } else {
        if(Vcb->Modified) {
            UDFPrint(("Read failed, try relocation\n"));
        } else {
            UDFPrint(("no remap on not modified volume\n"));
            UDFReleaseResource(&(Vcb->IoResource));
            return RC;
        }
    }
    if(Flags & PH_LOCK_CACHE) {
        UDFReleaseResource(&(Vcb->IoResource));
        WCacheStartDirect__(&(Vcb->FastCache), Vcb, TRUE);
        UDFAcquireResourceExclusive(&(Vcb->IoResource), TRUE);
    }

    Flags &= ~PH_KEEP_VERIFY_CACHE;

    // NOTE: SparingBlockSize may be not equal to PacketSize
    // perform recovery
    mask = Vcb->SparingBlockSize-1;
    lba0 = LBA & ~mask;
    len = ((LBA+(Length>>Vcb->BlockSizeBits)+mask) & ~mask) - lba0;
    j=0;
    if((lba0 == LBA) && (len == mask+1) && (len == (Length>>Vcb->BlockSizeBits))) {
        single_packet = TRUE;
        tmp_buff = NULL;
    } else {
        tmp_buff = (PUCHAR)DbgAllocatePoolWithTag(NonPagedPool, Vcb->SparingBlockSize << Vcb->BlockSizeBits, 'bNWD');
        if(!tmp_buff) {
            UDFPrint(("  can't alloc tmp\n"));
            UDFReleaseResource(&(Vcb->IoResource));
            return STATUS_DEVICE_DATA_ERROR;
        }
        free_tmp = TRUE;
    }

    for(i=0; i<len; i++) {
        if(!Vcb->SparingCountFree) {
            UDFPrint(("  no more free spare blocks, abort verification\n"));
            break;
        }
        UDFPrint(("  read LBA %x (%x)\n", lba0+i, j));
        if(!j) {
            need_remap = FALSE;
            lba1 = lba0+i;
            non_zero = FALSE;
            if(single_packet) {
                // single packet requested
                tmp_buff = (PUCHAR)Buffer;
                if(Flags & PH_EX_WRITE) {
                    UDFPrint(("  remap single write\n"));
                    UDFPrint(("  try del from verify cache @ %x, %x\n", lba0, len));
                    UDFVForget(Vcb, len, UDFRelocateSector(Vcb, lba0), 0);
                    goto do_remap;
                } else {
                    UDFPrint(("  recover and remap single read\n"));
                }
            }
        }
        p = tmp_buff+(j<<Vcb->BlockSizeBits);
        // not cached, try to read
        // prepare for error, if block cannot be read, assume it is zero-filled
        RtlZeroMemory(p, Vcb->BlockSize);

        // check if block valid
        if(Vcb->BSBM_Bitmap) {
            if(UDFGetBit((uint32*)(Vcb->BSBM_Bitmap), UDFRelocateSector(Vcb, lba0+i))) {
                UDFPrint(("  remap: known BB @ %x, mapped to %x\n", lba0+i, UDFRelocateSector(Vcb, lba0+i)));
                need_remap = TRUE;
            }
        }
        zero = FALSE;
        if(Vcb->FSBM_Bitmap) {
            if(UDFGetFreeBit((uint32*)(Vcb->FSBM_Bitmap), lba0+i)) {
                UDFPrint(("  unused @ %x\n", lba0+i));
                zero = TRUE;
            }
        }
        if(!zero && Vcb->ZSBM_Bitmap) {
            if(UDFGetZeroBit((uint32*)(Vcb->ZSBM_Bitmap), lba0+i)) {
                UDFPrint(("  unused @ %x (Z)\n", lba0+i));
                zero = TRUE;
            }
        }
        non_zero |= !zero;

        if(!j) {
            packet_ok = FALSE;
            if(!single_packet) {
                // try to read entire packet, this returs error more often then sequential reading of all blocks one by one
                tmp_wb = (SIZE_T)_Vcb;
                RC = UDFTRead(_Vcb, p, Vcb->SparingBlockSize << Vcb->BlockSizeBits, lba0+i, &tmp_wb,
                              Flags | PH_READ_VERIFY_CACHE | PH_TMP_BUFFER | PH_VCB_IN_RETLEN);
            } else {
                // Note: we get here ONLY if original request failed
                // do not retry if it was single-packet request
                RC = STATUS_UNSUCCESSFUL;
            }
            if(RC == STATUS_SUCCESS) {
                UDFPrint(("  packet ok @ %x\n", lba0+i));
                packet_ok = TRUE;
                i += Vcb->SparingBlockSize-1;
                continue;
            } else {
                need_remap = TRUE;
            }
        }

        if(!zero) {
            if(WCacheIsCached__(&(Vcb->FastCache), lba0+i, 1)) {
                // even if block is cached, we have to verify if it is readable
                if(!packet_ok && !UDFVIsStored(Vcb, lba0+i)) {

                    tmp_wb = (SIZE_T)_Vcb;
                    RC = UDFTRead(_Vcb, p, Vcb->BlockSize, lba0+i, &tmp_wb,
                                  Flags | PH_FORGET_VERIFIED | PH_READ_VERIFY_CACHE | PH_TMP_BUFFER | PH_VCB_IN_RETLEN);
                    if(!OS_SUCCESS(RC)) {
                        UDFPrint(("  Found BB @ %x\n", lba0+i));
                    }

                }
                RC = WCacheDirect__(&(Vcb->FastCache), _Vcb, lba0+i, FALSE, &cached_block, TRUE/* cached only */);
            } else {
                cached_block = NULL;
                if(!packet_ok) {
                    RC = STATUS_UNSUCCESSFUL;
                } else {
                    RC = STATUS_SUCCESS;
                }
            }
            if(OS_SUCCESS(RC)) {
                // cached or successfully read
                if(cached_block) {
                    // we can get from cache the most fresh data
                    RtlCopyMemory(p, cached_block, Vcb->BlockSize);
                }

            } else {
                if(!UDFVIsStored(Vcb, lba0+i)) {
                    tmp_wb = (SIZE_T)_Vcb;
                    RC = UDFTRead(_Vcb, p, Vcb->BlockSize, lba0+i, &tmp_wb,
                                  Flags | PH_FORGET_VERIFIED | PH_READ_VERIFY_CACHE | PH_TMP_BUFFER | PH_VCB_IN_RETLEN);
                } else {
                    // get it from verify-cache
                    RC = STATUS_UNSUCCESSFUL;
                }
                if(!OS_SUCCESS(RC)) {
/*
                    UDFPrint(("  retry @ %x\n", lba0+i));
                    tmp_wb = (uint32)_Vcb;
                    RC = UDFTRead(_Vcb, p, Vcb->BlockSize, lba0+i, &tmp_wb,
                                  Flags | PH_FORGET_VERIFIED | PH_READ_VERIFY_CACHE | PH_TMP_BUFFER | PH_VCB_IN_RETLEN);
*/
                    UDFPrint(("  try get from verify cache @ %x\n", lba0+i));
                    RC = UDFVRead(Vcb, p, 1, UDFRelocateSector(Vcb, lba0+i),
                                  Flags | PH_FORGET_VERIFIED | PH_READ_VERIFY_CACHE | PH_TMP_BUFFER);
                    need_remap = TRUE;
                }
            }
        } else {
            RtlZeroMemory(p, Vcb->BlockSize);
        }
        if(!packet_ok) {
            UDFPrint(("  try del from verify cache @ %x\n", lba0+i));
            RC = UDFVForget(Vcb, 1, UDFRelocateSector(Vcb, lba0+i), 0);
        }

        if(!packet_ok || need_remap) {
            UDFPrint(("  block in bad packet @ %x\n", lba0+i));
            if(Vcb->BSBM_Bitmap) {
                UDFSetBit(Vcb->BSBM_Bitmap, lba0+i);
            }
            if(Vcb->FSBM_Bitmap) {
                UDFSetUsedBit(Vcb->FSBM_Bitmap, lba0+i);
            }
        }

        j++;
        if(j >= Vcb->SparingBlockSize) {
            // remap this packet
            if(need_remap) {
                ASSERT(!packet_ok);
                if(!non_zero) {
                    UDFPrint(("  forget Z packet @ %x\n", lba1));
                    UDFUnmapRange(Vcb, lba1, Vcb->SparingBlockSize);
                    RC = STATUS_SUCCESS;
                } else {
do_remap:
                    for(j=0; j<3; j++) {
                        UDFPrint(("  remap packet @ %x\n", lba1));
                        RC = UDFRemapPacket(Vcb, lba1, FALSE);
                        if(!OS_SUCCESS(RC)) {
                            if(RC == STATUS_SHARING_VIOLATION) {
                                UDFPrint(("  remap2\n"));
                                // remapped location have died
                                RC = UDFRemapPacket(Vcb, lba1, TRUE);
                            }
                            if(!OS_SUCCESS(RC)) {
                                // packet cannot be remapped :(
                                RC = STATUS_DEVICE_DATA_ERROR;
                            }
                        }
                        UDFPrint(("  remap status %x\n", RC));
                        if(OS_SUCCESS(RC)) {
                            // write to remapped area
                            tmp_wb = (SIZE_T)_Vcb;
                            RC = UDFTWrite(_Vcb, tmp_buff, Vcb->SparingBlockSize << Vcb->BlockSizeBits, lba1, &tmp_wb,
                                          Flags | PH_FORGET_VERIFIED | PH_READ_VERIFY_CACHE | PH_TMP_BUFFER | PH_VCB_IN_RETLEN);
                            UDFPrint(("  write status %x\n", RC));
                            if(RC != STATUS_SUCCESS) {
                                // will be remapped
                                UDFPrint(("  retry remap\n"));

                                // Note: when remap of already remapped block is requested, verify of
                                // entire sparing are will be performed.

                            } else {
                                UDFPrint(("  remap OK\n"));
                                break;
                            }
                        } else {
                            UDFPrint(("  failed remap\n"));
                            break;
                        }
                    } // for
                }
                if(!OS_SUCCESS(RC) && !OS_SUCCESS(final_RC)) {
                    final_RC = RC;
                }
            } else {
                UDFPrint(("  NO remap for @ %x\n", (lba0+i) & ~mask));
            }
            j=0;
        }
    }
    if(free_tmp) {
        DbgFreePool(tmp_buff);
    }

    tmp_wb = (SIZE_T)_Vcb;
    if(Flags & PH_EX_WRITE) {
        UDFPrint(("IO-Write-Verify (2)\n"));
        //RC = UDFTWrite(_Vcb, Buffer, Length, LBA, &tmp_wb, Flags | PH_FORGET_VERIFIED | PH_VCB_IN_RETLEN);
    } else {
        UDFPrint(("IO-Read-Verify (2)\n"));
        RC = UDFTRead(_Vcb, Buffer, Length, LBA, &tmp_wb, Flags | PH_FORGET_VERIFIED | PH_VCB_IN_RETLEN);
    }
    (*IOBytes) = tmp_wb;
    UDFPrint(("Final %x\n", RC));

    UDFReleaseResource(&(Vcb->IoResource));
    if(Flags & PH_LOCK_CACHE) {
        WCacheEODirect__(&(Vcb->FastCache), Vcb);
    }

    return RC;
} // end UDFTIOVerify()

OSSTATUS
UDFTWriteVerify(
    IN void* _Vcb,
    IN void* Buffer,     // Target buffer
    IN SIZE_T Length,
    IN uint32 LBA,
    OUT PSIZE_T WrittenBytes,
    IN uint32 Flags
    )
{
    return UDFTIOVerify(_Vcb, Buffer, Length, LBA, WrittenBytes, Flags | PH_VCB_IN_RETLEN | PH_EX_WRITE | PH_KEEP_VERIFY_CACHE);
} // end UDFTWriteVerify()

OSSTATUS
UDFTReadVerify(
    IN void* _Vcb,
    IN void* Buffer,     // Target buffer
    IN SIZE_T Length,
    IN uint32 LBA,
    OUT PSIZE_T ReadBytes,
    IN uint32 Flags
    )
{
    return UDFTIOVerify(_Vcb, Buffer, Length, LBA, ReadBytes, Flags | PH_VCB_IN_RETLEN | PH_KEEP_VERIFY_CACHE);
} // end UDFTReadVerify()
#endif //_BROWSE_UDF_

/*
    This routine performs low-level write

    ATTENTION! When we are in Variable-Packet mode (CDR_Mode = TRUE)
    LBA is ignored and assumed to be equal to NWA by CD-R(W) driver
 */
OSSTATUS
UDFTWrite(
    IN void* _Vcb,
    IN void* Buffer,     // Target buffer
    IN SIZE_T Length,
    IN uint32 LBA,
    OUT PSIZE_T WrittenBytes,
    IN uint32 Flags
    )
{
#ifndef UDF_READ_ONLY_BUILD
#define Vcb ((PVCB)_Vcb)

#ifdef _BROWSE_UDF_
    PEXTENT_MAP RelocExtent;
    PEXTENT_MAP RelocExtent_saved = NULL;
#endif //_BROWSE_UDF_
    uint32 retry;
    BOOLEAN res_acq = FALSE;

    OSSTATUS RC = STATUS_SUCCESS;
    uint32 rLba;
    uint32 BCount;
    uint32 i;

#ifdef DBG
    //ASSERT(!(LBA & (32-1)));
#endif //DBG

    (*WrittenBytes) = 0;
    BCount = Length>>Vcb->BlockSizeBits;

    UDFPrint(("TWrite %x (%x)\n", LBA, BCount));
#ifdef _BROWSE_UDF_
    if(Vcb->VCBFlags & UDF_VCB_FLAGS_DEAD) {
        UDFPrint(("DEAD\n"));
        return STATUS_NO_SUCH_DEVICE;
    }

    Vcb->VCBFlags |= (UDF_VCB_SKIP_EJECT_CHECK | UDF_VCB_LAST_WRITE);
    if(!Vcb->CDR_Mode) {
        RelocExtent = UDFRelocateSectors(Vcb, LBA, BCount);
        if(!RelocExtent) {
            UDFPrint(("can't relocate\n"));
            return STATUS_INSUFFICIENT_RESOURCES;
        }
        rLba = LBA;
    } else {
        RelocExtent = UDF_NO_EXTENT_MAP;
        rLba = Vcb->NWA;
    }
#else //_BROWSE_UDF_
    rLba = LBA;
#endif //_BROWSE_UDF_

#ifdef DBG
    //ASSERT(!(rLba & (32-1)));
#endif //DBG

    _SEH2_TRY {
#ifdef _BROWSE_UDF_

        if(!(Flags & PH_IO_LOCKED)) {
            UDFAcquireResourceExclusive(&(Vcb->IoResource), TRUE);
            res_acq = TRUE;
        }

        if(RelocExtent == UDF_NO_EXTENT_MAP) {
#endif //_BROWSE_UDF_
            retry = UDF_WRITE_MAX_RETRY;
retry_1:
            RC = UDFPrepareForWriteOperation(Vcb, rLba, BCount);
            if(!OS_SUCCESS(RC)) {
                UDFPrint(("prepare failed\n"));
                try_return(RC);
            }
            if(Flags & PH_VCB_IN_RETLEN) {
                (*WrittenBytes) = (ULONG_PTR)Vcb;
            }
            RC = UDFPhWriteVerifySynchronous(Vcb->TargetDeviceObject, Buffer, Length,
                       ((uint64)rLba) << Vcb->BlockSizeBits, WrittenBytes, Flags);
#ifdef _BROWSE_UDF_
            Vcb->VCBFlags |= UDF_VCB_SKIP_EJECT_CHECK;
#endif //_BROWSE_UDF_
            if(!OS_SUCCESS(RC) &&
                OS_SUCCESS(RC = UDFRecoverFromError(Vcb, TRUE, RC, rLba, BCount, &retry)) )
                goto retry_1;
            try_return(RC);
#ifdef _BROWSE_UDF_
        }
        // write according to relocation table
        RelocExtent_saved = RelocExtent;
        for(i=0; RelocExtent->extLength; i++, RelocExtent++) {
            SIZE_T _WrittenBytes;
            rLba = RelocExtent->extLocation;
            BCount = RelocExtent->extLength>>Vcb->BlockSizeBits;
            retry = UDF_WRITE_MAX_RETRY;
retry_2:
            RC = UDFPrepareForWriteOperation(Vcb, rLba, BCount);
            if(!OS_SUCCESS(RC)) {
                UDFPrint(("prepare failed (2)\n"));
                break;
            }
            if(Flags & PH_VCB_IN_RETLEN) {
                _WrittenBytes = (ULONG_PTR)Vcb;
            }
            RC = UDFPhWriteVerifySynchronous(Vcb->TargetDeviceObject, Buffer, RelocExtent->extLength,
                       ((uint64)rLba) << Vcb->BlockSizeBits, &_WrittenBytes, Flags);
            Vcb->VCBFlags |= UDF_VCB_SKIP_EJECT_CHECK;
            if(!OS_SUCCESS(RC) &&
                OS_SUCCESS(RC = UDFRecoverFromError(Vcb, TRUE, RC, rLba, BCount, &retry)) )
                goto retry_2;
            LBA += BCount;
            (*WrittenBytes) += _WrittenBytes;
            if(!OS_SUCCESS(RC)) break;
            *((uint32*)&Buffer) += RelocExtent->extLength;
        }
#endif //_BROWSE_UDF_
try_exit: NOTHING;
    } _SEH2_FINALLY {
        if(res_acq) {
            UDFReleaseResource(&(Vcb->IoResource));
        }
#ifdef _BROWSE_UDF_
        if(RelocExtent_saved) {
            MyFreePool__(RelocExtent_saved);
        }
#endif //_BROWSE_UDF_
    } _SEH2_END;
    UDFPrint(("TWrite: %x\n", RC));
    return RC;

#undef Vcb
#else //UDF_READ_ONLY_BUILD
    return STATUS_ACCESS_DENIED;
#endif //UDF_READ_ONLY_BUILD
} // end UDFTWrite()

/*
    This routine performs low-level read
 */
OSSTATUS
UDFTRead(
    IN void* _Vcb,
    IN void* Buffer,     // Target buffer
    IN SIZE_T Length,
    IN uint32 LBA,
    OUT PSIZE_T ReadBytes,
    IN uint32 Flags
    )
{
    uint32 rLba;
    OSSTATUS RC = STATUS_SUCCESS;
    uint32 retry;
    PVCB Vcb = (PVCB)_Vcb;
    uint32 BCount = Length >> Vcb->BlockSizeBits;
    uint32 i;
#ifdef _BROWSE_UDF_
    PEXTENT_MAP RelocExtent;
    PEXTENT_MAP RelocExtent_saved = NULL;
    BOOLEAN res_acq = FALSE;
//    LARGE_INTEGER delay;
    Vcb->VCBFlags |= UDF_VCB_SKIP_EJECT_CHECK;

    ASSERT(Buffer);

    (*ReadBytes) = 0;

    if(Vcb->VCBFlags & UDF_VCB_FLAGS_DEAD)
        return STATUS_NO_SUCH_DEVICE;

    RelocExtent = UDFRelocateSectors(Vcb, LBA, BCount);
    if(!RelocExtent) return STATUS_INSUFFICIENT_RESOURCES;

    _SEH2_TRY {

        if(!(Flags & PH_IO_LOCKED)) {
            UDFAcquireResourceExclusive(&(Vcb->IoResource), TRUE);
            res_acq = TRUE;
        }

        if(RelocExtent == UDF_NO_EXTENT_MAP) {
            rLba = LBA;
            if(rLba >= (Vcb->CDR_Mode ? Vcb->NWA : Vcb->LastLBA + 1)) {
                RtlZeroMemory(Buffer, Length);
                try_return(RC = STATUS_SUCCESS);
            }
            retry = UDF_WRITE_MAX_RETRY;
retry_1:
            RC = UDFPrepareForReadOperation(Vcb, rLba, Length >> Vcb->BlockSizeBits);
            if(!OS_SUCCESS(RC)) try_return(RC);
            rLba = UDFFixFPAddress(Vcb, rLba);
#else
            rLba = LBA;
            retry = UDF_WRITE_MAX_RETRY;
retry_1:
            RC = UDFPrepareForReadOperation(Vcb, rLba, Length >> Vcb->BlockSizeBits);
            if(!OS_SUCCESS(RC)) return RC; // this is for !_BROWSE_UDF only
#endif //_BROWSE_UDF_
            if(Flags & PH_VCB_IN_RETLEN) {
                (*ReadBytes) = (SIZE_T)Vcb;
            }
            RC = UDFPhReadSynchronous(Vcb->TargetDeviceObject, Buffer, Length,
                       ((uint64)rLba) << Vcb->BlockSizeBits, ReadBytes, Flags);
            Vcb->VCBFlags &= ~UDF_VCB_LAST_WRITE;
#ifdef _BROWSE_UDF_
            Vcb->VCBFlags |= UDF_VCB_SKIP_EJECT_CHECK;
#endif //_BROWSE_UDF_
            if(!OS_SUCCESS(RC) &&
                OS_SUCCESS(RC = UDFRecoverFromError(Vcb, FALSE, RC, rLba, BCount, &retry)) ) {
                if(RC != STATUS_BUFFER_ALL_ZEROS) {
                    goto retry_1;
                }
                RtlZeroMemory(Buffer, Length);
                (*ReadBytes) = Length;
                RC = STATUS_SUCCESS;
            }
#ifdef _BROWSE_UDF_
            try_return(RC);
        }
        // read according to relocation table
        RelocExtent_saved = RelocExtent;
        for(i=0; RelocExtent->extLength; i++, RelocExtent++) {
            SIZE_T _ReadBytes;
            rLba = RelocExtent->extLocation;
            if(rLba >= (Vcb->CDR_Mode ? Vcb->NWA : Vcb->LastLBA + 1)) {
                RtlZeroMemory(Buffer, _ReadBytes = RelocExtent->extLength);
                RC = STATUS_SUCCESS;
                goto TR_continue;
            }
            BCount = RelocExtent->extLength>>Vcb->BlockSizeBits;
            retry = UDF_WRITE_MAX_RETRY;
retry_2:
            RC = UDFPrepareForReadOperation(Vcb, rLba, RelocExtent->extLength >> Vcb->BlockSizeBits);
            if(!OS_SUCCESS(RC)) break;
            rLba = UDFFixFPAddress(Vcb, rLba);
            if(Flags & PH_VCB_IN_RETLEN) {
                _ReadBytes = (SIZE_T)Vcb;
            }
            RC = UDFPhReadSynchronous(Vcb->TargetDeviceObject, Buffer, RelocExtent->extLength,
                       ((uint64)rLba) << Vcb->BlockSizeBits, &_ReadBytes, Flags);
            Vcb->VCBFlags &= ~UDF_VCB_LAST_WRITE;
            Vcb->VCBFlags |= UDF_VCB_SKIP_EJECT_CHECK;
            if(!OS_SUCCESS(RC) &&
                OS_SUCCESS(RC = UDFRecoverFromError(Vcb, FALSE, RC, rLba, BCount, &retry)) ) {
                if(RC != STATUS_BUFFER_ALL_ZEROS) {
                    goto retry_2;
                }
                RtlZeroMemory(Buffer, RelocExtent->extLength);
                _ReadBytes = RelocExtent->extLength;
                RC = STATUS_SUCCESS;
            }
TR_continue:
            (*ReadBytes) += _ReadBytes;
            if(!OS_SUCCESS(RC)) break;
            *((uint32*)&Buffer) += RelocExtent->extLength;
        }
try_exit: NOTHING;
    } _SEH2_FINALLY {
        if(res_acq) {
            UDFReleaseResource(&(Vcb->IoResource));
        }
        if(RelocExtent_saved) {
            MyFreePool__(RelocExtent_saved);
        }
    } _SEH2_END;
#endif //_BROWSE_UDF_
    return RC;
} // end UDFTRead()

#ifdef UDF_ASYNC_IO
/*
    This routine performs asynchronous low-level read
    Is not used now.
 */
OSSTATUS
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
    OSSTATUS RC = STATUS_SUCCESS;
//    LARGE_INTEGER delay;
    uint32 retry = UDF_READ_MAX_RETRY;
    PVCB Vcb = (PVCB)_Vcb;
    Vcb->VCBFlags |= UDF_VCB_SKIP_EJECT_CHECK;
    uint32 rLba;
    uint32 BCount;

    ASSERT(Buffer);

    (*ReadBytes) = 0;

    RelocExtent = UDFRelocateSectors(Vcb, LBA, BCount = Length >> Vcb->BlockSizeBits);
    if(!RelocExtent) return STATUS_INSUFFICIENT_RESOURCES;
    if(RelocExtent == UDF_NO_EXTENT_MAP) {
        rLba = LBA;
        if(rLba >= (Vcb->CDR_Mode ? Vcb->NWA : Vcb->LastLBA + 1)) {
            RtlZeroMemory(Buffer, Length);
            return STATUS_SUCCESS;
        }
retry_1:
        RC = UDFPrepareForReadOperation(Vcb, rLba, BCount);
        if(!OS_SUCCESS(RC)) return RC;
        rLba = UDFFixFPAddress(Vcb, rLba);
        RC = UDFPhReadSynchronous(Vcb->TargetDeviceObject, Buffer, Length,
                   ((uint64)rLba) << Vcb->BlockSizeBits, ReadBytes, 0);
        Vcb->VCBFlags &= ~UDF_VCB_LAST_WRITE;
        Vcb->VCBFlags |= UDF_VCB_SKIP_EJECT_CHECK;
        if(!OS_SUCCESS(RC) &&
            OS_SUCCESS(RC = UDFRecoverFromError(Vcb, FALSE, RC, rLba, BCount, &retry)) )
            goto retry_1;
        return RC;
    }
    // read according to relocation table
    RelocExtent_saved = RelocExtent;
    for(uint32 i=0; RelocExtent->extLength; i++, RelocExtent++) {
        SIZE_T _ReadBytes;
        rLba = RelocExtent->extLocation;
        if(rLba >= (Vcb->CDR_Mode ? Vcb->NWA : Vcb->LastLBA + 1)) {
            RtlZeroMemory(Buffer, _ReadBytes = RelocExtent->extLength);
            RC = STATUS_SUCCESS;
            goto TR_continue;
        }
        BCount = RelocExtent->extLength>>Vcb->BlockSizeBits;
retry_2:
        RC = UDFPrepareForReadOperation(Vcb, rLba, RelocExtent->extLength >> Vcb->BlockSizeBits);
        if(!OS_SUCCESS(RC)) break;
        rLba = UDFFixFPAddress(Vcb, rLba);
        RC = UDFPhReadSynchronous(Vcb->TargetDeviceObject, Buffer, RelocExtent->extLength,
                   ((uint64)rLba) << Vcb->BlockSizeBits, &_ReadBytes, 0);
        Vcb->VCBFlags &= ~UDF_VCB_LAST_WRITE;
        Vcb->VCBFlags |= UDF_VCB_SKIP_EJECT_CHECK;
        if(!OS_SUCCESS(RC) &&
            OS_SUCCESS(RC = UDFRecoverFromError(Vcb, FALSE, RC, rLba, BCount, &retry)) )
            goto retry_2;
TR_continue:
        (*ReadBytes) += _ReadBytes;
        if(!OS_SUCCESS(RC)) break;
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
OSSTATUS
UDFPrepareForWriteOperation(
    IN PVCB Vcb,
    IN uint32 Lba,
    IN uint32 BCount
    )
{
#ifndef UDF_READ_ONLY_BUILD

#ifdef _UDF_STRUCTURES_H_
    if(Vcb->BSBM_Bitmap) {
        ULONG i;
        for(i=0; i<BCount; i++) {
            if(UDFGetBit((uint32*)(Vcb->BSBM_Bitmap), Lba+i)) {
                UDFPrint(("W: Known BB @ %#x\n", Lba));
                //return STATUS_FT_WRITE_RECOVERY; // this shall not be treated as error and
                                                   // we shall get IO request to BAD block
                return STATUS_DEVICE_DATA_ERROR;
            }
        }
    }
#endif //_UDF_STRUCTURES_H_

    Vcb->VCBFlags |= UDF_VCB_LAST_WRITE;

    return STATUS_SUCCESS;

#endif //UDF_READ_ONLY_BUILD
    UDFPrint(("  no suitable track!\n"));
    return STATUS_INVALID_PARAMETER;
} // end UDFPrepareForWriteOperation()

//#ifdef _BROWSE_UDF_
/*
    This routine tries to recover from hardware error
    Return: STATUS_SUCCESS - retry requst
            STATUS_XXX - unrecoverable error
 */
OSSTATUS
UDFRecoverFromError(
    IN PVCB Vcb,
    IN BOOLEAN WriteOp,
    IN OSSTATUS status,
    IN uint32 Lba,
    IN uint32 BCount,
 IN OUT uint32* retry
    )
{
    return status;

} // end UDFRecoverFromError()

//#endif //_BROWSE_UDF_

/*
    use standard way to determine disk layout (ReadTOC cmd)
 */
OSSTATUS
UDFUseStandard(
    PDEVICE_OBJECT DeviceObject, // the target device object
    PVCB           Vcb           // Volume control block from this DevObj
    )
{
    OSSTATUS                RC = STATUS_SUCCESS;
    CDROM_TOC_LARGE*        toc = (CDROM_TOC_LARGE*)MyAllocatePool__(NonPagedPool, sizeof(CDROM_TOC_LARGE));
    CDROM_TOC_SESSION_DATA* LastSes = (CDROM_TOC_SESSION_DATA*)MyAllocatePool__(NonPagedPool, sizeof(CDROM_TOC_SESSION_DATA));
    uint32                  LocalTrackCount;
    uint32                  TocEntry;
    void*                   TempBuffer = NULL;
#ifdef _BROWSE_UDF_
    uint32                  OldTrkNum;
    uint32                  TrkNum;
    SIZE_T                  ReadBytes, i, len;
#endif //_BROWSE_UDF_

    UDFPrint(("UDFUseStandard\n"));

    _SEH2_TRY {

        if(!toc || !LastSes) {
            try_return (RC = STATUS_INSUFFICIENT_RESOURCES);
        }
        RtlZeroMemory(toc, sizeof(CDROM_TOC_LARGE));

        CDROM_READ_TOC_EX Command;

        RtlZeroMemory(&Command, sizeof(Command));

        RC = UDFPhSendIOCTL(IOCTL_CDROM_READ_TOC_EX,
                            DeviceObject,
                            &Command,
                            sizeof(Command),
                            toc,
                            sizeof(CDROM_TOC_LARGE),
                            TRUE,
                            NULL);

        if(!NT_SUCCESS(RC)) {

            // try using the MSF mode
            Command.Msf = 1;

            RC = UDFPhSendIOCTL(IOCTL_CDROM_READ_TOC_EX,
                                DeviceObject,
                                &Command,
                                sizeof(Command),
                                toc,
                                sizeof(CDROM_TOC_LARGE),
                                TRUE,
                                NULL);
        }

        // If even standard read toc does not work, then use default values
        if(!OS_SUCCESS(RC)) {

            RC = UDFReallocTrackMap(Vcb, 2);
            if(!OS_SUCCESS(RC)) {
                try_return(RC);
            }

            Vcb->LastSession=1;
            Vcb->FirstTrackNum=1;
//            Vcb->FirstLBA=0;
            Vcb->LastTrackNum=1;
            Vcb->TrackMap[1].FirstLba = Vcb->FirstLBA;
            Vcb->TrackMap[1].LastLba = Vcb->LastLBA;
            Vcb->TrackMap[1].PacketSize = PACKETSIZE_UDF;

#ifdef _BROWSE_UDF_
                if(UDFGetDevType(DeviceObject) == FILE_DEVICE_DISK) {
                    try_return(RC = STATUS_SUCCESS);
                }
#endif //_BROWSE_UDF_

            Vcb->LastPossibleLBA = max(Vcb->LastLBA, DEFAULT_LAST_LBA_FP_CD);
            Vcb->TrackMap[1].DataParam = TrkInfo_Dat_XA | TrkInfo_FP | TrkInfo_Packet;
            Vcb->TrackMap[1].TrackParam = TrkInfo_Trk_XA;
            Vcb->TrackMap[1].NWA = 0xffffffff;
            Vcb->NWA = DEFAULT_LAST_LBA_FP_CD + 7 + 1;
            try_return(RC = STATUS_SUCCESS);
        }

        LocalTrackCount = toc->LastTrack - toc->FirstTrack + 1;

        // Get out if there is an immediate problem with the TOC.
        if(toc->LastTrack - toc->FirstTrack >= MAXIMUM_NUMBER_TRACKS_LARGE) {
            try_return(RC = STATUS_DISK_CORRUPT_ERROR);
        }

#ifdef _BROWSE_UDF_
        Vcb->LastTrackNum = toc->LastTrack;
        Vcb->FirstTrackNum = toc->FirstTrack;
        // some devices report LastTrackNum=0 for full disks
        Vcb->LastTrackNum = max(Vcb->LastTrackNum, Vcb->FirstTrackNum);

        RC = UDFReallocTrackMap(Vcb, MAXIMUM_NUMBER_TRACKS_LARGE+1);

        if(!OS_SUCCESS(RC)) {
            BrutePoint();
            try_return(RC);
        }
        // find 1st and last session
        RC = UDFPhSendIOCTL(IOCTL_CDROM_GET_LAST_SESSION,
            DeviceObject,
            NULL,
            0,
            LastSes,
            sizeof(CDROM_TOC_SESSION_DATA),
            TRUE,
            NULL);

        if(OS_SUCCESS(RC)) {

            TrkNum = LastSes->TrackData[0].TrackNumber;

            Vcb->FirstLBA = 0;
            SwapCopyUchar4(&Vcb->FirstLBA, &LastSes->TrackData[0].Address);

            Vcb->LastSession = LastSes->FirstCompleteSession;
            for(TocEntry=0;TocEntry<LocalTrackCount + 1;TocEntry++) {
                if(toc->TrackData[TocEntry].TrackNumber == TrkNum) {
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
                try_return(RC = STATUS_SUCCESS);
            }
            UDFPrint(("Track N %d (0x%x) first LBA %ld (%lx) \n",TrkNum,TrkNum,
                MSF_TO_LBA(TempMSF[1],TempMSF[2],TempMSF[3]),
                MSF_TO_LBA(TempMSF[1],TempMSF[2],TempMSF[3])));
#endif // UDF_DBG
            if(TOC_LastTrack_ID == TrkNum) {

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
                if(Vcb->TrackMap[TrkNum].FirstLba & 0x80000000)
                    Vcb->TrackMap[TrkNum].FirstLba = 0;
                if(TrkNum) {
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
        RC = STATUS_SUCCESS;
        // find last _valid_ track
        for(;TrkNum;TrkNum--) {
            if((Vcb->TrackMap[TrkNum].DataParam  != TrkInfo_Dat_unknown) &&
               (Vcb->TrackMap[TrkNum].TrackParam != TrkInfo_Trk_unknown)) {
                RC = STATUS_UNSUCCESSFUL;
                Vcb->LastTrackNum = TrkNum;
                break;
            }
        }
        // no valid tracks...
        if(!TrkNum) {
            UDFPrint(("UDFUseStandard: no valid tracks...\n"));
            try_return(RC = STATUS_UNRECOGNIZED_VOLUME);
        }
        i = 0;

        // Check for last Variable Packet(VP) track. Some last sectors may belong to Link-data &
        // be unreadable. We should forget about them, because UDF needs
        // last _readable_ sector.

        TempBuffer = MyAllocatePool__(NonPagedPool, Vcb->BlockSize);

        if(!TempBuffer) { 
            return STATUS_INSUFFICIENT_RESOURCES;
        }

        while(!OS_SUCCESS(RC) && (i<8)) {
            RC = UDFPhReadSynchronous(Vcb->TargetDeviceObject, TempBuffer, Vcb->BlockSize,
                       ((uint64)(Vcb->TrackMap[TrkNum].LastLba-i)) << Vcb->BlockSizeBits, &ReadBytes, PH_TMP_BUFFER);
            i++;
        }
        if(OS_SUCCESS(RC)) {
            Vcb->LastLBA = Vcb->TrackMap[TrkNum].LastLba-i+1;
/*            if(i) {
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

            while(!OS_SUCCESS(RC) && (i<9)) {
                RC = UDFPhReadSynchronous(Vcb->TargetDeviceObject, TempBuffer, Vcb->BlockSize,
                           ((uint64)(Vcb->TrackMap[TrkNum].FirstLba-i+len)) << Vcb->BlockSizeBits, &ReadBytes, PH_TMP_BUFFER);
                i++;
            }
            if(OS_SUCCESS(RC)) {
                Vcb->LastLBA =
                Vcb->TrackMap[TrkNum].LastLba = Vcb->TrackMap[TrkNum].FirstLba-i+len+1;
                Vcb->TrackMap[TrkNum].PacketSize = PACKETSIZE_UDF;
//                Vcb->TrackMap[TrkNum].;
            } else
            if(RC == STATUS_INVALID_DEVICE_REQUEST) {
                // wrap return code from Audio-disk
                RC = STATUS_SUCCESS;
            }
        }

#ifdef UDF_CDRW_EMULATION_ON_ROM
        Vcb->LastPossibleLBA = Vcb->LastLBA+7+1+1024;
        Vcb->NWA = Vcb->LastLBA+7+1;
#else
        Vcb->LastPossibleLBA =
        Vcb->NWA = Vcb->LastLBA+7+1;
#endif //UDF_CDRW_EMULATION_ON_ROM

#else //_BROWSE_UDF_

        Vcb->FirstTrackNum=toc->Tracks.Last_TrackSes;
        Vcb->LastTrackNum=toc->Tracks.First_TrackSes;

        // Scan toc for first & last LBA
        for(TocEntry=0;TocEntry<LocalTrackCount + 1;TocEntry++) {
#define TempMSF toc->TrackData[TocEntry].LBA
            if(Vcb->FirstTrackNum == toc->TrackData[TocEntry].TrackNum) {
                Vcb->FirstLBA = MSF_TO_LBA(TempMSF[1],TempMSF[2],TempMSF[3]);
                if(Vcb->FirstLBA & 0x80000000) {
                    Vcb->FirstLBA = 0;
                }
            }
            if(TOC_LastTrack_ID   == toc->TrackData[TocEntry].TrackNum) {
                Vcb->LastLBA = MSF_TO_LBA(TempMSF[1],TempMSF[2],TempMSF[3])-1;
            }
#undef TempMSF
        }

//        Vcb->LastLBA=PacketVariable2Fixed(Vcb->LastLBA)-2;
        Vcb->LastPossibleLBA = DEFAULT_LAST_LBA_FP_CD;
#endif //_BROWSE_UDF_
try_exit: NOTHING;
    } _SEH2_FINALLY {
        if(toc) MyFreePool__(toc);
        if(LastSes) MyFreePool__(LastSes);
        if(TempBuffer) MyFreePool__(TempBuffer);
    } _SEH2_END;

    return RC;
} // end UDFUseStandard()

/*
    Get block size (for read operation)
 */
OSSTATUS
UDFGetBlockSize(
    IN PDEVICE_OBJECT DeviceObject,      // the target device object
    IN PVCB           Vcb                // Volume control block from this DevObj
    )
{
    OSSTATUS        RC = STATUS_SUCCESS;
    DISK_GEOMETRY_EX DiskGeometryEx;
    PARTITION_INFORMATION  PartitionInfo;

#ifdef _BROWSE_UDF_
    if(UDFGetDevType(DeviceObject) == FILE_DEVICE_DISK) {
        UDFPrint(("UDFGetBlockSize: HDD\n"));
        RC = UDFPhSendIOCTL(IOCTL_DISK_GET_DRIVE_GEOMETRY_EX,DeviceObject,
            0,NULL,
            &DiskGeometryEx,sizeof(DISK_GEOMETRY_EX),
            TRUE,NULL );
        Vcb->BlockSize = (OS_SUCCESS(RC)) ? DiskGeometryEx.Geometry.BytesPerSector : 512;
        if(!NT_SUCCESS(RC))
            try_return(RC);
        RC = UDFPhSendIOCTL(IOCTL_DISK_GET_PARTITION_INFO,DeviceObject,
            0,NULL,
            &PartitionInfo,sizeof(PARTITION_INFORMATION),
            TRUE,NULL );
        if(!NT_SUCCESS(RC)) {
            UDFPrint(("UDFGetBlockSize: IOCTL_DISK_GET_PARTITION_INFO failed\n"));
            if(RC == STATUS_INVALID_DEVICE_REQUEST) /* ReactOS Code Change (was =) */
                RC = STATUS_UNRECOGNIZED_VOLUME;
            try_return(RC);
        }
        if(PartitionInfo.PartitionType != PARTITION_IFS && PartitionInfo.PartitionType != PARTITION_HUGE) {
            UDFPrint(("UDFGetBlockSize: PartitionInfo.PartitionType != PARTITION_IFS\n"));
            try_return(RC = STATUS_UNRECOGNIZED_VOLUME);
        }
    } else {
        RC = UDFPhSendIOCTL(IOCTL_CDROM_GET_DRIVE_GEOMETRY_EX,DeviceObject,
            &DiskGeometryEx,sizeof(DISK_GEOMETRY_EX),
            &DiskGeometryEx,sizeof(DISK_GEOMETRY_EX),
            TRUE,NULL );

        if(RC == STATUS_DEVICE_NOT_READY) {
            // probably, the device is really busy, may be by CD/DVD recording
            UserPrint(("  busy (0)\n"));
            try_return(RC);
        }

        Vcb->BlockSize = (OS_SUCCESS(RC)) ? DiskGeometryEx.Geometry.BytesPerSector : 2048;
    }

#endif //_BROWSE_UDF_

    // Block size must be an even multiple of 512
    switch (Vcb->BlockSize) {
        case 2048: Vcb->BlockSizeBits = 11; break;
        case 512:  Vcb->BlockSizeBits = 9; break;
        case 1024: Vcb->BlockSizeBits = 10; break;
        case 4096: Vcb->BlockSizeBits = 12; break;
        case 8192: Vcb->BlockSizeBits = 13; break;
        default:
        {
            UserPrint(("UDF: Bad block size (%ld)\n", Vcb->BlockSize));
            try_return(RC = STATUS_UNSUCCESSFUL);
        }
    }

    if(
#ifdef _BROWSE_UDF_
        UDFGetDevType(DeviceObject) == FILE_DEVICE_DISK ||
#endif //_BROWSE_UDF_
        FALSE) {
        Vcb->FirstLBA=0;//(ULONG)(PartitionInfo->StartingOffset.QuadPart >> Vcb->BlockSizeBits);
        Vcb->LastPossibleLBA =
        Vcb->LastLBA = (uint32)(DiskGeometryEx.DiskSize.QuadPart >> Vcb->BlockSizeBits)/* + Vcb->FirstLBA*/ - 1;
    } else {
        Vcb->FirstLBA=0;
        if(OS_SUCCESS(RC)) {
            Vcb->LastLBA = (uint32)(DiskGeometryEx.Geometry.Cylinders.QuadPart *
                                    DiskGeometryEx.Geometry.TracksPerCylinder *
                                    DiskGeometryEx.Geometry.SectorsPerTrack - 1);
            if(Vcb->LastLBA == 0x7fffffff) {
                ASSERT(FALSE);
            }
        } else {
            ASSERT(FALSE);
        }
        Vcb->LastPossibleLBA = Vcb->LastLBA;
    }

#ifdef _BROWSE_UDF_
//    if(UDFGetDevType(DeviceObject) == FILE_DEVICE_DISK) {
        Vcb->WriteBlockSize = PACKETSIZE_UDF*Vcb->BlockSize;
//    } else {
//        Vcb->WriteBlockSize = PACKETSIZE_UDF*Vcb->BlockSize;
//    }
#else //_BROWSE_UDF_
    if(fms->opt_media == MT_HD) {
        Vcb->WriteBlockSize = Vcb->BlockSize;
    } else {
        Vcb->WriteBlockSize = PACKETSIZE_UDF*Vcb->BlockSize;
    }
#endif //_BROWSE_UDF_

    RC = STATUS_SUCCESS;

try_exit:   NOTHING;

    UDFPrint(("UDFGetBlockSize:\nBlock size is %x, Block size bits %x, Last LBA is %x\n",
              Vcb->BlockSize, Vcb->BlockSizeBits, Vcb->LastLBA));

    return RC;

} // end UDFGetBlockSize()

#ifdef _BROWSE_UDF_

uint32
UDFFixFPAddress(
    IN PVCB           Vcb,               // Volume control block from this DevObj
    IN uint32         Lba
    )
{
    uint32 i = Vcb->LastReadTrack;
    uint32 pk;
    uint32 rel;

//    if(Vcb->CompatFlags & UDF_VCB_IC_MRW_ADDR_PROBLEM) {
    if(Vcb->TrackMap[i].Flags & TrackMap_FixMRWAddressing) {
        pk = Lba / MRW_DA_SIZE;
        rel = Lba % MRW_DA_SIZE;
        Lba = pk*MRW_DMA_SEGMENT_SIZE + rel;
        Lba += MRW_DMA_OFFSET;
    }
    if(Vcb->TrackMap[i].Flags & TrackMap_FixFPAddressing) {
        if(Lba < 0x20)
            return Lba;
        pk = Lba / Vcb->TrackMap[i].PacketSize;
        rel = Lba % Vcb->TrackMap[i].PacketSize;
        UDFPrint(("FixFPAddr: %x -> %x\n", Lba, pk*(Vcb->TrackMap[i].PacketSize+7) + rel));
        return pk*(Vcb->TrackMap[i].PacketSize+7) + rel /*- Vcb->TrackMap[i].PacketFPOffset*/;
    }
    return Lba;
} // end UDFFixFPAddress()

#endif //_BROWSE_UDF_

/*
    detect device driver & try to read disk layout (use all methods)
 */
OSSTATUS
UDFGetDiskInfo(
    IN PDEVICE_OBJECT DeviceObject,      // the target device object
    IN PVCB           Vcb                // Volume control block from this DevObj
    )
{
    OSSTATUS        RC = STATUS_UNRECOGNIZED_VOLUME;
    uint32 i;

    UDFPrint(("UDFGetDiskInfo\n"));

    _SEH2_TRY {
        RC = UDFGetBlockSize(DeviceObject, Vcb);

        if(!OS_SUCCESS(RC)) {
            try_return(RC);
        }

        RC = UDFUseStandard(DeviceObject, Vcb);

        if(!OS_SUCCESS(RC)) {
            try_return(RC);
        }

try_exit:   NOTHING;

    } _SEH2_FINALLY {

        if((Vcb->LastPossibleLBA & 0x80000000) || (Vcb->LastPossibleLBA < Vcb->LastLBA)) {
            UDFPrint(("UDF: bad LastPossibleLBA %x -> %x\n", Vcb->LastPossibleLBA, Vcb->LastLBA));
            Vcb->LastPossibleLBA = Vcb->LastLBA;
        }
        if(!Vcb->WriteBlockSize)
            Vcb->WriteBlockSize = PACKETSIZE_UDF*Vcb->BlockSize;

#ifdef _BROWSE_UDF_
        if(Vcb->TrackMap) {
            if(Vcb->TrackMap[Vcb->LastTrackNum].LastLba > Vcb->NWA) {
                if(Vcb->NWA) {
                    if(Vcb->TrackMap[Vcb->LastTrackNum].DataParam & TrkInfo_FP) {
                        Vcb->LastLBA = Vcb->NWA-1;
                    } else {
                        Vcb->LastLBA = Vcb->NWA-7-1;
                    }
                }
            } else {
                if((Vcb->LastTrackNum > 1) &&
                   (Vcb->TrackMap[Vcb->LastTrackNum-1].FirstLba >= Vcb->TrackMap[Vcb->LastTrackNum-1].LastLba)) {
                    Vcb->LastLBA = Vcb->TrackMap[Vcb->LastTrackNum-1].LastLba;
                }
            }
        }

        for(i=0; i<32; i++) {
            if(!(Vcb->LastPossibleLBA >> i))
                break;
        }
        if(i > 20) {
            Vcb->WCacheBlocksPerFrameSh = max(Vcb->WCacheBlocksPerFrameSh, (2*i)/5+2);
            Vcb->WCacheBlocksPerFrameSh = min(Vcb->WCacheBlocksPerFrameSh, 16);
        }

#endif //_BROWSE_UDF_

        if(Vcb->VCBFlags & UDF_VCB_FLAGS_VOLUME_READ_ONLY) {
            if(!Vcb->BlankCD && Vcb->MediaType != MediaType_UnknownSize_CDRW) {
                UDFPrint(("UDFGetDiskInfo: R/O+!Blank+!RW -> !RAW\n"));
                Vcb->VCBFlags &= ~UDF_VCB_FLAGS_RAW_DISK;
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
        UDFPrint(("UDF: blocks per frame: %x\n",1 << Vcb->WCacheBlocksPerFrameSh));
        UDFPrint(("UDF: Flags: %s%s\n",
                 Vcb->VCBFlags & UDF_VCB_FLAGS_RAW_DISK ? "RAW " : "",
                 Vcb->VCBFlags & UDF_VCB_FLAGS_VOLUME_READ_ONLY ? "R/O " : "WR "
                 ));
        UDFPrint(("UDF: ------------------------------------------\n"));

    } _SEH2_END;

    UDFPrint(("UDFGetDiskInfo: %x\n", RC));
    return(RC);

} // end UDFGetDiskInfo()

//#ifdef _BROWSE_UDF_

OSSTATUS
UDFPrepareForReadOperation(
    IN PVCB Vcb,
    IN uint32 Lba,
    IN uint32 BCount
    )
{
    if( (Vcb->FsDeviceType != FILE_DEVICE_CD_ROM_FILE_SYSTEM) ) {
        Vcb->VCBFlags &= ~UDF_VCB_LAST_WRITE;
        return STATUS_SUCCESS;
    }
    uint32 i = Vcb->LastReadTrack;
#ifdef _BROWSE_UDF_
    PUCHAR tmp;
    OSSTATUS RC;
    SIZE_T ReadBytes;
#endif //_BROWSE_UDF_

#ifdef _UDF_STRUCTURES_H_
    if(Vcb->BSBM_Bitmap) {
        ULONG i;
        for(i=0; i<BCount; i++) {
            if(UDFGetBit((uint32*)(Vcb->BSBM_Bitmap), Lba+i)) {
                UDFPrint(("R: Known BB @ %#x\n", Lba));
                //return STATUS_FT_WRITE_RECOVERY; // this shall not be treated as error and
                                                   // we shall get IO request to BAD block
                return STATUS_DEVICE_DATA_ERROR;
            }
        }
    }
#endif //_UDF_STRUCTURES_H_

#ifdef _BROWSE_UDF_

    if(UDFIsDvdMedia(Vcb))
        return STATUS_SUCCESS;

    if(Vcb->LastReadTrack &&
       ((Vcb->TrackMap[i].FirstLba <= Lba) || (Vcb->TrackMap[i].FirstLba & 0x80000000)) &&
       (Vcb->TrackMap[i].LastLba >= Lba)) {
check_for_data_track:
        // check track mode (Mode1/XA)
        switch((Vcb->TrackMap[i].DataParam & TrkInfo_Dat_Mask)) {
        case TrkInfo_Dat_Mode1: // Mode1
        case TrkInfo_Dat_XA:    // XA Mode2
        case TrkInfo_Dat_Unknown: // for some stupid irons
            break;
        default:
            Vcb->IncrementalSeekState = INCREMENTAL_SEEK_NONE;
            return STATUS_INVALID_PARAMETER;
        }
    } else {
        for(i=Vcb->FirstTrackNum; i<=Vcb->LastTrackNum; i++) {
            if(((Vcb->TrackMap[i].FirstLba > Lba) && !(Vcb->TrackMap[i].FirstLba & 0x80000000)) ||
               (Vcb->TrackMap[i].LastLba < Lba))
                continue;
            Vcb->LastReadTrack = i;
            goto check_for_data_track;
        }
        Vcb->LastReadTrack = 0;
    }
    if(Vcb->IncrementalSeekState != INCREMENTAL_SEEK_WORKAROUND) {
        Vcb->IncrementalSeekState = INCREMENTAL_SEEK_NONE;
        return STATUS_SUCCESS;
    }
    UDFPrint(("    UDFPrepareForReadOperation: seek workaround...\n"));
    Vcb->IncrementalSeekState = INCREMENTAL_SEEK_DONE;

    tmp = (PUCHAR)DbgAllocatePoolWithTag(NonPagedPool, Vcb->BlockSize, 'bNWD');
    if(!tmp) {
        Vcb->IncrementalSeekState = INCREMENTAL_SEEK_NONE;
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    for(i=0x1000; i<=Lba; i+=0x1000) {
        RC = UDFPhReadSynchronous(Vcb->TargetDeviceObject, tmp, Vcb->BlockSize,
                   ((uint64)UDFFixFPAddress(Vcb,i)) << Vcb->BlockSizeBits, &ReadBytes, 0);
        UDFPrint(("    seek workaround, LBA %x, status %x\n", i, RC));
    }
    DbgFreePool(tmp);
#endif //_BROWSE_UDF_

    return STATUS_SUCCESS;
} // end UDFPrepareForReadOperation()

//#endif //_BROWSE_UDF_

/*
    This routine reads physical sectors
 */
OSSTATUS
UDFReadSectors(
    IN PVCB Vcb,
    IN BOOLEAN Translate,       // Translate Logical to Physical
    IN uint32 Lba,
    IN uint32 BCount,
    IN BOOLEAN Direct,
    OUT int8* Buffer,
    OUT PSIZE_T ReadBytes
    )
{
    if(Vcb->FastCache.ReadProc && (KeGetCurrentIrql() < DISPATCH_LEVEL)) {
        return WCacheReadBlocks__(&Vcb->FastCache, Vcb, Buffer, Lba, BCount, ReadBytes, Direct);
    }
    return UDFTRead(Vcb, Buffer, BCount*Vcb->BlockSize, Lba, ReadBytes);
} // end UDFReadSectors()

#ifdef _BROWSE_UDF_

/*
    This routine reads physical sectors
 */
OSSTATUS
UDFReadInSector(
    IN PVCB Vcb,
    IN BOOLEAN Translate,       // Translate Logical to Physical
    IN uint32 Lba,
    IN uint32 i,                 // offset in sector
    IN uint32 l,                 // transfer length
    IN BOOLEAN Direct,          // Disable access to non-cached data
    OUT int8* Buffer,
    OUT PSIZE_T ReadBytes
    )
{
    int8* tmp_buff;
    OSSTATUS status;
    SIZE_T _ReadBytes;

    (*ReadBytes) = 0;
    if(Vcb->FastCache.ReadProc && (KeGetCurrentIrql() < DISPATCH_LEVEL)) {
        status = WCacheDirect__(&Vcb->FastCache, Vcb, Lba, FALSE, &tmp_buff, Direct);
        if(OS_SUCCESS(status)) {
            (*ReadBytes) += l;
            RtlCopyMemory(Buffer, tmp_buff+i, l);
        }
        if(!Direct) WCacheEODirect__(&Vcb->FastCache, Vcb);
    } else {
        if(Direct) {
            return STATUS_INVALID_PARAMETER;
        }
        tmp_buff = (int8*)MyAllocatePool__(NonPagedPool, Vcb->BlockSize);
        if(!tmp_buff) return STATUS_INSUFFICIENT_RESOURCES;
        status = UDFReadSectors(Vcb, Translate, Lba, 1, FALSE, tmp_buff, &_ReadBytes);
        if(OS_SUCCESS(status)) {
            (*ReadBytes) += l;
            RtlCopyMemory(Buffer, tmp_buff+i, l);
        }
        MyFreePool__(tmp_buff);
    }
    return status;
} // end UDFReadInSector()

/*
    This routine reads data of unaligned offset & length
 */
OSSTATUS
UDFReadData(
    IN PVCB Vcb,
    IN BOOLEAN Translate,       // Translate Logical to Physical
    IN int64 Offset,
    IN uint32 Length,
    IN BOOLEAN Direct,          // Disable access to non-cached data
    OUT int8* Buffer,
    OUT PSIZE_T ReadBytes
    )
{
    uint32 i, l, Lba, BS=Vcb->BlockSize;
    uint32 BSh=Vcb->BlockSizeBits;
    OSSTATUS status;
    SIZE_T _ReadBytes = 0;
    Vcb->VCBFlags |= UDF_VCB_SKIP_EJECT_CHECK;
    uint32 to_read;

    (*ReadBytes) = 0;
    if(!Length) return STATUS_SUCCESS;
    if(Vcb->VCBFlags & UDF_VCB_FLAGS_DEAD)
        return STATUS_NO_SUCH_DEVICE;
    // read tail of the 1st sector if Offset is not sector_size-aligned
    Lba = (uint32)(Offset >> BSh);
    if((i = (uint32)(Offset & (BS-1)))) {
        l = (BS - i) < Length ?
            (BS - i) : Length;
        // here we use 'ReadBytes' 'cause now it's set to zero
        status = UDFReadInSector(Vcb, Translate, Lba, i, l, Direct, Buffer, ReadBytes);
        if(!OS_SUCCESS(status)) return status;
        if(!(Length = Length - l)) return STATUS_SUCCESS;
        Lba ++;
        Buffer += l;
    }
    // read sector_size-aligned part
    i = Length >> BSh;
    while(i) {
        to_read = min(i, 64);
        status = UDFReadSectors(Vcb, Translate, Lba, to_read, Direct, Buffer, &_ReadBytes);
        (*ReadBytes) += _ReadBytes;
        if(!OS_SUCCESS(status)) {
            return status;
        }
        Buffer += to_read<<BSh;
        Length -= to_read<<BSh;
        Lba += to_read;
        i -= to_read;
    }
    // read head of the last sector
    if(!Length) return STATUS_SUCCESS;
    status = UDFReadInSector(Vcb, Translate, Lba, 0, Length, Direct, Buffer, &_ReadBytes);
    (*ReadBytes) += _ReadBytes;

    return status;
} // end UDFReadData()

#endif //_BROWSE_UDF_

#ifndef UDF_READ_ONLY_BUILD
/*
    This routine writes physical sectors. This routine supposes Lba & Length
    alignment on WriteBlock (packet) size.
 */
OSSTATUS
UDFWriteSectors(
    IN PVCB Vcb,
    IN BOOLEAN Translate,       // Translate Logical to Physical
    IN uint32 Lba,
    IN uint32 BCount,
    IN BOOLEAN Direct,          // Disable access to non-cached data
    IN int8* Buffer,
    OUT PSIZE_T WrittenBytes
    )
{
    OSSTATUS status;

#ifdef _BROWSE_UDF_
    if(!Vcb->Modified || (Vcb->IntegrityType == INTEGRITY_TYPE_CLOSE)) {
        UDFSetModified(Vcb);
        if(Vcb->LVid && !Direct) {
            status = UDFUpdateLogicalVolInt(Vcb,FALSE);
        }
    }

    if(Vcb->CDR_Mode) {
        if(Vcb->LastLBA < Lba+BCount-1)
            Vcb->LastLBA = Lba+BCount-1;
    }
#endif //_BROWSE_UDF_

    if(Vcb->FastCache.WriteProc && (KeGetCurrentIrql() < DISPATCH_LEVEL)) {
        status = WCacheWriteBlocks__(&(Vcb->FastCache), Vcb, Buffer, Lba, BCount, WrittenBytes, Direct);
        ASSERT(OS_SUCCESS(status));
#ifdef _BROWSE_UDF_
        UDFClrZeroBits(Vcb->ZSBM_Bitmap, Lba, BCount);
#endif //_BROWSE_UDF_
        return status;
    }

    status = UDFTWrite(Vcb, Buffer, BCount<<Vcb->BlockSizeBits, Lba, WrittenBytes);
    ASSERT(OS_SUCCESS(status));
#ifdef _BROWSE_UDF_
    UDFClrZeroBits(Vcb->ZSBM_Bitmap, Lba, BCount);
#endif //_BROWSE_UDF_
    return status;
} // end UDFWriteSectors()

OSSTATUS
UDFWriteInSector(
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
    OSSTATUS status;
#ifdef _BROWSE_UDF_
    SIZE_T _WrittenBytes;
    SIZE_T ReadBytes;

    if(!Vcb->Modified) {
        UDFSetModified(Vcb);
        if(Vcb->LVid)
            status = UDFUpdateLogicalVolInt(Vcb,FALSE);
    }

    if(Vcb->CDR_Mode) {
        if(Vcb->LastLBA < Lba)
            Vcb->LastLBA = Lba;
    }
#endif //_BROWSE_UDF_

    (*WrittenBytes) = 0;
#ifdef _BROWSE_UDF_
    if(Vcb->FastCache.WriteProc && (KeGetCurrentIrql() < DISPATCH_LEVEL)) {
#endif //_BROWSE_UDF_
        status = WCacheDirect__(&(Vcb->FastCache), Vcb, Lba, TRUE, &tmp_buff, Direct);
        if(OS_SUCCESS(status)) {
#ifdef _BROWSE_UDF_
            UDFClrZeroBit(Vcb->ZSBM_Bitmap, Lba);
#endif //_BROWSE_UDF_
            (*WrittenBytes) += l;
            RtlCopyMemory(tmp_buff+i, Buffer, l);
        }
        if(!Direct) WCacheEODirect__(&(Vcb->FastCache), Vcb);
#ifdef _BROWSE_UDF_
    } else {
        // If Direct = TRUE we should never get here, but...
        if(Direct) {
            BrutePoint();
            return STATUS_INVALID_PARAMETER;
        }
        tmp_buff = (int8*)MyAllocatePool__(NonPagedPool, Vcb->BlockSize);
        if(!tmp_buff) {
            BrutePoint();
            return STATUS_INSUFFICIENT_RESOURCES;
        }
        // read packet
        status = UDFReadSectors(Vcb, Translate, Lba, 1, FALSE, tmp_buff, &ReadBytes);
        if(!OS_SUCCESS(status)) goto EO_WrSctD;
        // modify packet
        RtlCopyMemory(tmp_buff+i, Buffer, l);
        // write modified packet
        status = UDFWriteSectors(Vcb, Translate, Lba, 1, FALSE, tmp_buff, &_WrittenBytes);
        if(OS_SUCCESS(status))
            (*WrittenBytes) += l;
EO_WrSctD:
        MyFreePool__(tmp_buff);
    }
    ASSERT(OS_SUCCESS(status));
    if(!OS_SUCCESS(status)) {
        UDFPrint(("UDFWriteInSector() for LBA %x failed\n", Lba));
    }
#endif //_BROWSE_UDF_
    return status;
} // end UDFWriteInSector()

/*
    This routine writes data at unaligned offset & length
 */
OSSTATUS
UDFWriteData(
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
    uint32 i, l, Lba, BS=Vcb->BlockSize;
    uint32 BSh=Vcb->BlockSizeBits;
    OSSTATUS status;
    SIZE_T _WrittenBytes;
    Vcb->VCBFlags |= UDF_VCB_SKIP_EJECT_CHECK;

    (*WrittenBytes) = 0;
    if(!Length) return STATUS_SUCCESS;
    if(Vcb->VCBFlags & UDF_VCB_FLAGS_DEAD)
        return STATUS_NO_SUCH_DEVICE;
    // write tail of the 1st sector if Offset is not sector_size-aligned
    Lba = (uint32)(Offset >> BSh);
    if((i = ((uint32)Offset & (BS-1)))) {
        l = (BS - i) < Length ?
            (BS - i) : Length;
        status = UDFWriteInSector(Vcb, Translate, Lba, i, l, Direct, Buffer, WrittenBytes);
        if(!OS_SUCCESS(status)) return status;
        if(!(Length = Length - l)) return STATUS_SUCCESS;
        Lba ++;
        Buffer += l;
    }
    // write sector_size-aligned part
    i = Length >> BSh;
    if(i) {
        status = UDFWriteSectors(Vcb, Translate, Lba, i, Direct, Buffer, &_WrittenBytes);
        (*WrittenBytes) += _WrittenBytes;
        if(!OS_SUCCESS(status)) return status;
        l = i<<BSh;
#ifdef _BROWSE_UDF_
        UDFClrZeroBits(Vcb->ZSBM_Bitmap, Lba, i);
#endif //_BROWSE_UDF_
        if(!(Length = Length - l)) return STATUS_SUCCESS;
        Lba += i;
        Buffer += l;
    }
    status = UDFWriteInSector(Vcb, Translate, Lba, 0, Length, Direct, Buffer, &_WrittenBytes);
    (*WrittenBytes) += _WrittenBytes;
#ifdef _BROWSE_UDF_
    UDFClrZeroBit(Vcb->ZSBM_Bitmap, Lba);
#endif //_BROWSE_UDF_

    return status;
} // end UDFWriteData()

#endif //UDF_READ_ONLY_BUILD
