////////////////////////////////////////////////////////////////////
// Copyright (C) Alexander Telyatnikov, Ivan Keliukh, Yegor Anchishkin, SKIF Software, 1999-2013. Kiev, Ukraine
// All rights reserved
// This file was released under the GPLv2 on June 2015.
////////////////////////////////////////////////////////////////////
/*
        Module name:

   alloc.cpp

        Abstract:

   This file contains filesystem-specific routines
   responsible for disk space management

*/

#include "udf.h"

#define         UDF_BUG_CHECK_ID                UDF_FILE_UDF_INFO_ALLOC

static const int8 bit_count_tab[] = {
    0, 1, 1, 2, 1, 2, 2, 3,   1, 2, 2, 3, 2, 3, 3, 4,
    1, 2, 2, 3, 2, 3, 3, 4,   2, 3, 3, 4, 3, 4, 4, 5,
    1, 2, 2, 3, 2, 3, 3, 4,   2, 3, 3, 4, 3, 4, 4, 5,
    2, 3, 3, 4, 3, 4, 4, 5,   3, 4, 4, 5, 4, 5, 5, 6,
    1, 2, 2, 3, 2, 3, 3, 4,   2, 3, 3, 4, 3, 4, 4, 5,
    2, 3, 3, 4, 3, 4, 4, 5,   3, 4, 4, 5, 4, 5, 5, 6,
    2, 3, 3, 4, 3, 4, 4, 5,   3, 4, 4, 5, 4, 5, 5, 6,
    3, 4, 4, 5, 4, 5, 5, 6,   4, 5, 5, 6, 5, 6, 6, 7,

    1, 2, 2, 3, 2, 3, 3, 4,   2, 3, 3, 4, 3, 4, 4, 5,
    2, 3, 3, 4, 3, 4, 4, 5,   3, 4, 4, 5, 4, 5, 5, 6,
    2, 3, 3, 4, 3, 4, 4, 5,   3, 4, 4, 5, 4, 5, 5, 6,
    3, 4, 4, 5, 4, 5, 5, 6,   4, 5, 5, 6, 5, 6, 6, 7,
    2, 3, 3, 4, 3, 4, 4, 5,   3, 4, 4, 5, 4, 5, 5, 6,
    3, 4, 4, 5, 4, 5, 5, 6,   4, 5, 5, 6, 5, 6, 6, 7,
    3, 4, 4, 5, 4, 5, 5, 6,   4, 5, 5, 6, 5, 6, 6, 7,
    4, 5, 5, 6, 5, 6, 6, 7,   5, 6, 6, 7, 6, 7, 7, 8
};

/*
    This routine converts physical address to logical in specified partition
 */
uint32
UDFPhysLbaToPart(
    IN PVCB Vcb,
    IN uint32 RefPartNum,
    IN uint32 Addr
    )
{
    uint32 retval = 0;
    PUDFPartMap pm = Vcb->Partitions;
    uint32 i;
    // walk through partition maps to find suitable one...
    for(i=RefPartNum; i<Vcb->PartitionMaps; i++, pm++) {
        if (pm->PartitionNum == UDFGetPartNumByPartRef(Vcb, RefPartNum))
            // wow! return relative address
            retval = (Addr - pm->PartitionRoot);
    }

#ifdef UDF_DBG
    {
        // validate return value
        lb_addr locAddr;
        locAddr.logicalBlockNum = retval;
        locAddr.partitionReferenceNum = (uint16)RefPartNum;
        UDFPartLbaToPhys(Vcb, &locAddr);
    }
#endif // UDF_DBG

    return retval;
} // end UDFPhysLbaToPart()

/*
    This routine returns physical Lba for partition-relative addr
 */
uint32
__fastcall
UDFPartLbaToPhys(
  IN PVCB Vcb,
  IN lb_addr* Addr
  )
{
    uint32 i, a;
    if (Addr->partitionReferenceNum >= Vcb->PartitionMaps) {
        AdPrint(("UDFPartLbaToPhys: part %x, lbn %x (err)\n",
            Addr->partitionReferenceNum, Addr->logicalBlockNum));
        if (Vcb->PartitionMaps &&
           (Vcb->CompatFlags & UDF_VCB_IC_INSTANT_COMPAT_ALLOC_DESCS)) {
            AdPrint(("UDFPartLbaToPhys: try to recover: part %x -> %x\n",
                Addr->partitionReferenceNum, Vcb->PartitionMaps-1));
            Addr->partitionReferenceNum = (USHORT)(Vcb->PartitionMaps-1);
        } else {
            return LBA_OUT_OF_EXTENT;
        }
    }
    // walk through partition maps & transform relative address
    // to physical
    for(i=Addr->partitionReferenceNum; i<Vcb->PartitionMaps; i++) {
        if (Vcb->Partitions[i].PartitionNum == Addr->partitionReferenceNum) {
            a = Vcb->Partitions[i].PartitionRoot + Addr->logicalBlockNum;
            if (a > Vcb->LastPossibleLBA) {
                AdPrint(("UDFPartLbaToPhys: root %x, lbn %x, lba %x (err1)\n",
                    Vcb->Partitions[i].PartitionRoot, Addr->logicalBlockNum, a));
                BrutePoint();
                return LBA_OUT_OF_EXTENT;
            }
            return a;
        }
    }
    a = Vcb->Partitions[i-1].PartitionRoot + Addr->logicalBlockNum;

    if (a > Vcb->LastPossibleLBA) {
        AdPrint(("UDFPartLbaToPhys: i %x, root %x, lbn %x, lba %x (err2)\n",
            i, Vcb->Partitions[i-1].PartitionRoot, Addr->logicalBlockNum, a));
        BrutePoint();
        return LBA_OUT_OF_EXTENT;
    }
    return a;
} // end UDFPartLbaToPhys()


/*
    This routine returns physycal Lba for partition-relative addr
    No partition bounds check is performed.
    This routine only checks if requested partition exists.
    It is introduced for 'Adaptec DirectCD' compatibility,
    because it uses negative values as extent terminator (against standard)
 */
/*uint32
__fastcall
UDFPartLbaToPhysCompat(
  IN PVCB Vcb,
  IN lb_addr* Addr
  )
{
    uint32 i, a;
    if (Addr->partitionReferenceNum >= Vcb->PartitionMaps) return LBA_NOT_ALLOCATED;
    // walk through partition maps & transform relative address
    // to physical
    for(i=Addr->partitionReferenceNum; i<Vcb->PartitionMaps; i++) {
        if (Vcb->Partitions[i].PartitionNum == Addr->partitionReferenceNum) {
            a = Vcb->Partitions[i].PartitionRoot + Addr->logicalBlockNum;
            if (a > Vcb->LastPossibleLBA) {
                BrutePoint();
            }
            return a;
        }
    }
    a = Vcb->Partitions[i-1].PartitionRoot + Addr->logicalBlockNum;
    if (a > Vcb->LastPossibleLBA) {
        BrutePoint();
    }
    return a;
} // end UDFPartLbaToPhysCompat()*/


/*
    This routine looks for the partition containing given physical sector
 */
uint32
__fastcall
UDFGetRefPartNumByPhysLba(
    IN PVCB Vcb,
    IN uint32 Lba
    )
{
    uint32 i=Vcb->PartitionMaps-1, root;
    PUDFPartMap pm = &(Vcb->Partitions[i]);
    // walk through the partition maps to find suitable one
    for (; i != 0xffffffff; i--, pm--) {
        if ( ((root = pm->PartitionRoot) <= Lba) &&
             ((root + pm->PartitionLen) > Lba) )
            // Unsure if this is correct
            return (pm->PartitionNum >= Vcb->PartitionMaps ? i : (uint16)pm->PartitionNum);
    }
    return LBA_OUT_OF_EXTENT; // Lba doesn't belong to any partition
} // end UDFGetPartNumByPhysLba()

/*
    Very simple routine. It walks through the Partition Maps & returns
    the 1st Lba of the 1st suitable one
 */
uint32
__fastcall
UDFPartStart(
    PVCB Vcb,
    uint32 RefPartNum
    )
{
    uint32 i;
    if (RefPartNum == (uint32)-1) return 0;
    if (RefPartNum == (uint32)-2) return Vcb->Partitions[0].PartitionRoot;
    for (i = RefPartNum; i < Vcb->PartitionMaps; i++) {
        if (Vcb->Partitions[i].PartitionNum == UDFGetPartNumByPartRef(Vcb, RefPartNum))
            return Vcb->Partitions[i].PartitionRoot;
    }
    return 0;
} // end UDFPartStart(

/*
   This routine does almost the same as previous.
   The only difference is changing First Lba to Last one...
 */
uint32
__fastcall
UDFPartEnd(
    PVCB Vcb,
    uint32 RefPartNum
    )
{
    uint32 i;
    if (RefPartNum == (uint32)-1) return Vcb->LastLBA;
    if (RefPartNum == (uint32)-2) RefPartNum = Vcb->PartitionMaps-1;
    for(i=RefPartNum; i<Vcb->PartitionMaps; i++) {
        if (Vcb->Partitions[i].PartitionNum == UDFGetPartNumByPartRef(Vcb, RefPartNum))
            return (Vcb->Partitions[i].PartitionRoot +
                    Vcb->Partitions[i].PartitionLen);
    }
    return (Vcb->Partitions[i-1].PartitionRoot +
            Vcb->Partitions[i-1].PartitionLen);
} // end UDFPartEnd()

/*
    Very simple routine. It walks through the Partition Maps & returns
    the 1st Lba of the 1st suitable one
 */
uint32
__fastcall
UDFPartLen(
    PVCB Vcb, 
    uint32 RefPartNum
    )
{
    if (RefPartNum == (uint32)-2) return UDFPartEnd(Vcb, -2) - UDFPartStart(Vcb, -2);

    uint32 i;
    if (RefPartNum == (uint32)-1) return Vcb->LastLBA;
    for (i = RefPartNum; i < Vcb->PartitionMaps; i++) {
        if (Vcb->Partitions[i].PartitionNum == UDFGetPartNumByPartRef(Vcb, RefPartNum))
            return Vcb->Partitions[i].PartitionLen;
    }
    return (Vcb->Partitions[i-1].PartitionRoot +
            Vcb->Partitions[i-1].PartitionLen);
} // end UDFPartLen()

/*
    This routine returns length of bit-chain starting from Offs bit in
    array Bitmap. Bitmap scan is limited with Lim.
 */
SIZE_T
UDFGetBitmapLen(
    uint32* Bitmap,
    SIZE_T Offs,
    SIZE_T Lim          // NOT included
    )
{
    ASSERT(Offs <= Lim);
    if (Offs >= Lim) {
        return 0;//(Offs == Lim);
    }

    BOOLEAN bit = UDFGetBit(Bitmap, Offs);
    SIZE_T i=Offs>>5;
    SIZE_T len=0;
    uint8 j=(uint8)(Offs&31);
    uint8 lLim=(uint8)(Lim&31);

    Lim = Lim>>5;

    ASSERT((bit == 0) || (bit == 1));

    uint32 a;

    a = Bitmap[i] >> j;

    while(i<=Lim) {

        while( j < ((i<Lim) ? 32 : lLim) ) {
            if ( ((BOOLEAN)(a&1)) != bit)
                return len;
            len++;
            a>>=1;
            j++;
        }
        j=0;
While_3:
        i++;
        a = Bitmap[i];

        if (i<Lim) {
            if ((bit && (a==0xffffffff)) ||
               (!bit && !a)) {
                len+=32;
                goto While_3;
            }
        }
    }
    return len;
} // end UDFGetBitmapLen()

/*
    This routine scans disc free space Bitmap for minimal suitable extent.
    It returns maximal available extent if no long enough extents found.
 */
SIZE_T
UDFFindMinSuitableExtent(
    IN PVCB Vcb,
    IN uint32 Length, // in blocks
    IN uint32 SearchStart,
    IN uint32 SearchLim,    // NOT included
    OUT uint32* MaxExtLen,
    IN uint8  AllocFlags
    )
{
    SIZE_T i, len;
    uint32* cur;
    SIZE_T best_lba=0;
    SIZE_T best_len=0;
    SIZE_T max_lba=0;
    SIZE_T max_len=0;
    BOOLEAN align = FALSE;
    SIZE_T PS = Vcb->WriteBlockSize >> Vcb->SectorShift;

    UDF_CHECK_BITMAP_RESOURCE(Vcb);

    // we'll try to allocate packet-aligned block at first
    if (!(Length & (PS-1)) && !Vcb->CDR_Mode && (Length >= PS*2))
        align = TRUE;
    if (AllocFlags & EXTENT_FLAG_ALLOC_SEQUENTIAL)
        align = TRUE;
    if (Length > (uint32)(UDF_EXTENT_LENGTH_MASK >> Vcb->SectorShift))
        Length = (UDF_EXTENT_LENGTH_MASK >> Vcb->SectorShift);

    cur = (uint32*)(Vcb->FSBM_Bitmap);

retry_no_align:

    i=SearchStart;
    // scan Bitmap
    while(i<SearchLim) {
        ASSERT(i <= SearchLim);
        if (align) {
            i = (i+PS-1) & ~(PS-1);
            // we can't find suitable Packet-size aligned block
            // the block will be found without any alignment at the next iteration
            // ASSERT(i <= SearchLim);
            if (i >= SearchLim)
                break;
        }
        len = UDFGetBitmapLen(cur, i, SearchLim);
        if (UDFGetFreeBit(cur, i)) { // is the extent found free or used ?
            // wow! it is free!
            if (len >= Length) {
                // minimize extent length
                if (!best_len || (best_len > len)) {
                    best_lba = i;
                    best_len = len;
                }
                if (len == Length)
                    break;
            } else {
                // remember max extent
                if (max_len < len) {
                    max_lba = i;
                    max_len = len;
                }
            }
            // if this is CD-R mode, we should not think about fragmentation
            // due to CD-R nature file will be fragmented in any case
            if (Vcb->CDR_Mode) break;
        }
        i += len;
    }
    // if we can't find suitable Packet-size aligned block,
    // retry without any alignment requirements
    if (!best_len && align) {
        align = FALSE;
        goto retry_no_align;
    }
    if (best_len) {
        // minimal suitable block
        (*MaxExtLen) = best_len;
        return best_lba;
    }
    // maximal available
    (*MaxExtLen) = max_len;
    return max_lba;
} // end UDFFindMinSuitableExtent()

#ifdef UDF_CHECK_DISK_ALLOCATION
/*
    This routine checks space described by Mapping as Used/Freed (optionaly)
 */
void
UDFCheckSpaceAllocation_(
    IN PVCB Vcb,
    IN PEXTENT_MAP Map,
    IN uint32 asXXX
#ifdef UDF_TRACK_ONDISK_ALLOCATION
   ,IN uint32 FE_lba,
    IN uint32 BugCheckId,
    IN uint32 Line
#endif //UDF_TRACK_ONDISK_ALLOCATION
    )
{
    uint32 i=0;
    uint32 lba, j, len, BS, BSh;
    BOOLEAN asUsed = (asXXX == AS_USED);

    if (!Map) return;

    BS = Vcb->BlockSize;
    BSh = Vcb->BlockSizeBits;

    UDFAcquireResourceShared(&(Vcb->BitMapResource1),TRUE);
    // walk through all frags in data area specified
#ifdef UDF_TRACK_ONDISK_ALLOCATION
    AdPrint(("ChkAlloc:Map:%x:File:%x:Line:%d\n",
        Map,
        BugCheckId,
        Line
        ));
#endif //UDF_TRACK_ONDISK_ALLOCATION
    while(Map[i].extLength & UDF_EXTENT_LENGTH_MASK) {

#ifdef UDF_TRACK_ONDISK_ALLOCATION
        AdPrint(("ChkAlloc:%x:%s:%x:@:%x:(%x):File:%x:Line:%d\n",
            FE_lba,
            asUsed ? "U" : "F",
            (Map[i].extLength & UDF_EXTENT_LENGTH_MASK) >> BSh,
            Map[i].extLocation,
            (Map[i].extLength >> 30),
            BugCheckId,
            Line
            ));
#endif //UDF_TRACK_ONDISK_ALLOCATION
        if (asUsed) {
            UDFCheckUsedBitOwner(Vcb, (Map[i].extLength & UDF_EXTENT_LENGTH_MASK) >> BSh, FE_lba);
        } else {
            UDFCheckFreeBitOwner(Vcb, (Map[i].extLength & UDF_EXTENT_LENGTH_MASK) >> BSh);
        }

        if ((Map[i].extLength >> 30) == EXTENT_NOT_RECORDED_NOT_ALLOCATED) {
            // skip unallocated frags
//            ASSERT(!(Map[i].extLength & UDF_EXTENT_LENGTH_MASK));
            ASSERT(!Map[i].extLocation);
            i++;
            continue;
        } else {
//            ASSERT(!(Map[i].extLength & UDF_EXTENT_LENGTH_MASK));
            ASSERT(Map[i].extLocation);
        }

#ifdef UDF_CHECK_EXTENT_SIZE_ALIGNMENT
        ASSERT(!(Map[i].extLength & (BS-1)));
#endif //UDF_CHECK_EXTENT_SIZE_ALIGNMENT
        len = ((Map[i].extLength & UDF_EXTENT_LENGTH_MASK)+BS-1) >> BSh;
        lba = Map[i].extLocation;
        if ((lba+len) > Vcb->LastPossibleLBA) {
            // skip blocks beyond media boundary
            if (lba > Vcb->LastPossibleLBA) {
                ASSERT(FALSE);
                i++;
                continue;
            }
            len = Vcb->LastPossibleLBA - lba;
        }

        // mark frag as XXX (see asUsed parameter)
        if (asUsed) {

            ASSERT(len);
            for(j=0;j<len;j++) {
                if (lba+j > Vcb->LastPossibleLBA) {
                    BrutePoint();
                    AdPrint(("USED Mapping covers block(s) beyond media @%x\n",lba+j));
                    break;
                }
                if (!UDFGetUsedBit(Vcb->FSBM_Bitmap, lba+j)) {
                    BrutePoint();
                    AdPrint(("USED Mapping covers FREE block(s) @%x\n",lba+j));
                    break;
                }
            }

        } else {

            ASSERT(len);
            for(j=0;j<len;j++) {
                if (lba+j > Vcb->LastPossibleLBA) {
                    BrutePoint();
                    AdPrint(("USED Mapping covers block(s) beyond media @%x\n",lba+j));
                    break;
                }
                if (!UDFGetFreeBit(Vcb->FSBM_Bitmap, lba+j)) {
                    BrutePoint();
                    AdPrint(("FREE Mapping covers USED block(s) @%x\n",lba+j));
                    break;
                }
            }
        }

        i++;
    }
    UDFReleaseResource(&(Vcb->BitMapResource1));
} // end UDFCheckSpaceAllocation_()
#endif //UDF_CHECK_DISK_ALLOCATION

void
UDFMarkBadSpaceAsUsed(
    IN PVCB Vcb,
    IN lba_t lba,
    IN ULONG len
    )
{
    uint32 j;
#define BIT_C   (sizeof(Vcb->BSBM_Bitmap[0])*8)
    len = (lba+len+BIT_C-1)/BIT_C;
    if (Vcb->BSBM_Bitmap) {
        for(j=lba/BIT_C; j<len; j++) {
            Vcb->FSBM_Bitmap[j] &= ~Vcb->BSBM_Bitmap[j];
        }
    }
#undef BIT_C
} // UDFMarkBadSpaceAsUsed()

/*
    This routine marks space described by Mapping as Used/Freed (optionaly)
 */
void
UDFMarkSpaceAsXXXNoProtect_(
    IN PVCB Vcb,
    IN PEXTENT_MAP Map,
    IN uint32 asXXX
#ifdef UDF_TRACK_ONDISK_ALLOCATION
   ,IN uint32 FE_lba,
    IN uint32 BugCheckId,
    IN uint32 Line
#endif 
    )
{
    uint32 i = 0;
    uint32 lba, j, len, BS, BSh;
    BOOLEAN asUsed = (asXXX == AS_USED || (asXXX & AS_BAD));
    
    // Low-Memory variables for Cache Manager
    PBCB Bcb;
    PVOID WindowBuffer;
    LARGE_INTEGER Offset;

    UDF_CHECK_BITMAP_RESOURCE(Vcb);

    if (!Map) return;

    BS = Vcb->SectorSize;
    BSh = Vcb->SectorShift;
    Vcb->BitmapModified = TRUE;
    UDFSetModified(Vcb);

    while (Map[i].extLength & UDF_EXTENT_LENGTH_MASK) {
        if ((Map[i].extLength >> 30) == EXTENT_NOT_RECORDED_NOT_ALLOCATED) {
            i++;
            continue;
        }
        
        // FIX: Use ULONGLONG for length math to prevent 256GB overflow
        ULONGLONG safeLen = ((ULONGLONG)(Map[i].extLength & UDF_EXTENT_LENGTH_MASK) + BS - 1) >> BSh;
        len = (uint32)safeLen;
        lba = Map[i].extLocation;

        if (((ULONGLONG)lba + len) > (ULONGLONG)Vcb->LastPossibleLBA + 1) {
            if (lba > Vcb->LastPossibleLBA) { i++; continue; }
            len = (uint32)((ULONGLONG)Vcb->LastPossibleLBA + 1 - lba);
        }

        // FIX: Windowed Read-Modify-Write via Cache Manager
        // This loop iterates through the extent and maps 4KB bitmap pages as needed.
        for (j = 0; j < len; j++) {
            uint32 currentLba = lba + j;
            
            // Calculate byte offset in the bitmap file for this bit
            Offset.QuadPart = (ULONGLONG)(currentLba >> 3) & ~((ULONGLONG)BS - 1);

            // Map the 2KB/4KB sector containing our target bit
            // This replaces the 64MB global buffer access
            if (CcMapData(Vcb->BitmapFileObject, &Offset, BS, TRUE, &Bcb, &WindowBuffer)) {
                
                // Calculate bit offset within the pinned window
                uint32 bitInWindow = (uint32)((ULONGLONG)currentLba - (Offset.QuadPart << 3));
                
                if (asUsed) {
                    UDFSetBit(WindowBuffer, bitInWindow);
                } else {
                    UDFClrBit(WindowBuffer, bitInWindow);
                }

                // Signal the Cache Manager to flush this page back to disk later
                CcSetDirtyPinnedData(Bcb, NULL);
                CcUnpinData(Bcb);
            }
        }

        if (!asUsed) {
            if (asXXX & AS_DISCARDED) {
                UDFUnmapRange(Vcb, lba, len);
                // Note: ZSBM_Bitmap should also be converted to CcMapData if it is large
            }
            Map[i].extLength = ((ULONGLONG)len << BSh) | ((ULONGLONG)EXTENT_NOT_RECORDED_NOT_ALLOCATED << 30);
            Map[i].extLocation = 0;
        }

        i++;
    }
} // end UDFMarkSpaceAsXXXNoProtect_()

/*
    This routine marks space described by Mapping as Used/Freed (optionaly)
    It protects data with sync Resource
 */
void
UDFMarkSpaceAsXXX_(
    IN PVCB Vcb,
    IN PEXTENT_MAP Map,
    IN uint32 asXXX
#ifdef UDF_TRACK_ONDISK_ALLOCATION
   ,IN uint32 FE_lba,
    IN uint32 BugCheckId,
    IN uint32 Line
#endif //UDF_TRACK_ONDISK_ALLOCATION
    )
{
    if (!Map) return;
    if (!Map[0].extLength) {
#ifdef UDF_DBG
        ASSERT(!Map[0].extLocation);
#endif // UDF_DBG
        return;
    }

    UDFAcquireResourceExclusive(&(Vcb->BitMapResource1),TRUE);
#ifdef UDF_TRACK_ONDISK_ALLOCATION
    UDFMarkSpaceAsXXXNoProtect_(Vcb, Map, asXXX, FE_lba, BugCheckId, Line);
#else //UDF_TRACK_ONDISK_ALLOCATION
    UDFMarkSpaceAsXXXNoProtect_(Vcb, Map, asXXX);
#endif //UDF_TRACK_ONDISK_ALLOCATION
    UDFReleaseResource(&(Vcb->BitMapResource1));

} // end UDFMarkSpaceAsXXX_()

/*
    This routine builds mapping for Length bytes in FreeSpace
    It should be used when IN_ICB method is unavailable.
 */
NTSTATUS
UDFAllocFreeExtent_(
    IN PIRP_CONTEXT IrpContext,
    IN PVCB   Vcb,
    IN int64  Length,
    IN uint32 SearchStart,
    IN uint32 SearchLim,     // NOT included
    OUT PEXTENT_INFO ExtInfo,
    IN uint8  AllocFlags
#ifdef UDF_TRACK_ALLOC_FREE_EXTENT
   ,IN uint32 src,
    IN uint32 line
#endif //UDF_TRACK_ALLOC_FREE_EXTENT
    )
{
    EXTENT_AD Ext;
    PEXTENT_MAP Map = NULL;
    uint32 len, LBS, BSh, blen;

    LBS = Vcb->SectorSize;
    BSh = Vcb->SectorShift;
    uint32 MaxExtentLength = ALIGN_DOWN_BY(UDF_EXTENT_LENGTH_MASK, LBS);
    blen = (uint32)(((Length+LBS-1) & ~((int64)LBS-1)) >> BSh);
    ExtInfo->Mapping = NULL;
    ExtInfo->Offset = 0;

    ASSERT(blen <= (uint32)(MaxExtentLength >> BSh));

    UDFAcquireResourceExclusive(&(Vcb->BitMapResource1),TRUE);

    if (blen > (SearchLim - SearchStart)) {
        goto no_free_space_err;
    }
    // walk through the free space bitmap & find a single extent or a set of
    // frags giving in sum the Length specified
    while(blen) {
        Ext.extLocation = UDFFindMinSuitableExtent(Vcb, blen, SearchStart,
                                                               SearchLim, &len, AllocFlags);

        if (len >= blen) {
            // complete search
            Ext.extLength = blen<<BSh;
            blen = 0;
        } else if (len) {
            // we need still some frags to complete request &
            // probably we have the opportunity to do it
            Ext.extLength = len<<BSh;
            blen -= len;
        } else {
no_free_space_err:
            // no more free space. abort
            if (ExtInfo->Mapping) {
                UDFMarkSpaceAsXXXNoProtect(Vcb, 0, ExtInfo->Mapping, AS_DISCARDED); // free
                MyFreePool__(ExtInfo->Mapping);
                ExtInfo->Mapping = NULL;
            }
            UDFReleaseResource(&(Vcb->BitMapResource1));
            ExtInfo->Length = 0;//UDFGetExtentLength(ExtInfo->Mapping);
            AdPrint(("  DISK_FULL\n"));
            return STATUS_DISK_FULL;
        }
        // append the frag found to mapping
        ASSERT(!(Ext.extLength >> 30));
        ASSERT(Ext.extLocation);

        if (AllocFlags & EXTENT_FLAG_VERIFY) {
            if (!UDFCheckArea(IrpContext, Vcb, Ext.extLocation, Ext.extLength >> BSh)) {
                AdPrint(("newly allocated extent contains BB\n"));
                UDFMarkSpaceAsXXXNoProtect(Vcb, 0, ExtInfo->Mapping, AS_DISCARDED); // free
                UDFMarkBadSpaceAsUsed(Vcb, Ext.extLocation, Ext.extLength >> BSh); // bad -> bad+used
                // roll back
                blen += Ext.extLength>>BSh;
                continue;
            }
        }

        Ext.extLength |= EXTENT_NOT_RECORDED_ALLOCATED << 30;
        if (!(ExtInfo->Mapping)) {
            // create new
#ifdef UDF_TRACK_ALLOC_FREE_EXTENT
            ExtInfo->Mapping = UDFExtentToMapping_(&Ext, src, line);
#else // UDF_TRACK_ALLOC_FREE_EXTENT
            ExtInfo->Mapping = UDFExtentToMapping(&Ext);
#endif // UDF_TRACK_ALLOC_FREE_EXTENT
            if (!ExtInfo->Mapping) {
                BrutePoint();
                UDFReleaseResource(&(Vcb->BitMapResource1));
                ExtInfo->Length = 0;
                return STATUS_INSUFFICIENT_RESOURCES;
            }
            UDFMarkSpaceAsXXXNoProtect(Vcb, 0, ExtInfo->Mapping, AS_USED); // used
        } else {
            // update existing
            Map = UDFExtentToMapping(&Ext);
            if (!Map) {
                BrutePoint();
                UDFReleaseResource(&(Vcb->BitMapResource1));
                ExtInfo->Length = UDFGetExtentLength(ExtInfo->Mapping);
                return STATUS_INSUFFICIENT_RESOURCES;
            }
            UDFMarkSpaceAsXXXNoProtect(Vcb, 0, Map, AS_USED); // used
            ExtInfo->Mapping = UDFMergeMappings(ExtInfo->Mapping, Map);
            MyFreePool__(Map);
        }
        if (!ExtInfo->Mapping) {
            BrutePoint();
            UDFReleaseResource(&(Vcb->BitMapResource1));
            ExtInfo->Length = 0;
            return STATUS_INSUFFICIENT_RESOURCES;
        }
    }
    UDFReleaseResource(&(Vcb->BitMapResource1));
    ExtInfo->Length = Length;
    return STATUS_SUCCESS;
} // end UDFAllocFreeExtent_()

/*
    Returns block-count
 */
uint32
__fastcall
UDFGetPartFreeSpace(
    IN PVCB Vcb,
    IN uint32 partNum
    )
{
    uint32 lim/*, len=1*/;
    uint32 s=0;
    uint32 j;
    PUCHAR cur = (PUCHAR)(Vcb->FSBM_Bitmap);

    lim = (UDFPartEnd(Vcb,partNum)+7)/8;
    for(j=(UDFPartStart(Vcb,partNum)+7)/8; j<lim/* && len*/; j++) {
        s+=bit_count_tab[cur[j]];
    }
    return s;
} // end UDFGetPartFreeSpace()

int64
__fastcall
UDFGetFreeSpace(
    IN PVCB Vcb
    )
{
    int64 s=0;
    uint32 i;
//    uint32* cur = (uint32*)(Vcb->FSBM_Bitmap);

    if (!Vcb->CDR_Mode) {
        for(i=0;i<Vcb->PartitionMaps;i++) {
/*            lim = UDFPartEnd(Vcb,i);
            for(j=UDFPartStart(Vcb,i); j<lim && len; ) {
                len = UDFGetBitmapLen(cur, j, lim);
                if (UDFGetFreeBit(cur, j)) // is the extent found free or used ?
                    s+=len;
                j+=len;
            }*/
            s += UDFGetPartFreeSpace(Vcb, i);
        }
    } else {
        ASSERT(Vcb->LastPossibleLBA >= max(Vcb->NWA, Vcb->LastLBA));
        s = Vcb->LastPossibleLBA - max(Vcb->NWA, Vcb->LastLBA);
        //if (s & ((int64)1 << 64)) s=0;
    }
    return s;
} // end UDFGetFreeSpace()

/*
    Returns block-count
 */
int64
UDFGetTotalSpace(
    IN PVCB Vcb
    )
{
    int64 s=0;
    uint32 i;

    if (!Vcb->CDR_Mode) {
        for(i=0;i<Vcb->PartitionMaps;i++) {
            s+=Vcb->Partitions[i].PartitionLen;
        }
    } else {
        if (s & ((int64)1 << 63)) s=0;  /* FIXME ReactOS this shift value was 64, which is undefiened behavior. */
        s= Vcb->LastPossibleLBA - Vcb->Partitions[0].PartitionRoot;
    }
    return s;
} // end UDFGetTotalSpace()
