////////////////////////////////////////////////////////////////////
// Copyright (C) Alexander Telyatnikov, Ivan Keliukh, Yegor Anchishkin, SKIF Software, 1999-2013. Kiev, Ukraine
// All rights reserved
// This file was released under the GPLv2 on June 2015.
////////////////////////////////////////////////////////////////////
/*************************************************************************
*
* File: sys_spec.h
*
* Module: UDF File System Driver (Kernel mode execution only)
*
* Description:
*   The main include file for the UDF file system driver.
*
* Author: Alter
*
*************************************************************************/

#ifndef _UDF_ENV_SPEC_H_
#define _UDF_ENV_SPEC_H_

NTSTATUS
UDFPhReadSynchronous(
    PIRP_CONTEXT IrpContext,
    PDEVICE_OBJECT DeviceObject,
    PVOID Buffer,
    ULONG ByteCount,
    LONGLONG Offset,
    PULONG ReadBytes,
    ULONG Flags
    );

NTSTATUS
UDFPhWriteSynchronous(
    PDEVICE_OBJECT  DeviceObject,
    PVOID Buffer,
    ULONG ByteCount,
    LONGLONG Offset,
    PSIZE_T WrittenBytes,
    ULONG Flags
);

NTSTATUS
NTAPI
UDFTSendIOCTL(
    IN ULONG IoControlCode,
    IN PVCB Vcb,
    IN PVOID InputBuffer ,
    IN ULONG InputBufferLength,
    OUT PVOID OutputBuffer ,
    IN ULONG OutputBufferLength,
    IN BOOLEAN OverrideVerify,
    OUT PIO_STATUS_BLOCK Iosb OPTIONAL
    );

NTSTATUS
UDFPerformDevIoCtrl(
    IN ULONG IoControlCode,
    IN PDEVICE_OBJECT DeviceObject,
    IN PVOID InputBuffer ,
    IN ULONG InputBufferLength,
    OUT PVOID OutputBuffer ,
    IN ULONG OutputBufferLength,
    IN BOOLEAN OverrideVerify,
    OUT PIO_STATUS_BLOCK Iosb OPTIONAL);

// This routine performs low-level write (asynchronously if possible)
extern NTSTATUS UDFTWriteAsync(
    IN PVOID _Vcb,
    IN PVOID Buffer,     // Target buffer
    IN ULONG Length,
    IN ULONG LBA,
    OUT PULONG WrittenBytes,
    IN BOOLEAN FreeBuffer);

VOID
UDFNotifyReportChange(
    PIRP_CONTEXT IrpContext,
    PVCB Vcb,
    PFCB Fcb,
    ULONG Filter,
    ULONG Action,
    PLCB Lcb,
    PFILE_OBJECT FileObject
    );

NTSTATUS NTAPI UDFAsyncCompletionRoutine(IN PDEVICE_OBJECT DeviceObject,
                                         IN PIRP Irp,
                                         IN PVOID Contxt);

NTSTATUS NTAPI UDFSyncCompletionRoutine(IN PDEVICE_OBJECT DeviceObject,
                                        IN PIRP Irp,
                                        IN PVOID Contxt);

NTSTATUS NTAPI UDFSyncCompletionRoutine2(IN PDEVICE_OBJECT DeviceObject,
                                         IN PIRP Irp,
                                         IN PVOID Contxt);

#define UDFGetDevType(DevObj)    (DevObj->DeviceType)


#endif  // _UDF_ENV_SPEC_H_
