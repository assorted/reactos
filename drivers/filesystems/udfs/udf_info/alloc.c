////////////////////////////////////////////////////////////////////
// Copyright (C) Alexander Telyatnikov, Ivan Keliukh, Yegor Anchishkin, SKIF Software, 1999-2013. Kiev, Ukraine
// All rights reserved
// This file was released under the GPLv2 on June 2015.
////////////////////////////////////////////////////////////////////
/*
        Module name:

   alloc.c

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
    RtlFindNextForwardRunSet — not exported by ReactOS ntoskrnl,
    but statically linked into MS udfs.sys. Same algorithm here.
    Finds the next run of set (1) bits starting at or after FromIndex.
 */
static
ULONG
UDFBitmapFindNextRunSet(
    IN PRTL_BITMAP BitMapHeader,
    IN ULONG FromIndex,
    OUT PULONG StartingRunIndex
    )
{
    ULONG Size = BitMapHeader->SizeOfBitMap;
    PULONG Buf = BitMapHeader->Buffer;
    ULONG Idx = FromIndex;

    if (Size == 0) {
        *StartingRunIndex = Idx;
        return 0;
    }

    ULONG LastWord = (Size - 1) >> 5;

    // Phase 1: skip clear bits, find first set bit
    // Skip partial first word
    ULONG Word = Idx >> 5;
    if ((Idx & 31) && Word <= LastWord) {
        // Mask off bits below Idx within this word
        ULONG Val = Buf[Word] & ~((1UL << (Idx & 31)) - 1);
        if (Val) {
            // Found a set bit — count trailing zeros
            Idx = (Word << 5);
            while (!(Val & 1)) { Val >>= 1; Idx++; }
            goto FoundStart;
        }
        Idx = (Word + 1) << 5;
        Word++;
    }
    // Skip whole zero words
    while (Word <= LastWord && Buf[Word] == 0) { Word++; Idx += 32; }
    // Scan within the non-zero word
    if (Word <= LastWord) {
        ULONG Val = Buf[Word];
        Idx = Word << 5;
        while (!(Val & 1)) { Val >>= 1; Idx++; }
    }

FoundStart:
    if (Idx >= Size) {
        *StartingRunIndex = Size;
        return 0;
    }
    *StartingRunIndex = Idx;

    // Phase 2: count consecutive set bits
    ULONG End = Idx;
    Word = End >> 5;
    // Check partial first word
    if ((End & 31) && Word <= LastWord) {
        ULONG Val = Buf[Word] >> (End & 31);
        while ((End & 31) && (Val & 1)) { Val >>= 1; End++; }
        if (End & 31) goto Done;  // Run ended mid-word
        Word++;
    }
    // Skip whole 0xFFFFFFFF words
    while (Word <= LastWord && Buf[Word] == 0xFFFFFFFF) { Word++; End += 32; }
    // Scan partial word at run end
    if (Word <= LastWord) {
        ULONG Val = Buf[Word];
        while ((Val & 1)) { Val >>= 1; End++; }
    }

Done:
    if (End > Size) End = Size;
    return End - Idx;
}

/*
    Pin the bitmap cache page containing the given LBN (BitIndex).
    Initializes RTL_BITMAP on the pinned data for bounds-safe access.
    Stores result in Vcb->BitmapRtl, BitmapPageStartLbn, BitmapPageBitCount.
 */
VOID
UDFPinBitmapPage(
    IN PVCB Vcb,
    IN ULONG Lbn
    )
{
    // Calculate stream byte offset for this LBN
    ULONG streamOffset = Vcb->BitmapDataOffset + (Lbn >> 3);
    ULONG pinBase = streamOffset & ~(BITMAP_PIN_GRANULARITY - 1);

    if (Vcb->BitmapBcb && Vcb->BitmapPinnedOffset == pinBase) {
        // Already pinned at this offset — reuse
        return;
    }

    // Unpin current page if any
    if (Vcb->BitmapBcb) {
        CcUnpinData(Vcb->BitmapBcb);
        Vcb->BitmapBcb = NULL;
    }

    // Calculate pin length
    ULONG allocEnd = (ULONG)Vcb->BitmapFcb->Header.AllocationSize.QuadPart;
    ULONG pinEnd = min(pinBase + BITMAP_PIN_GRANULARITY, allocEnd);
    ULONG pinLength = pinEnd - pinBase;

    LARGE_INTEGER offset;
    offset.QuadPart = pinBase;
    PVOID buffer;

    CcPinRead(Vcb->BitmapStreamFileObject,
              &offset,
              pinLength,
              TRUE,
              &Vcb->BitmapBcb,
              &buffer);

    Vcb->BitmapPinnedData = (PUCHAR)buffer;
    Vcb->BitmapPinnedOffset = pinBase;
    Vcb->BitmapPinnedLength = pinLength;

    // Initialize RTL_BITMAP on the bitmap bit data within this pinned region.
    // The bitmap stream layout: [SBD header (BitmapDataOffset bytes)] [bit data...]
    // On page 0, skip the SBD header; on other pages, data starts at pinBase.
    ULONG dataStart = max(Vcb->BitmapDataOffset, pinBase);
    ULONG dataEnd = min(pinBase + pinLength,
                        Vcb->BitmapDataOffset + Vcb->FSBM_ByteCount);

    PULONG bitmapBits = (PULONG)(Vcb->BitmapPinnedData + (dataStart - pinBase));
    ULONG startLbn = (dataStart - Vcb->BitmapDataOffset) << 3;
    ULONG bitCount = (dataEnd - dataStart) << 3;

    // Clamp to total bitmap size
    if (startLbn + bitCount > Vcb->FSBM_BitCount) {
        bitCount = Vcb->FSBM_BitCount - startLbn;
    }

    RtlInitializeBitMap(&Vcb->BitmapRtl, bitmapBits, bitCount);
    Vcb->BitmapPageStartLbn = startLbn;
    Vcb->BitmapPageBitCount = bitCount;
}

VOID
UDFUnpinBitmapPage(
    IN PVCB Vcb
    )
{
    if (Vcb->BitmapBcb) {
        CcUnpinData(Vcb->BitmapBcb);
        Vcb->BitmapBcb = NULL;
        Vcb->BitmapPinnedData = NULL;
    }
}

VOID
UDFDirtyBitmapPage(
    IN PVCB Vcb
    )
{
    if (Vcb->BitmapBcb) {
        CcSetDirtyPinnedData(Vcb->BitmapBcb, NULL);
    }
}

/*
    Check if a single bitmap bit is free (1 = free in UDF).
    Pins the relevant page internally.
 */
BOOLEAN
UDFIsBitmapBitFree(
    IN PVCB Vcb,
    IN ULONG Lbn
    )
{
    UDFPinBitmapPage(Vcb, Lbn);
    ULONG idx = Lbn - Vcb->BitmapPageStartLbn;
    return RtlCheckBit(&Vcb->BitmapRtl, idx) ? TRUE : FALSE;
}

/*
    Count consecutive free (set) bits starting from Start LBN, up to Limit.
    Uses RTL_BITMAP across pinned page boundaries.
 */
SIZE_T
UDFGetCachedBitmapLen(
    IN PVCB Vcb,
    IN ULONG Start,
    IN ULONG Limit
    )
{
    if (Start >= Limit) return 0;

    SIZE_T totalLen = 0;
    ULONG pos = Start;

    while (pos < Limit) {
        UDFPinBitmapPage(Vcb, pos);

        ULONG localIdx = pos - Vcb->BitmapPageStartLbn;
        ULONG pageEnd = min(Vcb->BitmapPageStartLbn + Vcb->BitmapPageBitCount, Limit);

        // Find run of set (free) bits starting at localIdx
        ULONG runStartIdx;
        ULONG runLen = UDFBitmapFindNextRunSet(&Vcb->BitmapRtl, localIdx, &runStartIdx);

        // If the run doesn't start at our position, the bit at pos is not free
        if (runLen == 0 || runStartIdx != localIdx) {
            break;
        }

        // Clamp to page/limit boundary
        ULONG maxBits = pageEnd - pos;
        if (runLen > maxBits) runLen = maxBits;

        totalLen += runLen;
        pos += runLen;

        // If run ended before page boundary, the next bit is used — stop
        if (pos < pageEnd) break;
    }

    return totalLen;
}

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
            if (Addr->logicalBlockNum >= Vcb->Partitions[i].PartitionLen) {
                AdPrint(("UDFPartLbaToPhys: root %x, lbn %x, plen %x (err1)\n",
                    Vcb->Partitions[i].PartitionRoot, Addr->logicalBlockNum,
                    Vcb->Partitions[i].PartitionLen));
                BrutePoint();
                return LBA_OUT_OF_EXTENT;
            }
            a = Vcb->Partitions[i].PartitionRoot + Addr->logicalBlockNum;
            return a;
        }
    }
    if (Addr->logicalBlockNum >= Vcb->Partitions[i-1].PartitionLen) {
        AdPrint(("UDFPartLbaToPhys: i %x, root %x, lbn %x, plen %x (err2)\n",
            i, Vcb->Partitions[i-1].PartitionRoot, Addr->logicalBlockNum,
            Vcb->Partitions[i-1].PartitionLen));
        BrutePoint();
        return LBA_OUT_OF_EXTENT;
    }
    a = Vcb->Partitions[i-1].PartitionRoot + Addr->logicalBlockNum;
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
    if (RefPartNum == (uint32)-1) return Vcb->SessionEndLba;
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
    if (RefPartNum == (uint32)-1) return Vcb->SessionEndLba;
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
        if (i > Lim) break;
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
    IN uint32 SearchStart,  // PSN
    IN uint32 SearchLim,    // PSN, NOT included
    OUT uint32* MaxExtLen,
    IN uint8  AllocFlags
    )
{
    SIZE_T i, len;
    SIZE_T best_lba=0;
    SIZE_T best_len=0;
    SIZE_T max_lba=0;
    SIZE_T max_len=0;

    // Convert PSN search range to LBN for bitmap access
    uint32 partRoot = Vcb->Partitions[0].PartitionRoot;
    uint32 lbnStart = SearchStart - partRoot;
    uint32 lbnLim = SearchLim - partRoot;

    UDF_CHECK_BITMAP_RESOURCE(Vcb);

    if (Length > (uint32)(UDF_EXTENT_LENGTH_MASK >> Vcb->SectorShift))
        Length = (UDF_EXTENT_LENGTH_MASK >> Vcb->SectorShift);

    i=lbnStart;
    if (Vcb->BitmapFcb) {
        // Per-page scanning using RTL_BITMAP with cross-page run tracking
        ULONG CurrentRunStart = 0;
        ULONG CurrentRunLength = 0;
        ULONG CurrentLbn = (ULONG)lbnStart;

        while (CurrentLbn < lbnLim) {
            UDFPinBitmapPage(Vcb, CurrentLbn);

            ULONG pageStart = Vcb->BitmapPageStartLbn;
            ULONG pageBits = Vcb->BitmapPageBitCount;

            // Calculate local index within this page's RTL_BITMAP
            ULONG fromIndex = CurrentLbn - pageStart;

            // Find next run of set bits (free blocks) starting from fromIndex
            ULONG runStartIndex;
            ULONG runLen = UDFBitmapFindNextRunSet(&Vcb->BitmapRtl, fromIndex, &runStartIndex);

            // Convert to absolute LBN
            ULONG runStartLbn = runStartIndex + pageStart;

            if (CurrentRunLength != 0) {
                // We have an active run — check if this extends it
                if (runLen == 0 || runStartLbn != CurrentLbn) {
                    // Active run ended — evaluate it
                    if (CurrentRunLength >= Length) {
                        if (!best_len || (best_len > CurrentRunLength)) {
                            best_lba = CurrentRunStart;
                            best_len = CurrentRunLength;
                        }
                    } else if (max_len < CurrentRunLength) {
                        max_lba = CurrentRunStart;
                        max_len = CurrentRunLength;
                    }
                    if (Vcb->CDR_Mode && (best_len || max_len)) break;

                    // Start new run if we found free blocks
                    CurrentRunLength = runLen;
                    CurrentRunStart = runStartLbn;
                } else {
                    // Extends current run
                    CurrentRunLength += runLen;
                }
            } else {
                // No active run — start new one if found
                if (runLen != 0) {
                    CurrentRunLength = runLen;
                    CurrentRunStart = runStartLbn;
                }
            }

            // Check early exit
            if (best_len == Length) break;

            // Advance to next position
            if (runLen == 0) {
                // No free run found on this page — skip to next page
                CurrentLbn = pageStart + pageBits;
            } else {
                CurrentLbn = CurrentRunStart + CurrentRunLength;
            }

            if (CurrentLbn > lbnLim) CurrentLbn = (ULONG)lbnLim;
        }

        // Final run evaluation
        if (CurrentRunLength != 0) {
            if (CurrentRunLength >= Length) {
                if (!best_len || (best_len > CurrentRunLength)) {
                    best_lba = CurrentRunStart;
                    best_len = CurrentRunLength;
                }
            } else if (max_len < CurrentRunLength) {
                max_lba = CurrentRunStart;
                max_len = CurrentRunLength;
            }
        }
    } else {
    // Legacy in-memory bitmap path
    while(i<lbnLim) {
        ASSERT(i <= lbnLim);
        len = UDFGetBitmapLen((uint32*)(Vcb->FSBM_Bitmap), i, lbnLim);
        if (UDFGetFreeBit((uint32*)(Vcb->FSBM_Bitmap), i)) {
            // free extent found
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
            if (Vcb->CDR_Mode) break;
        }
        i += len;
    }
    } // end legacy path
    UDFUnpinBitmapPage(Vcb);
    if (!best_len && !max_len) {
        UDFPrint(("UDF BM: FindMinSuitable: NO FREE SPACE lbnStart=%x lbnLim=%x Length=%x BitCount=%x\n",
            (ULONG)lbnStart, (ULONG)lbnLim, Length, Vcb->FSBM_BitCount));
    }
    if (best_len) {
        // minimal suitable block
        (*MaxExtLen) = best_len;
        return best_lba + partRoot;  // convert LBN back to PSN
    }
    // maximal available
    (*MaxExtLen) = max_len;
    return max_lba + partRoot;  // convert LBN back to PSN
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
        if ((lba+len) > Vcb->FSBM_BitCount) {
            // skip blocks beyond bitmap boundary
            if (lba >= Vcb->FSBM_BitCount) {
                ASSERT(FALSE);
                i++;
                continue;
            }
            len = Vcb->FSBM_BitCount - lba;
        }

        // Convert PSN to LBN for bitmap access
        uint32 lbn = lba - Vcb->Partitions[0].PartitionRoot;

        // mark frag as XXX (see asUsed parameter)
        if (asUsed) {

            ASSERT(len);
            for(j=0;j<len;j++) {
                if (lba+j >= Vcb->FSBM_BitCount) {
                    BrutePoint();
                    AdPrint(("USED Mapping covers block(s) beyond bitmap @%x\n",lba+j));
                    break;
                }
                if (Vcb->BitmapFcb ? UDFIsBitmapBitFree(Vcb, lbn+j) : !UDFGetUsedBit(Vcb->FSBM_Bitmap, lbn+j)) {
                    BrutePoint();
                    AdPrint(("USED Mapping covers FREE block(s) @%x\n",lba+j));
                    break;
                }
            }

        } else {

            ASSERT(len);
            for(j=0;j<len;j++) {
                if (lba+j >= Vcb->FSBM_BitCount) {
                    BrutePoint();
                    AdPrint(("USED Mapping covers block(s) beyond bitmap @%x\n",lba+j));
                    break;
                }
                if (Vcb->BitmapFcb ? !UDFIsBitmapBitFree(Vcb, lbn+j) : !UDFGetFreeBit(Vcb->FSBM_Bitmap, lbn+j)) {
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
        if (Vcb->BitmapFcb) {
            // Per-page: AND bad-block mask into pinned bitmap data
            for(j=lba/BIT_C; j<len; j++) {
                if (Vcb->BSBM_Bitmap[j]) {
                    UDFPinBitmapPage(Vcb, j * BIT_C);
                    ULONG byteOff = (j * BIT_C - Vcb->BitmapPageStartLbn) / 8;
                    // Access raw pinned data for bad-block masking
                    PUCHAR rawData = (PUCHAR)Vcb->BitmapRtl.Buffer;
                    rawData[byteOff] &= ~Vcb->BSBM_Bitmap[j];
                    UDFDirtyBitmapPage(Vcb);
                }
            }
        } else {
            for(j=lba/BIT_C; j<len; j++) {
                Vcb->FSBM_Bitmap[j] &= ~Vcb->BSBM_Bitmap[j];
            }
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
#endif //UDF_TRACK_ONDISK_ALLOCATION
    )
{
    uint32 i=0;
    uint32 lba, j, len, BS, BSh;
    uint32 root;
    BOOLEAN asUsed = (asXXX == AS_USED || (asXXX & AS_BAD));
#ifdef UDF_TRACK_ONDISK_ALLOCATION
    BOOLEAN bit_before, bit_after;
#endif //UDF_TRACK_ONDISK_ALLOCATION

    UDF_CHECK_BITMAP_RESOURCE(Vcb);

    if (!Map) return;

    BS = Vcb->SectorSize;
    BSh = Vcb->SectorShift;
    Vcb->BitmapModified = TRUE;
    UDFSetModified(Vcb);
    uint32 partRoot = Vcb->Partitions[0].PartitionRoot;
    // walk through all frags in data area specified
    while(Map[i].extLength & UDF_EXTENT_LENGTH_MASK) {
        if ((Map[i].extLength >> 30) == EXTENT_NOT_RECORDED_NOT_ALLOCATED) {
            // skip unallocated frags
            i++;
            continue;
        }
        ASSERT(Map[i].extLocation);

#ifdef UDF_TRACK_ONDISK_ALLOCATION
        AdPrint(("Alloc:%x:%s:%x:@:%x:File:%x:Line:%d\n",
            FE_lba,
            asUsed ? ((asXXX & AS_BAD) ? "B" : "U") : "F",
            (Map[i].extLength & UDF_EXTENT_LENGTH_MASK) >> Vcb->BlockSizeBits,
            Map[i].extLocation,
            BugCheckId,
            Line
            ));
#endif //UDF_TRACK_ONDISK_ALLOCATION

#ifdef UDF_DBG
#ifdef UDF_CHECK_EXTENT_SIZE_ALIGNMENT
        ASSERT(!(Map[i].extLength & (BS-1)));
#endif //UDF_CHECK_EXTENT_SIZE_ALIGNMENT
//        len = ((Map[i].extLength & UDF_EXTENT_LENGTH_MASK)+BS-1) >> BSh;
#else // UDF_DBG
//        len = (Map[i].extLength & UDF_EXTENT_LENGTH_MASK) >> BSh;
#endif // UDF_DBG
        len = ((Map[i].extLength & UDF_EXTENT_LENGTH_MASK)+BS-1) >> BSh;
        lba = Map[i].extLocation;
        if ((lba+len) > Vcb->FSBM_BitCount) {
            // skip blocks beyond bitmap boundary
            if (lba >= Vcb->FSBM_BitCount) {
                ASSERT(FALSE);
                i++;
                continue;
            }
            len = Vcb->FSBM_BitCount - lba;
        }

        // Convert PSN to LBN for bitmap access
        uint32 lbn = lba - partRoot;

        // mark frag as XXX (see asUsed parameter)
        if (asUsed) {
            ASSERT(len);
            if (Vcb->BitmapFcb) {
                // Per-page: clear bits (used = 0) across page boundaries
                ULONG remaining = len;
                ULONG pos = lbn;
                while (remaining > 0) {
                    UDFPinBitmapPage(Vcb, pos);
                    ULONG localIdx = pos - Vcb->BitmapPageStartLbn;
                    ULONG bitsInPage = min(remaining, Vcb->BitmapPageBitCount - localIdx);
                    RtlClearBits(&Vcb->BitmapRtl, localIdx, bitsInPage);
                    UDFDirtyBitmapPage(Vcb);
                    pos += bitsInPage;
                    remaining -= bitsInPage;
                }
            } else {
                UDFSetUsedBits(Vcb->FSBM_Bitmap, lbn, len);
            }

            if (Vcb->Vat) {
                for(j=0;j<len;j++) {
                    root = UDFPartStart(Vcb, UDFGetRefPartNumByPhysLba(Vcb, lba));
                    if ((Vcb->Vat[lba-root+j] == UDF_VAT_FREE_ENTRY) &&
                       (lba > Vcb->SessionEndLba)) {
                         Vcb->Vat[lba-root+j] = 0x7fffffff;
                    }
                }
            }
        } else {
            ASSERT(len);
            if (Vcb->BitmapFcb) {
                // Per-page: set bits (free = 1) across page boundaries
                ULONG remaining = len;
                ULONG pos = lbn;
                while (remaining > 0) {
                    UDFPinBitmapPage(Vcb, pos);
                    ULONG localIdx = pos - Vcb->BitmapPageStartLbn;
                    ULONG bitsInPage = min(remaining, Vcb->BitmapPageBitCount - localIdx);
                    RtlSetBits(&Vcb->BitmapRtl, localIdx, bitsInPage);
                    UDFDirtyBitmapPage(Vcb);
                    pos += bitsInPage;
                    remaining -= bitsInPage;
                }
            } else {
                UDFSetFreeBits(Vcb->FSBM_Bitmap, lbn, len);
            }
            if (asXXX & AS_BAD) {
                UDFSetBits(Vcb->BSBM_Bitmap, lbn, len);
            }
            UDFMarkBadSpaceAsUsed(Vcb, lbn, len);

            if (asXXX & AS_DISCARDED) {
                UDFUnmapRange(Vcb, lba, len);
            }
            if (Vcb->Vat) {
                for(j=0;j<len;j++) {
                    root = UDFPartStart(Vcb, UDFGetRefPartNumByPhysLba(Vcb, lba));
                    Vcb->Vat[lba-root+j] = UDF_VAT_FREE_ENTRY;
                }
            }
            Map[i].extLength = (len << BSh) | (EXTENT_NOT_RECORDED_NOT_ALLOCATED << 30);
            Map[i].extLocation = 0;
        }

        i++;
    }

    UDFUnpinBitmapPage(Vcb);
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
            UDFPrint(("UDF BM: DISK_FULL blen=%x SearchStart=%x SearchLim=%x BitmapFcb=%p BitCount=%x\n",
                blen, SearchStart, SearchLim, Vcb->BitmapFcb, Vcb->FSBM_BitCount));
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
    uint32 s=0;

    if (Vcb->BitmapFcb) {
        // Per-page: iterate pinned pages, count free (set) bits via RTL_BITMAP
        ULONG pos = 0;
        while (pos < Vcb->FSBM_BitCount) {
            UDFPinBitmapPage(Vcb, pos);
            ULONG bits = Vcb->BitmapPageBitCount;
            ULONG startLbn = Vcb->BitmapPageStartLbn;
            ULONG bufWords = (bits + 31) / 32;
            ULONG dataBytes = (Vcb->BitmapRtl.SizeOfBitMap + 7) / 8;
            if (bufWords * 4 > Vcb->BitmapPinnedLength) {
                UDFPrint(("UDF BM: FreeSpace OVERFLOW pos=%x bits=%x need=%x pinLen=%x pinOff=%x\n",
                    pos, bits, bufWords * 4, Vcb->BitmapPinnedLength, Vcb->BitmapPinnedOffset));
                break;
            }
            s += RtlNumberOfSetBits(&Vcb->BitmapRtl);
            pos = startLbn + bits;
        }
        UDFUnpinBitmapPage(Vcb);
    } else {
        PUCHAR cur = (PUCHAR)(Vcb->FSBM_Bitmap);
        ULONG lim = (Vcb->FSBM_BitCount+7)/8;
        for(ULONG j=0; j<lim; j++) {
            s+=bit_count_tab[cur[j]];
        }
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
        if (Vcb->BitmapFcb) {
            UDFAcquireResourceShared(&(Vcb->BitMapResource1),TRUE);
        }
        for(i=0;i<Vcb->PartitionMaps;i++) {
            s += UDFGetPartFreeSpace(Vcb, i);
        }
        if (Vcb->BitmapFcb) {
            UDFReleaseResource(&(Vcb->BitMapResource1));
        }
    } else {
        ASSERT(Vcb->FSBM_BitCount >= max(Vcb->NWA, Vcb->SessionEndLba));
        s = Vcb->FSBM_BitCount - max(Vcb->NWA, Vcb->SessionEndLba);
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
        s = Vcb->Partitions[0].PartitionLen;
    }
    return s;
} // end UDFGetTotalSpace()
