////////////////////////////////////////////////////////////////////
// Copyright (C) Alexander Telyatnikov, Ivan Keliukh, Yegor Anchishkin, SKIF Software, 1999-2013. Kiev, Ukraine
// All rights reserved
// This file was released under the GPLv2 on June 2015.
////////////////////////////////////////////////////////////////////

#ifndef __UDF_PHYS_LIB__H__
#define __UDF_PHYS_LIB__H__

NTSTATUS
UDFTRead(
    PIRP_CONTEXT IrpContext,
    PVOID _Vcb,
    PVOID Buffer,     // Target buffer
    SIZE_T Length,
    ULONG LBA,
    PULONG ReadBytes,
    ULONG Flags
    );

NTSTATUS
UDFTWrite(
    IN PIRP_CONTEXT IrpContext,
    IN PVOID _Vcb,
    IN PVOID Buffer,     // Target buffer
    IN SIZE_T Length,
    IN ULONG LBA,
    OUT PSIZE_T WrittenBytes,
    IN ULONG Flags
    );

#define PH_TMP_BUFFER          1
#define PH_LOCK_CACHE          0x10000000

#define PH_EX_WRITE            0x80000000
#define PH_IO_LOCKED           0x20000000

extern NTSTATUS UDFPrepareForWriteOperation(
    IN PVCB Vcb,
    IN ULONG Lba,
    IN ULONG BCount);

NTSTATUS
UDFDetermineVolumeLayout(
    PIRP_CONTEXT IrpContext,
    PDEVICE_OBJECT DeviceObject,
    PVCB Vcb,
    PULONG SessionStart,
    PULONG SessionEnd
    );

extern NTSTATUS UDFGetBlockSize(PDEVICE_OBJECT DeviceObject, // the target device object
                                PVCB           Vcb);         // Volume control block fro this DevObj

NTSTATUS
UDFGetDiskInfo(
    IN PIRP_CONTEXT IrpContext,
    IN PDEVICE_OBJECT DeviceObject, // the target device object
    IN PVCB Vcb                     // Volume control block from this DevObj
    ); 

NTSTATUS
UDFPrepareForReadOperation(
    IN PIRP_CONTEXT IrpContext,
    IN PVCB Vcb,
    IN uint32 Lba,
    IN uint32 BCount
    );

extern NTSTATUS UDFDoDismountSequence(IN PVCB Vcb,
                                      IN BOOLEAN Eject);

// read physical sectors
NTSTATUS
UDFReadSectors(
    IN PIRP_CONTEXT IrpContext,
    IN PVCB Vcb,
    IN BOOLEAN Translate,// Translate Logical to Physical
    IN ULONG Lba,
    IN ULONG BCount,
    IN BOOLEAN Direct,
    OUT PCHAR Buffer,
    OUT PULONG ReadBytes
    );

// read data inside physical sector
NTSTATUS
UDFReadInSector(
    IN PIRP_CONTEXT IrpContext,
    IN PVCB Vcb,
    IN BOOLEAN Translate,       // Translate Logical to Physical
    IN ULONG Lba,
    IN ULONG i,                 // offset in sector
    IN ULONG l,                 // transfer length
    IN BOOLEAN Direct,
    OUT PCHAR Buffer,
    OUT PULONG ReadBytes
    );

// read unaligned data
NTSTATUS
UDFReadData(
    IN PIRP_CONTEXT IrpContext,
    IN PVCB Vcb,
    IN BOOLEAN Translate,   // Translate Logical to Physical
    IN LONGLONG Offset,
    IN ULONG Length,
    IN BOOLEAN Direct,
    OUT PCHAR Buffer,
    OUT PULONG ReadBytes
    );

// write physical sectors
NTSTATUS UDFWriteSectors(IN PIRP_CONTEXT IrpContext,
                         IN PVCB Vcb,
                         IN BOOLEAN Translate,      // Translate Logical to Physical
                         IN ULONG Lba,
                         IN ULONG WBCount,
                         IN BOOLEAN Direct,         // setting this flag delays flushing of given
                                                    // data to indefinite term
                         IN PCHAR Buffer,
                         OUT PSIZE_T WrittenBytes);
// write directly to cached sector
NTSTATUS UDFWriteInSector(
    IN PIRP_CONTEXT IrpContext,
    IN PVCB Vcb,
    IN BOOLEAN Translate,       // Translate Logical to Physical
    IN ULONG Lba,
    IN ULONG i,                 // offset in sector
    IN ULONG l,                 // transfer length
    IN BOOLEAN Direct,
    OUT PCHAR Buffer,
    OUT PSIZE_T WrittenBytes);

// write data at unaligned offset & length
NTSTATUS
UDFWriteData(
    IN PIRP_CONTEXT IrpContext,
    IN PVCB Vcb,
    IN BOOLEAN Translate,      // Translate Logical to Physical
    IN LONGLONG Offset,
    IN SIZE_T Length,
    IN BOOLEAN Direct,         // setting this flag delays flushing of given
                               // data to indefinite term
    IN PCHAR Buffer,
    OUT PSIZE_T WrittenBytes
);

// This macro copies an unaligned src longword to a dst longword,
// performing an little/big endian swap.

#define SwapCopyUchar4(Dst, Src) \
    (*(UNALIGNED ULONG*)(Dst) = _byteswap_ulong(*(UNALIGNED ULONG*)(Src)))

#endif //__UDF_PHYS_LIB__H__
