////////////////////////////////////////////////////////////////////
// Copyright (C) Alexander Telyatnikov, Ivan Keliukh, Yegor Anchishkin, SKIF Software, 1999-2013. Kiev, Ukraine
// All rights reserved
// This file was released under the GPLv2 on June 2015.
////////////////////////////////////////////////////////////////////
/*************************************************************************
*
* File: protos.h
*
* Module: UDF File System Driver (Kernel mode execution only)
*
* Description:
*   Contains the prototypes for functions in UDF FSD.
*
*************************************************************************/

#ifndef _UDF_PROTOS_H_
#define _UDF_PROTOS_H_

#include "mem.h"

/*************************************************************************
* Prototypes for the file create.cpp
*************************************************************************/
extern NTSTATUS NTAPI UDFCreate(
    IN PDEVICE_OBJECT          DeviceObject,       // the logical volume device object
    IN PIRP                    Irp);               // I/O Request Packet

extern NTSTATUS UDFCommonCreate(
    IN PIRP_CONTEXT PtrIrpContext,
    IN PIRP                    Irp);

extern NTSTATUS UDFFirstOpenFile(
    IN PVCB                    Vcb,                // volume control block
    IN PFILE_OBJECT            PtrNewFileObject,   // I/O Mgr. created file object
   OUT PFCB*                   PtrNewFcb,
    IN PUDF_FILE_INFO          RelatedFileInfo,
    IN PUDF_FILE_INFO          NewFileInfo,
    IN PUNICODE_STRING         LocalPath,
    IN PUNICODE_STRING         CurName);

extern NTSTATUS UDFOpenFile(
    IN PVCB                    Vcb,                // volume control block
    IN PFILE_OBJECT            PtrNewFileObject,   // I/O Mgr. created file object
    IN PFCB                    PtrNewFcb);

extern NTSTATUS UDFInitializeFCB(
    IN PFCB                    PtrNewFcb,          // FCB structure to be initialized
    IN PVCB                    Vcb,                // logical volume (VCB) pointer
    IN PtrUDFObjectName        PtrObjectName,      // name of the object
    IN ULONG                   Flags,              // is this a file/directory, etc.
    IN PFILE_OBJECT            FileObject);        // optional file object to be initialized

/*************************************************************************
* Prototypes for the file cleanup.cpp
*************************************************************************/
extern NTSTATUS NTAPI UDFCleanup(
PDEVICE_OBJECT              DeviceObject,       // the logical volume device object
PIRP                        Irp);               // I/O Request Packet

extern NTSTATUS UDFCommonCleanup(
PIRP_CONTEXT PtrIrpContext,
PIRP                        Irp);

extern NTSTATUS UDFCloseFileInfoChain(IN PVCB Vcb,
                                      IN PUDF_FILE_INFO fi,
                                      IN ULONG TreeLength,
                                      IN BOOLEAN VcbAcquired);
/*************************************************************************
* Prototypes for the file close.cpp
*************************************************************************/
extern NTSTATUS NTAPI UDFClose(
PDEVICE_OBJECT              DeviceObject,       // the logical volume device object
PIRP                        Irp);               // I/O Request Packet

extern NTSTATUS UDFCommonClose(
PIRP_CONTEXT PtrIrpContext,
PIRP                        Irp,
BOOLEAN                     CanWait);

#define UDF_CLOSE_NTREQFCB_DELETED 0x01
#define UDF_CLOSE_FCB_DELETED      0x02

extern ULONG    UDFCleanUpFcbChain(IN PVCB Vcb,
                                   IN PUDF_FILE_INFO fi,
                                   IN ULONG TreeLength,
                                   IN BOOLEAN VcbAcquired);

extern VOID UDFCloseAllDelayed(PVCB Vcb);

extern VOID NTAPI UDFDelayedClose(PVOID unused = NULL);

extern NTSTATUS UDFCloseAllXXXDelayedInDir(IN PVCB           Vcb,
                                           IN PUDF_FILE_INFO FileInfo,
                                           IN BOOLEAN        System);

#define UDFCloseAllDelayedInDir(Vcb,FI) \
    UDFCloseAllXXXDelayedInDir(Vcb,FI,FALSE);

#define UDFCloseAllSystemDelayedInDir(Vcb,FI) \
    UDFCloseAllXXXDelayedInDir(Vcb,FI,TRUE);

extern NTSTATUS UDFQueueDelayedClose(PIRP_CONTEXT IrpContext,
                                     PFCB             Fcb);

//extern VOID UDFRemoveFromDelayedQueue(PtrUDFFCB Fcb);
#define UDFRemoveFromDelayedQueue(Fcb) \
    UDFCloseAllDelayedInDir((Fcb)->Vcb, (Fcb)->FileInfo)

#define UDFRemoveFromSystemDelayedQueue(Fcb) \
    UDFCloseAllSystemDelayedInDir((Fcb)->Vcb, (Fcb)->FileInfo)

/*************************************************************************
* Prototypes for the file dircntrl.cpp
*************************************************************************/
extern NTSTATUS NTAPI UDFDirControl(
PDEVICE_OBJECT          DeviceObject,       // the logical volume device object
PIRP                    Irp);               // I/O Request Packet

extern NTSTATUS NTAPI UDFCommonDirControl(
PIRP_CONTEXT PtrIrpContext,
PIRP                    Irp);

extern NTSTATUS NTAPI UDFQueryDirectory(
PIRP_CONTEXT PtrIrpContext,
PIRP                    Irp,
PIO_STACK_LOCATION      IrpSp,
PFILE_OBJECT            FileObject,
PFCB                    Fcb,
PCCB                    Ccb);

extern NTSTATUS NTAPI UDFNotifyChangeDirectory(
PIRP_CONTEXT PtrIrpContext,
PIRP                    Irp,
PIO_STACK_LOCATION      IrpSp,
PFILE_OBJECT            FileObject,
PFCB                    Fcb,
PCCB                    Ccb);

/*************************************************************************
* Prototypes for the file devcntrl.cpp
*************************************************************************/
extern NTSTATUS NTAPI UDFDeviceControl(
PDEVICE_OBJECT              DeviceObject,       // the logical volume device object
PIRP                        Irp);               // I/O Request Packet

extern NTSTATUS NTAPI UDFCommonDeviceControl(
PIRP_CONTEXT PtrIrpContext,
PIRP                        Irp);

extern NTSTATUS NTAPI UDFDevIoctlCompletion(
PDEVICE_OBJECT              PtrDeviceObject,
PIRP                        Irp,
PVOID                       Context);

extern NTSTATUS NTAPI UDFHandleQueryPath(
PVOID                       BufferPointer);

/*************************************************************************
* Prototypes for the file fastio.cpp
*************************************************************************/
extern BOOLEAN NTAPI UDFFastIoCheckIfPossible(
IN PFILE_OBJECT             FileObject,
IN PLARGE_INTEGER           FileOffset,
IN ULONG                    Length,
IN BOOLEAN                  Wait,
IN ULONG                    LockKey,
IN BOOLEAN                  CheckForReadOperation,
OUT PIO_STATUS_BLOCK        IoStatus,
IN PDEVICE_OBJECT           DeviceObject);

extern FAST_IO_POSSIBLE NTAPI UDFIsFastIoPossible(
IN PFCB Fcb);

extern BOOLEAN NTAPI UDFFastIoQueryBasicInfo(
IN PFILE_OBJECT             FileObject,
IN BOOLEAN                  Wait,
OUT PFILE_BASIC_INFORMATION Buffer,
OUT PIO_STATUS_BLOCK        IoStatus,
IN PDEVICE_OBJECT           DeviceObject);

extern BOOLEAN NTAPI UDFFastIoQueryStdInfo(
IN PFILE_OBJECT                FileObject,
IN BOOLEAN                     Wait,
OUT PFILE_STANDARD_INFORMATION Buffer,
OUT PIO_STATUS_BLOCK           IoStatus,
IN PDEVICE_OBJECT              DeviceObject);

extern VOID NTAPI UDFFastIoRelCreateSec(
IN PFILE_OBJECT FileObject);

extern BOOLEAN NTAPI UDFAcqLazyWrite(
IN PVOID   Context,
IN BOOLEAN Wait);

extern VOID NTAPI UDFRelLazyWrite(
IN PVOID Context);

extern BOOLEAN NTAPI UDFAcqReadAhead(
IN PVOID   Context,
IN BOOLEAN Wait);

extern VOID NTAPI UDFRelReadAhead(
IN PVOID Context);

VOID NTAPI UDFDriverUnload(
    IN PDRIVER_OBJECT DriverObject);

// the remaining are only valid under NT Version 4.0 and later
#if(_WIN32_WINNT >= 0x0400)

extern BOOLEAN NTAPI UDFFastIoQueryNetInfo(
IN PFILE_OBJECT                                 FileObject,
IN BOOLEAN                                      Wait,
OUT struct _FILE_NETWORK_OPEN_INFORMATION*      Buffer,
OUT PIO_STATUS_BLOCK                            IoStatus,
IN PDEVICE_OBJECT                               DeviceObject);

extern BOOLEAN NTAPI UDFFastIoMdlRead(
IN PFILE_OBJECT             FileObject,
IN PLARGE_INTEGER           FileOffset,
IN ULONG                    Length,
IN ULONG                    LockKey,
OUT PMDL*                   MdlChain,
OUT PIO_STATUS_BLOCK        IoStatus,
IN PDEVICE_OBJECT           DeviceObject);

extern BOOLEAN UDFFastIoMdlReadComplete(
IN PFILE_OBJECT             FileObject,
OUT PMDL                    MdlChain,
IN PDEVICE_OBJECT           DeviceObject);

extern BOOLEAN NTAPI UDFFastIoPrepareMdlWrite(
IN PFILE_OBJECT             FileObject,
IN PLARGE_INTEGER           FileOffset,
IN ULONG                    Length,
IN ULONG                    LockKey,
OUT PMDL*                   MdlChain,
OUT PIO_STATUS_BLOCK        IoStatus,
IN PDEVICE_OBJECT           DeviceObject);

extern BOOLEAN NTAPI UDFFastIoMdlWriteComplete(
IN PFILE_OBJECT             FileObject,
IN PLARGE_INTEGER           FileOffset,
OUT PMDL                    MdlChain,
IN PDEVICE_OBJECT           DeviceObject);

extern NTSTATUS NTAPI UDFFastIoAcqModWrite(
IN PFILE_OBJECT             FileObject,
IN PLARGE_INTEGER           EndingOffset,
OUT PERESOURCE*             ResourceToRelease,
IN PDEVICE_OBJECT           DeviceObject);

extern NTSTATUS NTAPI UDFFastIoRelModWrite(
IN PFILE_OBJECT             FileObject,
IN PERESOURCE               ResourceToRelease,
IN PDEVICE_OBJECT           DeviceObject);

extern NTSTATUS NTAPI UDFFastIoAcqCcFlush(
IN PFILE_OBJECT             FileObject,
IN PDEVICE_OBJECT           DeviceObject);

extern NTSTATUS NTAPI UDFFastIoRelCcFlush(
IN PFILE_OBJECT             FileObject,
IN PDEVICE_OBJECT           DeviceObject);

extern BOOLEAN NTAPI UDFFastIoDeviceControl (
IN PFILE_OBJECT FileObject,
IN BOOLEAN Wait,
IN PVOID InputBuffer OPTIONAL,
IN ULONG InputBufferLength,
OUT PVOID OutputBuffer OPTIONAL,
IN ULONG OutputBufferLength,
IN ULONG IoControlCode,
OUT PIO_STATUS_BLOCK IoStatus,
IN PDEVICE_OBJECT DeviceObject);

extern BOOLEAN
NTAPI
UDFFastIoCopyWrite (
    IN PFILE_OBJECT FileObject,
    IN PLARGE_INTEGER FileOffset,
    IN ULONG Length,
    IN BOOLEAN Wait,
    IN ULONG LockKey,
    IN PVOID Buffer,
    OUT PIO_STATUS_BLOCK IoStatus,
    IN PDEVICE_OBJECT DeviceObject
    );

#endif  // (_WIN32_WINNT >= 0x0400)

/*************************************************************************
* Prototypes for the file fileinfo.cpp
*************************************************************************/
extern NTSTATUS NTAPI UDFQueryInfo(
PDEVICE_OBJECT  DeviceObject,       // the logical volume device object
PIRP            Irp);               // I/O Request Packet

extern NTSTATUS NTAPI UDFSetInfo(
PDEVICE_OBJECT  DeviceObject,       // the logical volume device object
PIRP            Irp);               // I/O Request Packet

extern NTSTATUS UDFCommonQueryInfo(
    PIRP_CONTEXT PtrIrpContext,
    PIRP                    Irp);

extern NTSTATUS UDFCommonSetInfo(
PIRP_CONTEXT PtrIrpContext,
PIRP                    Irp);

extern NTSTATUS UDFGetBasicInformation(
    IN PFILE_OBJECT                FileObject,
    IN PFCB                        Fcb,
    IN PFILE_BASIC_INFORMATION     PtrBuffer,
 IN OUT LONG*                      PtrReturnedLength);

extern NTSTATUS UDFGetNetworkInformation(
    IN PFCB                           Fcb,
    IN PFILE_NETWORK_OPEN_INFORMATION PtrBuffer,
 IN OUT PLONG                         PtrReturnedLength);

extern NTSTATUS UDFGetStandardInformation(
    IN PFCB                        Fcb,
    IN PFILE_STANDARD_INFORMATION  PtrBuffer,
 IN OUT PLONG                      PtrReturnedLength);

extern NTSTATUS UDFGetInternalInformation(
    PIRP_CONTEXT PtrIrpContext,
    IN PFCB                        Fcb,
    IN PCCB                        Ccb,
    IN PFILE_INTERNAL_INFORMATION  PtrBuffer,
 IN OUT PLONG                      PtrReturnedLength);

extern NTSTATUS UDFGetEaInformation(
    PIRP_CONTEXT PtrIrpContext,
    IN PFCB                 Fcb,
    IN PFILE_EA_INFORMATION PtrBuffer,
 IN OUT PLONG               PtrReturnedLength);

extern NTSTATUS UDFGetFullNameInformation(
    IN PFILE_OBJECT                FileObject,
    IN PFILE_NAME_INFORMATION      PtrBuffer,
 IN OUT PLONG                      PtrReturnedLength);

extern NTSTATUS UDFGetAltNameInformation(
    IN PFCB                        Fcb,
    IN PFILE_NAME_INFORMATION      PtrBuffer,
 IN OUT PLONG                      PtrReturnedLength);

extern NTSTATUS UDFGetPositionInformation(
    IN PFILE_OBJECT               FileObject,
    IN PFILE_POSITION_INFORMATION PtrBuffer,
 IN OUT PLONG                     PtrReturnedLength);

extern NTSTATUS UDFGetFileStreamInformation(
    IN PFCB                       Fcb,
    IN PFILE_STREAM_INFORMATION   PtrBuffer,
 IN OUT PULONG                    PtrReturnedLength);

extern NTSTATUS UDFSetBasicInformation(
    IN PFCB                   Fcb,
    IN PCCB                        Ccb,
    IN PFILE_OBJECT                FileObject,
    IN PFILE_BASIC_INFORMATION     PtrBuffer);

extern NTSTATUS UDFMarkStreamsForDeletion(
    IN PVCB           Vcb,
    IN PFCB           Fcb,
    IN BOOLEAN        ForDel);

extern NTSTATUS UDFSetDispositionInformation(
    IN PFCB                            Fcb,
    IN PCCB                            Ccb,
    IN PVCB                            Vcb,
    IN PFILE_OBJECT                    FileObject,
    IN BOOLEAN                         Delete);

extern NTSTATUS UDFSetAllocationInformation(
    IN PFCB                            Fcb,
    IN PCCB                            Ccb,
    IN PVCB                            Vcb,
    IN PFILE_OBJECT                    FileObject,
    IN PIRP_CONTEXT PtrIrpContext,
    IN PIRP                            Irp,
    IN PFILE_ALLOCATION_INFORMATION    PtrBuffer);

extern NTSTATUS UDFSetEOF(
    IN PIO_STACK_LOCATION              PtrSp,
    IN PFCB                            Fcb,
    IN PCCB                            Ccb,
    IN PVCB                            Vcb,
    IN PFILE_OBJECT                    FileObject,
    IN PIRP                            Irp,
    IN PFILE_END_OF_FILE_INFORMATION   PtrBuffer);

extern NTSTATUS UDFSetRenameInfo(IN PIO_STACK_LOCATION IrpSp,
                          IN PFCB Fcb,
                          IN PCCB Ccb,
                          IN PFILE_OBJECT FileObject,
                          IN PFILE_RENAME_INFORMATION PtrBuffer);

extern NTSTATUS UDFStoreFileId(
    IN PVCB Vcb,
    IN PCCB Ccb,
    IN PUDF_FILE_INFO fi,
    IN LONGLONG Id);

extern NTSTATUS UDFRemoveFileId(
    IN PVCB Vcb,
    IN LONGLONG Id);

#define UDFRemoveFileId__(Vcb, fi) \
    UDFRemoveFileId(Vcb, UDFGetNTFileId(Vcb, fi, &(fi->Fcb->FCBName->ObjectName)));

extern VOID UDFReleaseFileIdCache(
    IN PVCB Vcb);

extern NTSTATUS UDFGetOpenParamsByFileId(
    IN PVCB Vcb,
    IN LONGLONG Id,
    OUT PUNICODE_STRING* FName,
    OUT BOOLEAN* CaseSens);

extern NTSTATUS UDFHardLink(
    IN PIO_STACK_LOCATION PtrSp,
    IN PFCB Fcb1,
    IN PCCB Ccb1,
    IN PFILE_OBJECT FileObject1,   // Source File
    IN PFILE_LINK_INFORMATION PtrBuffer);
/*************************************************************************
* Prototypes for the file flush.cpp
*************************************************************************/
extern NTSTATUS NTAPI UDFFlush(
PDEVICE_OBJECT    DeviceObject,       // the logical volume device object
PIRP              Irp);               // I/O Request Packet

extern NTSTATUS UDFCommonFlush(
PIRP_CONTEXT PtrIrpContext,
PIRP                        Irp);

extern ULONG UDFFlushAFile(
PFCB              Fcb,
PCCB              Ccb,
PIO_STATUS_BLOCK  PtrIoStatus,
IN ULONG          FlushFlags = 0);

extern ULONG UDFFlushADirectory(
IN PVCB Vcb,
IN PUDF_FILE_INFO      FI,
OUT PIO_STATUS_BLOCK   PtrIoStatus,
ULONG                  FlushFlags = 0);

extern ULONG UDFFlushLogicalVolume(
PIRP_CONTEXT PtrIrpContext,
PIRP                   Irp,
PVCB                   Vcb,
ULONG                  FlushFlags = 0);

extern NTSTATUS NTAPI UDFFlushCompletion(
PDEVICE_OBJECT              PtrDeviceObject,
PIRP                        Irp,
PVOID                       Context);

extern BOOLEAN UDFFlushIsBreaking(
IN PVCB         Vcb,
IN ULONG        FlushFlags = 0);

extern VOID UDFFlushTryBreak(
IN PVCB         Vcb);

/*************************************************************************
* Prototypes for the file fscntrl.cpp
*************************************************************************/

extern NTSTATUS NTAPI UDFFSControl(
PDEVICE_OBJECT      DeviceObject,
PIRP                Irp);

extern NTSTATUS NTAPI UDFCommonFSControl(
PIRP_CONTEXT PtrIrpContext,
PIRP                Irp);                // I/O Request Packet

extern NTSTATUS NTAPI UDFUserFsCtrlRequest(
PIRP_CONTEXT PtrIrpContext,
PIRP                Irp);

extern NTSTATUS NTAPI UDFMountVolume(
PIRP_CONTEXT PtrIrpContext,
PIRP Irp);

extern VOID UDFScanForDismountedVcb (IN PIRP_CONTEXT IrpContext);

extern NTSTATUS UDFCompleteMount(IN PVCB Vcb);

extern VOID     UDFCloseResidual(IN PVCB Vcb);

extern VOID     UDFCleanupVCB(IN PVCB Vcb);

extern NTSTATUS UDFIsVolumeMounted(IN PIRP_CONTEXT IrpContext,
                                   IN PIRP Irp);

extern NTSTATUS UDFIsVolumeDirty(IN PIRP_CONTEXT IrpContext,
                          IN PIRP Irp);

extern NTSTATUS UDFGetStatistics(IN PIRP_CONTEXT IrpContext,
                                 IN PIRP Irp);

extern NTSTATUS UDFLockVolume (IN PIRP_CONTEXT IrpContext,
                               IN PIRP Irp,
                               IN ULONG PID = -1);

extern NTSTATUS UDFUnlockVolume (IN PIRP_CONTEXT IrpContext,
                                 IN PIRP Irp,
                                 IN ULONG PID = -1);

extern NTSTATUS UDFIsPathnameValid(IN PIRP_CONTEXT IrpContext,
                                   IN PIRP Irp);

extern NTSTATUS UDFDismountVolume(IN PIRP_CONTEXT IrpContext,
                                  IN PIRP Irp);

extern NTSTATUS UDFGetVolumeBitmap(IN PIRP_CONTEXT IrpContext,
                                   IN PIRP Irp);

extern NTSTATUS UDFGetRetrievalPointers(IN PIRP_CONTEXT IrpContext,
                                        IN PIRP  Irp,
                                        IN ULONG Special);

extern NTSTATUS UDFInvalidateVolumes(IN PIRP_CONTEXT IrpContext,
                                     IN PIRP Irp);

/*************************************************************************
* Prototypes for the file LockCtrl.cpp
*************************************************************************/

extern NTSTATUS NTAPI UDFLockControl(
    IN PDEVICE_OBJECT DeviceObject,       // the logical volume device object
    IN PIRP           Irp);               // I/O Request Packet

extern NTSTATUS NTAPI UDFCommonLockControl(
    IN PIRP_CONTEXT PtrIrpContext,
    IN PIRP             Irp);

extern BOOLEAN NTAPI UDFFastLock(
    IN PFILE_OBJECT           FileObject,
    IN PLARGE_INTEGER         FileOffset,
    IN PLARGE_INTEGER         Length,
    PEPROCESS                 ProcessId,
    ULONG                     Key,
    BOOLEAN                   FailImmediately,
    BOOLEAN                   ExclusiveLock,
    OUT PIO_STATUS_BLOCK      IoStatus,
    IN PDEVICE_OBJECT         DeviceObject);

extern BOOLEAN NTAPI UDFFastUnlockSingle(
    IN PFILE_OBJECT           FileObject,
    IN PLARGE_INTEGER         FileOffset,
    IN PLARGE_INTEGER         Length,
    PEPROCESS                 ProcessId,
    ULONG                     Key,
    OUT PIO_STATUS_BLOCK      IoStatus,
    IN PDEVICE_OBJECT         DeviceObject);

extern BOOLEAN NTAPI UDFFastUnlockAll(
    IN PFILE_OBJECT           FileObject,
    PEPROCESS                 ProcessId,
    OUT PIO_STATUS_BLOCK      IoStatus,
    IN PDEVICE_OBJECT         DeviceObject);

extern BOOLEAN NTAPI UDFFastUnlockAllByKey(
    IN PFILE_OBJECT           FileObject,
    PEPROCESS                 ProcessId,
    ULONG                     Key,
    OUT PIO_STATUS_BLOCK      IoStatus,
    IN PDEVICE_OBJECT         DeviceObject);

/*************************************************************************
* Prototypes for the file misc.cpp
*************************************************************************/
extern NTSTATUS UDFInitializeZones(
VOID);

extern VOID UDFDestroyZones(
VOID);

extern BOOLEAN __fastcall UDFIsIrpTopLevel(
PIRP                        Irp);                   // the IRP sent to our dispatch routine

extern long UDFExceptionFilter(
PIRP_CONTEXT PtrIrpContext,
PEXCEPTION_POINTERS         PtrExceptionPointers);

extern NTSTATUS UDFExceptionHandler(
PIRP_CONTEXT PtrIrpContext,
PIRP                        Irp);

extern VOID UDFLogEvent(
NTSTATUS                    UDFEventLogId,  // the UDF private message id
NTSTATUS                    RC);            // any NT error code we wish to log ...

extern PtrUDFObjectName UDFAllocateObjectName(
VOID);

extern VOID UDFReleaseObjectName(
PtrUDFObjectName            PtrObjectName);

extern PCCB UDFAllocateCCB(VOID);

extern VOID UDFReleaseCCB(PCCB Ccb);

extern VOID __fastcall UDFCleanUpCCB(PCCB Ccb);

extern PFCB UDFAllocateFCB(VOID);

/*extern VOID __fastcall UDFReleaseFCB(
PtrUDFFCB                   Fcb);*/
__inline
VOID
UDFReleaseFCB(
    PFCB Fcb
    )
{
    ASSERT(Fcb);

    MyFreePool__(Fcb);

    return;
}

extern VOID __fastcall UDFCleanUpFCB(PFCB Fcb);

extern PIRP_CONTEXT UDFAllocateIrpContext(
PIRP                        Irp,
PDEVICE_OBJECT              PtrTargetDeviceObject);

extern VOID UDFReleaseIrpContext(
PIRP_CONTEXT PtrIrpContext);

extern NTSTATUS UDFPostRequest(
PIRP_CONTEXT PtrIrpContext,
PIRP                        Irp);

extern VOID NTAPI UDFCommonDispatch(
VOID                            *Context);  // actually an IRPContext structure

extern NTSTATUS UDFInitializeVCB(
PDEVICE_OBJECT              PtrVolumeDeviceObject,
PDEVICE_OBJECT              PtrTargetDeviceObject,
PVPB                        PtrVPB);

extern VOID
UDFReadRegKeys(
    PVCB Vcb,
    BOOLEAN Update,
    BOOLEAN UseCfg);

extern ULONG UDFGetRegParameter(
    IN PVCB Vcb,
    IN PCWSTR Name,
    IN ULONG DefValue = 0);

extern ULONG
UDFGetCfgParameter(
    IN PVCB Vcb,
    IN PCWSTR Name,
    IN ULONG DefValue
    );

extern VOID UDFReleaseVCB(
    PVCB Vcb);

extern ULONG UDFRegCheckParameterValue(
    IN PUNICODE_STRING RegistryPath,
    IN PCWSTR Name,
    IN PUNICODE_STRING PtrVolumePath,
    IN PCWSTR DefaultPath,
    IN ULONG DefValue = 0);

extern VOID UDFInitializeStackIrpContextFromLite(
    OUT PIRP_CONTEXT IrpContext,
    IN PIRP_CONTEXT_LITE IrpContextLite);

extern NTSTATUS UDFInitializeIrpContextLite (
    OUT PIRP_CONTEXT_LITE *IrpContextLite,
    IN PIRP_CONTEXT IrpContext,
    IN PFCB                Fcb);

extern ULONG
UDFIsResourceAcquired(
    IN PERESOURCE Resource
    );

extern BOOLEAN UDFAcquireResourceExclusiveWithCheck(
    IN PERESOURCE Resource
    );

extern BOOLEAN UDFAcquireResourceSharedWithCheck(
    IN PERESOURCE Resource
    );

extern NTSTATUS UDFWCacheErrorHandler(
    IN PVOID Context,
    IN PWCACHE_ERROR_CONTEXT ErrorInfo
    );

extern NTSTATUS NTAPI UDFFilterCallbackAcquireForCreateSection(
    IN PFS_FILTER_CALLBACK_DATA CallbackData,
    IN PVOID *CompletionContext
    );

/*************************************************************************
* Prototypes for the file NameSup.cpp
*************************************************************************/

#include "namesup.h"

/*************************************************************************
* Prototypes for the file Udf_info\physical.cpp
*************************************************************************/
#if 0

extern OSSTATUS UDFTRead(PVOID           _Vcb,
                         PVOID           Buffer,     // Target buffer
                         ULONG           Length,
                         ULONG           LBA,
                         PULONG          ReadBytes,
                         ULONG           Flags = 0);

extern OSSTATUS UDFTWrite(IN PVOID _Vcb,
                   IN PVOID Buffer,     // Target buffer
                   IN ULONG Length,
                   IN ULONG LBA,
                   OUT PULONG WrittenBytes,
                   IN ULONG Flags = 0);

extern OSSTATUS UDFPrepareForWriteOperation(
    IN PVCB Vcb,
    IN ULONG Lba,
    IN ULONG BCount);

extern OSSTATUS UDFReadDiscTrackInfo(PDEVICE_OBJECT DeviceObject, // the target device object
                                     PVCB           Vcb);         // Volume Control Block for ^ DevObj

extern OSSTATUS UDFUseStandard(PDEVICE_OBJECT DeviceObject, // the target device object
                               PVCB           Vcb);         // Volume control block fro this DevObj

extern OSSTATUS UDFGetBlockSize(PDEVICE_OBJECT DeviceObject, // the target device object
                                PVCB           Vcb);         // Volume control block fro this DevObj

extern OSSTATUS UDFGetDiskInfo(IN PDEVICE_OBJECT DeviceObject, // the target device object
                               IN PVCB           Vcb);         // Volume control block from this DevObj

extern VOID     UDFUpdateNWA(PVCB Vcb,
                             ULONG LBA,
                             ULONG BCount,
                             OSSTATUS RC);

extern OSSTATUS UDFDoDismountSequence(IN PVCB Vcb,
                                      IN BOOLEAN Eject);

// read physical sectors
OSSTATUS UDFReadSectors(IN PVCB Vcb,
                        IN BOOLEAN Translate,// Translate Logical to Physical
                        IN ULONG Lba,
                        IN ULONG BCount,
                        IN BOOLEAN Direct,
                        OUT PCHAR Buffer,
                        OUT PSIZE_T ReadBytes);

// read data inside physical sector
extern OSSTATUS UDFReadInSector(IN PVCB Vcb,
                         IN BOOLEAN Translate,       // Translate Logical to Physical
                         IN ULONG Lba,
                         IN ULONG i,                 // offset in sector
                         IN ULONG l,                 // transfer length
                         IN BOOLEAN Direct,
                         OUT PCHAR Buffer,
                         OUT PULONG ReadBytes);
// read unaligned data
extern OSSTATUS UDFReadData(IN PVCB Vcb,
                     IN BOOLEAN Translate,   // Translate Logical to Physical
                     IN LONGLONG Offset,
                     IN ULONG Length,
                     IN BOOLEAN Direct,
                     OUT PCHAR Buffer,
                     OUT PULONG ReadBytes);

// write physical sectors
OSSTATUS UDFWriteSectors(IN PVCB Vcb,
                         IN BOOLEAN Translate,      // Translate Logical to Physical
                         IN ULONG Lba,
                         IN ULONG WBCount,
                         IN BOOLEAN Direct,         // setting this flag delays flushing of given
                                                    // data to indefinite term
                         IN PCHAR Buffer,
                         OUT PULONG WrittenBytes);
// write directly to cached sector
OSSTATUS UDFWriteInSector(IN PVCB Vcb,
                          IN BOOLEAN Translate,       // Translate Logical to Physical
                          IN ULONG Lba,
                          IN ULONG i,                 // offset in sector
                          IN ULONG l,                 // transfer length
                          IN BOOLEAN Direct,
                          OUT PCHAR Buffer,
                          OUT PULONG WrittenBytes);
// write data at unaligned offset & length
OSSTATUS UDFWriteData(IN PVCB Vcb,
                      IN BOOLEAN Translate,      // Translate Logical to Physical
                      IN LONGLONG Offset,
                      IN ULONG Length,
                      IN BOOLEAN Direct,         // setting this flag delays flushing of given
                                                 // data to indefinite term
                      IN PCHAR Buffer,
                      OUT PULONG WrittenBytes);

OSSTATUS UDFResetDeviceDriver(IN PVCB Vcb.
                              IN PDEVICE_OBJECT TargetDeviceObject,
                              IN BOOLEAN Unlock);
#endif
/*************************************************************************
* Prototypes for the file Pnp.cpp
*************************************************************************/
NTSTATUS
NTAPI
UDFPnp (
    IN PDEVICE_OBJECT DeviceObject,
    IN PIRP Irp
    );

/*************************************************************************
* Prototypes for the file read.cpp
*************************************************************************/
extern OSSTATUS NTAPI UDFRead(
    PDEVICE_OBJECT              DeviceObject,       // the logical volume device object
    PIRP                        Irp);               // I/O Request Packet

extern NTSTATUS UDFPostStackOverflowRead(
    IN PIRP_CONTEXT PtrIrpContext,
    IN PIRP             Irp,
    IN PFCB             Fcb);

extern VOID NTAPI UDFStackOverflowRead(
    IN PVOID Context,
    IN PKEVENT Event);

extern NTSTATUS UDFCommonRead(
    PIRP_CONTEXT PtrIrpContext,
    PIRP             Irp);

extern PVOID UDFMapUserBuffer(
    PIRP Irp);

extern NTSTATUS UDFLockUserBuffer(
    PIRP_CONTEXT PtrIrpContext,
    PIRP    Irp,
    LOCK_OPERATION LockOperation,
    ULONG   Length);

extern NTSTATUS UDFUnlockCallersBuffer(
    PIRP_CONTEXT PtrIrpContext,
    PIRP    Irp,
    PVOID   SystemBuffer);

extern VOID UDFMdlComplete(
    PIRP_CONTEXT PtrIrpContext,
    PIRP                    Irp,
    PIO_STACK_LOCATION      IrpSp,
    BOOLEAN                 ReadCompletion);

extern NTSTATUS
UDFCheckAccessRights(
    PFILE_OBJECT FileObject,
    PACCESS_STATE AccessState,
    PFCB         Fcb,
    PCCB         Ccb,
    ACCESS_MASK  DesiredAccess,
    USHORT       ShareAccess);

extern NTSTATUS
UDFSetAccessRights(
    PFILE_OBJECT FileObject,
    PACCESS_STATE AccessState,
    PFCB         Fcb,
    PCCB         Ccb,
    ACCESS_MASK  DesiredAccess,
    USHORT       ShareAccess);

/*************************************************************************
* Prototypes for the file Shutdown.cpp
*************************************************************************/
extern NTSTATUS NTAPI UDFShutdown(
PDEVICE_OBJECT              DeviceObject,       // the logical volume device object
PIRP                        Irp);               // I/O Request Packet

extern NTSTATUS UDFCommonShutdown(
PIRP_CONTEXT PtrIrpContext,
PIRP                        Irp);

/*************************************************************************
* Prototypes for the file Udf_dbg.cpp
*************************************************************************/
extern BOOLEAN
UDFDebugAcquireResourceSharedLite(
      IN PERESOURCE Resource,
      IN BOOLEAN    Wait,
      ULONG         BugCheckId,
      ULONG         Line);

extern BOOLEAN
UDFDebugAcquireSharedStarveExclusive(
      IN PERESOURCE Resource,
      IN BOOLEAN    Wait,
      ULONG         BugCheckId,
      ULONG         Line);

extern BOOLEAN
UDFDebugAcquireResourceExclusiveLite(
      IN PERESOURCE Resource,
      IN BOOLEAN    Wait,
      ULONG         BugCheckId,
      ULONG         Line);

extern VOID
UDFDebugReleaseResourceForThreadLite(
    IN PERESOURCE  Resource,
    IN ERESOURCE_THREAD  ResourceThreadId,
    ULONG         BugCheckId,
    ULONG         Line);

extern VOID
UDFDebugDeleteResource(
    IN PERESOURCE  Resource,
    IN ERESOURCE_THREAD  ResourceThreadId,
    ULONG         BugCheckId,
    ULONG         Line);

extern NTSTATUS
UDFDebugInitializeResourceLite(
    IN PERESOURCE  Resource,
    IN ERESOURCE_THREAD  ResourceThreadId,
    ULONG         BugCheckId,
    ULONG         Line);

extern VOID
UDFDebugConvertExclusiveToSharedLite(
    IN PERESOURCE  Resource,
    IN ERESOURCE_THREAD  ResourceThreadId,
    ULONG         BugCheckId,
    ULONG         Line);

extern BOOLEAN
UDFDebugAcquireSharedWaitForExclusive(
    IN PERESOURCE Resource,
    IN BOOLEAN    Wait,
    ULONG         BugCheckId,
    ULONG         Line);

extern LONG
UDFDebugInterlockedIncrement(
    IN PLONG      addr,
    ULONG         BugCheckId,
    ULONG         Line);

extern LONG
UDFDebugInterlockedDecrement(
    IN PLONG      addr,
    ULONG         BugCheckId,
    ULONG         Line);

extern LONG
UDFDebugInterlockedExchangeAdd(
    IN PLONG      addr,
    IN LONG       i,
    ULONG         BugCheckId,
    ULONG         Line);

/*************************************************************************
* Prototypes for the file UDFinit.cpp
*************************************************************************/
extern "C" NTSTATUS NTAPI DriverEntry(
PDRIVER_OBJECT              DriverObject,       // created by the I/O sub-system
PUNICODE_STRING             RegistryPath);      // path to the registry key

extern VOID NTAPI UDFInitializeFunctionPointers(
PDRIVER_OBJECT              DriverObject);      // created by the I/O sub-system

extern VOID NTAPI
UDFFsNotification(IN PDEVICE_OBJECT DeviceObject,
                  IN BOOLEAN FsActive);

#ifndef WIN64
//extern ptrFsRtlNotifyVolumeEvent FsRtlNotifyVolumeEvent;
#endif //WIN64

extern BOOLEAN
UDFGetInstallVersion(PULONG iVer);

extern BOOLEAN
UDFGetInstallTime(PULONG iTime);

extern BOOLEAN
UDFGetTrialEnd(PULONG iTrial);

/*************************************************************************
* Prototypes for the file verify.cpp
*************************************************************************/

extern NTSTATUS UDFVerifyVcb (
    IN PIRP_CONTEXT IrpContext,
    IN PVCB Vcb
    );

extern NTSTATUS UDFVerifyVolume (
                    IN PIRP Irp);

extern NTSTATUS UDFPerformVerify (
    IN PIRP_CONTEXT IrpContext,
    IN PIRP Irp,
    IN PDEVICE_OBJECT DeviceToVerify
    );

extern BOOLEAN UDFCheckForDismount (
    IN PIRP_CONTEXT IrpContext,
    IN PVCB Vcb,
    IN BOOLEAN VcbAcquired
    );

extern BOOLEAN UDFDismountVcb (
    IN PVCB Vcb,
    IN BOOLEAN VcbAcquired);

extern NTSTATUS UDFCompareVcb(IN PVCB OldVcb,
                              IN PVCB NewVcb,
                              IN BOOLEAN PhysicalOnly);

/*************************************************************************
* Prototypes for the file VolInfo.cpp
*************************************************************************/
extern NTSTATUS NTAPI UDFQueryVolInfo(PDEVICE_OBJECT DeviceObject,
                                      PIRP Irp);

extern NTSTATUS UDFCommonQueryVolInfo (PIRP_CONTEXT PtrIrpContext,
                                       PIRP Irp);

extern NTSTATUS NTAPI UDFSetVolInfo(PDEVICE_OBJECT DeviceObject,       // the logical volume device object
                              PIRP           Irp);               // I/O Request Packet

extern NTSTATUS UDFCommonSetVolInfo(PIRP_CONTEXT PtrIrpContext,
                                    PIRP             Irp);

/*************************************************************************
* Prototypes for the file write.cpp
*************************************************************************/
extern NTSTATUS NTAPI UDFWrite(
PDEVICE_OBJECT              DeviceObject,       // the logical volume device object
PIRP                        Irp);               // I/O Request Packet

extern NTSTATUS UDFCommonWrite(
PIRP_CONTEXT PtrIrpContext,
PIRP                        Irp);

extern VOID NTAPI UDFDeferredWriteCallBack (
VOID                        *Context1,          // Should be PtrIrpContext
VOID                        *Context2);         // Should be Irp

extern VOID UDFPurgeCacheEx_(
PFCB                        Fcb,
LONGLONG                    Offset,
LONGLONG                    Length,
//#ifndef ALLOW_SPARSE
BOOLEAN                     CanWait,
//#endif ALLOW_SPARSE
PVCB                        Vcb,
PFILE_OBJECT                FileObject
);

extern VOID UDFSetModified(
    IN PVCB        Vcb
);

extern VOID UDFPreClrModified(
    IN PVCB        Vcb
);

extern VOID UDFClrModified(
    IN PVCB        Vcb
);

/*#ifdef ALLOW_SPARSE
  #define UDFZeroDataEx(Fcb, Offset, Length, CanWait) \
      UDFPurgeCacheEx_(Fcb, Offset, Length)
  #define UDFPurgeCacheEx(Fcb, Offset, Length, CanWait) \
      UDFPurgeCacheEx_(Fcb, Offset, Length)
#else // ALLOW_SPARSE*/
  #define UDFZeroDataEx(Fcb, Offset, Length, CanWait, Vcb, FileObject) \
      UDFPurgeCacheEx_(Fcb, Offset, Length, CanWait, Vcb, FileObject)
  #define UDFPurgeCacheEx(Fcb, Offset, Length, CanWait, Vcb, FileObject) \
      UDFPurgeCacheEx_(Fcb, Offset, Length, CanWait, Vcb, FileObject)
//#endif //ALLOW_SPARSE

BOOLEAN
UDFZeroData (
    IN PVCB Vcb,
    IN PFILE_OBJECT FileObject,
    IN ULONG StartingZero,
    IN ULONG ByteCount,
    IN BOOLEAN CanWait
    );

NTSTATUS
UDFToggleMediaEjectDisable (
    IN PVCB Vcb,
    IN BOOLEAN PreventRemoval
    );


#endif  // _UDF_PROTOS_H_
