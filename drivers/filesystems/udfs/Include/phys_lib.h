////////////////////////////////////////////////////////////////////
// Copyright (C) Alexander Telyatnikov, Ivan Keliukh, Yegor Anchishkin, SKIF Software, 1999-2013. Kiev, Ukraine
// All rights reserved
// This file was released under the GPLv2 on June 2015.
////////////////////////////////////////////////////////////////////

#ifndef __UDF_PHYS_LIB__H__
#define __UDF_PHYS_LIB__H__

#ifndef UDF_FORMAT_MEDIA
extern BOOLEAN open_as_device;
extern BOOLEAN opt_invalidate_volume;
extern ULONG LockMode;
#endif //UDF_FORMAT_MEDIA

NTSTATUS
__fastcall
UDFTIOVerify(
    IN PIRP_CONTEXT IrpContext,
    IN void* _Vcb,
    IN void* Buffer,     // Target buffer
    IN SIZE_T Length,
    IN uint32 LBA,
    OUT PSIZE_T IOBytes,
    IN uint32 Flags
    );

extern NTSTATUS
UDFTWriteVerify(
    IN PIRP_CONTEXT IrpContext,
    IN void* _Vcb,
    IN void* Buffer,     // Target buffer
    IN SIZE_T Length,
    IN uint32 LBA,
    OUT PSIZE_T WrittenBytes,
    IN uint32 Flags
    );

NTSTATUS
UDFTReadVerify(
    IN PIRP_CONTEXT IrpContext,
    IN void* _Vcb,
    IN void* Buffer,     // Target buffer
    IN SIZE_T Length,
    IN uint32 LBA,
    OUT PSIZE_T ReadBytes,
    IN uint32 Flags
    );

NTSTATUS
UDFTRead(
    PIRP_CONTEXT IrpContext,
    PVOID _Vcb,
    PVOID Buffer,     // Target buffer
    SIZE_T Length,
    ULONG LBA,
    PSIZE_T ReadBytes,
    ULONG Flags = 0
    );

NTSTATUS
UDFTWrite(
    IN PIRP_CONTEXT IrpContext,
    IN PVOID _Vcb,
    IN PVOID Buffer,     // Target buffer
    IN SIZE_T Length,
    IN ULONG LBA,
    OUT PSIZE_T WrittenBytes,
    IN ULONG Flags = 0
    );

#define PH_TMP_BUFFER          1
#define PH_VCB_IN_RETLEN       2
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
    OUT PSIZE_T ReadBytes
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
    OUT PSIZE_T ReadBytes
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
    OUT PSIZE_T ReadBytes
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

NTSTATUS UDFResetDeviceDriver(IN PVCB Vcb,
                              IN PDEVICE_OBJECT TargetDeviceObject,
                              IN BOOLEAN Unlock);

// This macro copies an unaligned src longword to a dst longword,
// performing an little/big endian swap.

typedef union _UCHAR1 {
    UCHAR  Uchar[1];
    UCHAR  ForceAlignment;
} UCHAR1, *PUCHAR1;

#define SwapCopyUchar4(Dst,Src) {                                        \
    *((UNALIGNED UCHAR1 *)(Dst)) = *((UNALIGNED UCHAR1 *)(Src) + 3);     \
    *((UNALIGNED UCHAR1 *)(Dst) + 1) = *((UNALIGNED UCHAR1 *)(Src) + 2); \
    *((UNALIGNED UCHAR1 *)(Dst) + 2) = *((UNALIGNED UCHAR1 *)(Src) + 1); \
    *((UNALIGNED UCHAR1 *)(Dst) + 3) = *((UNALIGNED UCHAR1 *)(Src));     \
}

#endif //__UDF_PHYS_LIB__H__
