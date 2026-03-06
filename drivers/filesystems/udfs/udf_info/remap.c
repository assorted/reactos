////////////////////////////////////////////////////////////////////
// Copyright (C) Alexander Telyatnikov, Ivan Keliukh, Yegor Anchishkin, SKIF Software, 1999-2013. Kiev, Ukraine
// All rights reserved
// This file was released under the GPLv2 on June 2015.
////////////////////////////////////////////////////////////////////
/*
        Module name:

   remap.cpp

        Abstract:

   This file contains filesystem-specific routines
   responsible for disk space management

*/

#include "udf.h"

#define         UDF_BUG_CHECK_ID                UDF_FILE_UDF_INFO_REMAP

BOOLEAN
__fastcall
UDFCheckArea(
    PIRP_CONTEXT IrpContext,
    IN PVCB Vcb,
    IN lba_t LBA,
    IN uint32 BCount
    )
{
    uint8* buff;
    NTSTATUS RC;
    uint32 i, d;
    BOOLEAN ext_ok = TRUE;
    EXTENT_MAP Map[2];
    uint32 PS = Vcb->SparingBlockSize ? Vcb->SparingBlockSize : 1;

    buff = (uint8*)DbgAllocatePoolWithTag(NonPagedPool, PS << Vcb->SectorShift, 'bNWD' );
    if (buff) {
        for(i=0; i<BCount; i+=d) {
            if (!((LBA+i) & (PS-1)) &&
               (i+PS <= BCount)) {
                d = PS;
            } else {
                d = 1;
            }
            RC = UDFReadWriteSectors(IrpContext,
                           Vcb,
                           ((LONGLONG)(LBA + i)) << Vcb->SectorShift,
                           d << Vcb->SectorShift,
                           TRUE, buff, FALSE);

            if (RC != STATUS_SUCCESS) {
                Map[0].extLocation = LBA+i;
                Map[0].extLength = d << Vcb->SectorShift;
                UDFMarkSpaceAsXXXNoProtect(Vcb, 0, &(Map[0]), AS_DISCARDED | AS_BAD); // free
                ext_ok = FALSE;
            }
        }
        DbgFreePool(buff);
    }
    return ext_ok;
} // end UDFCheckArea()

/*
    This routine remaps sectors from bad packet
 */
NTSTATUS
UDFRemapPacket(
    IN PIRP_CONTEXT IrpContext,
    IN PVCB Vcb,
    IN uint32 Lba,
    IN BOOLEAN RemapSpared
    )
{
    uint32 i, max, BS, orig;
    PSPARING_MAP Map;
    BOOLEAN verified = FALSE;

    if (Vcb->SparingTable) {

        max = Vcb->SparingCount;
        BS = Vcb->SparingBlockSize;

        // use sparing table for relocation
        if (Vcb->SparingCountFree == (ULONG)-1) {
            UDFPrint(("calculate free spare areas\n"));
re_check:
            UDFPrint(("verify spare area\n"));
            Vcb->SparingCountFree = 0;
            Map = Vcb->SparingTable;
            for(i=0;i<max;i++,Map++) {
                if (Map->origLocation == SPARING_LOC_AVAILABLE) {
                    if (UDFCheckArea(IrpContext, Vcb, Map->mappedLocation, BS)) {
                        Vcb->SparingCountFree++;
                    } else {
                        UDFPrint(("initial check: bad spare block @ %x\n", Map->mappedLocation));
                        Map->origLocation = SPARING_LOC_CORRUPTED;
                        Vcb->SparingTableModified = TRUE;
                    }
                }
            }
        }
        if (!Vcb->SparingCountFree) {
            UDFPrint(("sparing table full\n"));
            return STATUS_DISK_FULL;
        }

        Map = Vcb->SparingTable;
        Lba &= ~(BS-1);
        for(i=0;i<max;i++,Map++) {
            orig = Map->origLocation;
            if (Lba == (orig & ~(BS-1)) ) {
                // already remapped

                UDFPrint(("remap remapped: bad spare block @ %x\n", Map->mappedLocation));
                if (!verified) {
                    verified = TRUE;
                    goto re_check;
                }

                if (!RemapSpared) {
                    return STATUS_SHARING_VIOLATION;
                } else {
                    // look for another remap area
                    Map->origLocation = SPARING_LOC_CORRUPTED;
                    Vcb->SparingTableModified = TRUE;
                    Vcb->SparingCountFree--;
                    break;
                }
            }
        }
        Map = Vcb->SparingTable;
        for(i=0;i<max;i++,Map++) {
            if (Map->origLocation == SPARING_LOC_AVAILABLE) {
                UDFPrint(("remap %x -> %x\n", Lba, Map->mappedLocation));
                Map->origLocation = Lba;
                Vcb->SparingTableModified = TRUE;
                Vcb->SparingCountFree--;
                return STATUS_SUCCESS;
            }
        }
        UDFPrint(("sparing table full\n"));
        return STATUS_DISK_FULL;
    }
    return STATUS_UNSUCCESSFUL;
} // end UDFRemapPacket()

/*
    This routine releases sector mapping when entire packet is marked as free
 */
NTSTATUS
__fastcall
UDFUnmapRange(
    IN PVCB Vcb,
    IN uint32 Lba,
    IN uint32 BCount
    )
{
    uint32 i, max, BS, orig;
    PSPARING_MAP Map;

    if (Vcb->SparingTable) {
        // use sparing table for relocation

        max = Vcb->SparingCount;
        BS = Vcb->SparingBlockSize;
        Map = Vcb->SparingTable;
        for(i=0;i<max;i++,Map++) {
            orig = Map->origLocation;
            switch(orig) {
            case SPARING_LOC_AVAILABLE:
            case SPARING_LOC_CORRUPTED:
                continue;
            }
            if (orig >= Lba &&
              (orig+BS) <= (Lba+BCount)) {
                // unmap
                UDFPrint(("unmap %x -> %x\n", orig, Map->mappedLocation));
                Map->origLocation = SPARING_LOC_AVAILABLE;
                Vcb->SparingTableModified = TRUE;
                Vcb->SparingCountFree++;
            }
        }
    }
    return STATUS_SUCCESS;
} // end UDFUnmapRange()

/*
    This routine returns physical address for relocated sector
 */
uint32
__fastcall
UDFRelocateSector(
    IN PVCB Vcb,
    IN uint32 Lba
    )
{
    uint32 i, max, BS, orig;

    if (Vcb->SparingTable) {
        // use sparing table for relocation
        uint32 _Lba;
        PSPARING_MAP Map = Vcb->SparingTable;

        max = Vcb->SparingCount;
        BS = Vcb->SparingBlockSize;
        _Lba = Lba & ~(BS-1);
        for(i=0;i<max;i++,Map++) {
            orig = Map->origLocation;
            if (_Lba == (orig & ~(BS-1)) ) {
            //if ( (Lba >= (orig = Map->origLocation)) && (Lba < orig + BS) ) {
                return Map->mappedLocation + Lba - orig;
            }
        }
    } else if (Vcb->Vat) {
        // use VAT for relocation
        uint32* Map = Vcb->Vat;
        uint32 root;
        // check if given Lba lays in the partition covered by VAT
        if (Lba >= Vcb->NWA)
            return Vcb->NWA;
        if (Lba < (root = Vcb->Partitions[Vcb->VatPartNdx].PartitionRoot))
            return Lba;
        Map = &(Vcb->Vat[(i = Lba - root)]);
        if ((i < Vcb->VatCount) && (i=(*Map)) ) {
            if (i != UDF_VAT_FREE_ENTRY) {
                return i + root;
            } else {
                return 0x7fffffff;
            }
        }
    }
    return Lba;
} // end UDFRelocateSector()

/*
    This routine checks if the extent specified requires relocation
 */
BOOLEAN
__fastcall
UDFAreSectorsRelocated(
    IN PVCB Vcb,
    IN uint32 Lba,
    IN uint32 BlockCount
    )
{

    if (Vcb->SparingTable) {
        // use sparing table for relocation
        uint32 i, BS, orig;
        BS = Vcb->SparingBlockSize;
        PSPARING_MAP Map;

        Map = Vcb->SparingTable;
        for(i=0;i<Vcb->SparingCount;i++,Map++) {
            if ( ((Lba >= (orig = Map->origLocation)) && (Lba < orig + BS)) ||
                ((Lba+BlockCount-1 >= orig) && (Lba+BlockCount-1 < orig + BS)) ||
                ((orig >= Lba) && (orig < Lba+BlockCount)) ||
                ((orig+BS >= Lba) && (orig+BS < Lba+BlockCount)) ) {
                return TRUE;
            }
        }
    } else if (Vcb->Vat) {
        // use VAT for relocation
        uint32 i, root, j;
        uint32* Map;
        if (Lba < (root = Vcb->Partitions[Vcb->VatPartNdx].PartitionRoot))
            return FALSE;
        if (Lba+BlockCount >= Vcb->NWA)
            return TRUE;
        Map = &(Vcb->Vat[Lba-root/*+i*/]);
        for(i=0; i<BlockCount; i++, Map++) {
            if ((j = (*Map)) &&
               (j != Lba-root+i) &&
               ((j != UDF_VAT_FREE_ENTRY) || ((Lba+i) < Vcb->SessionEndLba)))
                return TRUE;
        }
    }
    return FALSE;
} // end UDFAreSectorsRelocated()

/*
    This routine builds mapping for relocated extent
    If relocation is not required (-1) will be returned
 */
PEXTENT_MAP
__fastcall
UDFRelocateSectors(
    IN PVCB Vcb,
    IN uint32 Lba,
    IN uint32 BlockCount
    )
{
    if (!UDFAreSectorsRelocated(Vcb, Lba, BlockCount)) return UDF_NO_EXTENT_MAP;

    PEXTENT_MAP Extent=NULL, Extent2;
    uint32 NewLba, LastLba, j, i;
    EXTENT_AD locExt;

    LastLba = UDFRelocateSector(Vcb, Lba);
    for(i=0, j=1; i<BlockCount; i++, j++) {
        // create new entry if the extent in not contigous
        if ( ((NewLba = UDFRelocateSector(Vcb, Lba+i+1)) != (LastLba+1)) ||
            (i==(BlockCount-1)) ) {
            locExt.extLength = j << Vcb->SectorShift;
            locExt.extLocation = LastLba-j+1;
            Extent2 = UDFExtentToMapping(&locExt);
            if (!Extent) {
                Extent = Extent2;
            } else {
                Extent = UDFMergeMappings(Extent, Extent2);
                MyFreePool__(Extent2);
            }
            if (!Extent) return NULL;
            j = 0;
        }
        LastLba = NewLba;
    }
    return Extent;
} // end UDFRelocateSectors()

