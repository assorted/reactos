////////////////////////////////////////////////////////////////////
// Copyright (C) Alexander Telyatnikov, Ivan Keliukh, Yegor Anchishkin, SKIF Software, 1999-2013. Kiev, Ukraine
// All rights reserved
// This file was released under the GPLv2 on June 2015.
////////////////////////////////////////////////////////////////////

#ifndef __UDF_NAME_SUP__H__
#define __UDF_NAME_SUP__H__

VOID
UDFDissectName(
    _In_ PIRP_CONTEXT IrpContext,
    _Inout_ PUNICODE_STRING RemainingName,
    _Out_ PUNICODE_STRING FinalName,
    _Out_ PBOOLEAN IsStream
    );

extern BOOLEAN UDFIsNameInExpression(IN PVCB Vcb,
                                     IN PUNICODE_STRING FileName,
                                     IN PUNICODE_STRING PtrSearchPattern,
                                     OUT PBOOLEAN DosOpen,
                                     IN BOOLEAN IgnoreCase,
                                     IN BOOLEAN ContainsWC,
                                     IN BOOLEAN CanBe8dot3,
                                     IN BOOLEAN KeepIntact);

extern BOOLEAN UDFDoesNameContainWildCards(IN PUNICODE_STRING SearchPattern);

BOOLEAN
UDFIsNameValid(
    IN PUNICODE_STRING SearchPattern,
    OUT BOOLEAN* StreamOpen,
    OUT ULONG* SNameIndex
    );

extern BOOLEAN __fastcall UDFIsMatchAllMask(IN PUNICODE_STRING Name,
                                 OUT BOOLEAN* DosOpen);

extern BOOLEAN __fastcall UDFCanNameBeA8dot3(IN PUNICODE_STRING Name);

NTSTATUS
UDFGetFileNameFromFileInfo(
    IN PUDF_FILE_INFO FileInfo,
    OUT PUNICODE_STRING FileName
    );

NTSTATUS
UDFUpcaseString(
    OUT PUNICODE_STRING DestName,
    IN PUNICODE_STRING SourceName
    );

NTSTATUS
UDFGenerateShortName(
    IN PVCB Vcb,
    IN PUNICODE_STRING FileName,
    OUT PUNICODE_STRING ShortName
    );

#endif //__UDF_NAME_SUP__H__
