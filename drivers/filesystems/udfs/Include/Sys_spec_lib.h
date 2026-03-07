////////////////////////////////////////////////////////////////////
// Copyright (C) Alexander Telyatnikov, Ivan Keliukh, Yegor Anchishkin, SKIF Software, 1999-2013. Kiev, Ukraine
// All rights reserved
// This file was released under the GPLv2 on June 2015.
////////////////////////////////////////////////////////////////////
/*************************************************************************
*
* File: sys_spec_lib.h
*
* Module: UDF File System Driver (Kernel mode execution only)
*
* Description:
*   The main include file for the UDF file system driver.
*
* Author: Alter
*
*************************************************************************/

#ifndef _UDF_SYS_SPEC_LIB__H_
#define _UDF_SYS_SPEC_LIB__H_

// convert UDF timestamp to NT time
LONGLONG UDFTimeToNT(IN PUDF_TIME_STAMP UdfTime);
// translate UDF file attributes to NT ones
ULONG    UDFAttributesToNT(IN PDIR_INDEX_ITEM FileDirNdx,
                           IN tag* FileEntry);
// translate NT file attributes to UDF ones
VOID     UDFAttributesToUDF(IN PDIR_INDEX_ITEM FileDirNdx,
                            IN tag* FileEntry,
                            IN ULONG NTAttr);

// translate all file information to NT
NTSTATUS
UDFFileDirInfoToNT(
    IN PIRP_CONTEXT IrpContext,
    IN PVCB Vcb,
    IN PDIR_INDEX_ITEM FileDirNdx,
    OUT PFILE_BOTH_DIR_INFORMATION NTFileInfo
    );

// convert NT time to UDF timestamp
VOID     UDFTimeToUDF(IN LONGLONG NtTime,
                      OUT PUDF_TIME_STAMP UdfTime);
// change xxxTime field(s) in (Ext)FileEntry
VOID     UDFSetFileXTime(IN PUDF_FILE_INFO FileInfo,
                         IN LONGLONG* CrtTime,
                         IN LONGLONG* AccTime,
                         IN LONGLONG* AttrTime,
                         IN LONGLONG* ChgTime);
// get xxxTime field(s) in (Ext)FileEntry
VOID     UDFGetFileXTime(IN PUDF_FILE_INFO FileInfo,
                         OUT LONGLONG* CrtTime,
                         OUT LONGLONG* AccTime,
                         OUT LONGLONG* AttrTime,
                         OUT LONGLONG* ChgTime);
//
#define UDFUpdateAccessTime(Vcb, FileInfo)             \
if (Vcb->CompatFlags & UDF_VCB_IC_UPDATE_ACCESS_TIME) {       \
    LONGLONG NtTime;                                   \
    KeQuerySystemTime((PLARGE_INTEGER)&NtTime);            \
    UDFSetFileXTime(FileInfo, NULL, &NtTime, NULL, NULL);  \
}
//
#define UDFUpdateModifyTime(Vcb, FileInfo)             \
if (Vcb->CompatFlags & UDF_VCB_IC_UPDATE_MODIFY_TIME) {       \
    LONGLONG NtTime;                                   \
    ULONG Attr;                                        \
    PDIR_INDEX_ITEM DirNdx;                            \
    KeQuerySystemTime((PLARGE_INTEGER)&NtTime);               \
    UDFSetFileXTime(FileInfo, NULL, &NtTime, NULL, &NtTime);  \
    DirNdx = UDFDirIndex(UDFGetDirIndexByFileInfo(FileInfo), (FileInfo)->Index); \
    Attr = UDFAttributesToNT(DirNdx, (FileInfo)->Dloc->FileEntry); \
    if (!(Attr & FILE_ATTRIBUTE_ARCHIVE))                            \
        UDFAttributesToUDF(DirNdx, (FileInfo)->Dloc->FileEntry, Attr); \
}
//
#define UDFUpdateAttrTime(Vcb, FileInfo)               \
if (Vcb->CompatFlags & UDF_VCB_IC_UPDATE_ATTR_TIME) {         \
    LONGLONG NtTime;                                   \
    KeQuerySystemTime((PLARGE_INTEGER)&NtTime);               \
    UDFSetFileXTime(FileInfo, NULL, &NtTime, &NtTime, NULL);  \
}
//
#define UDFUpdateCreateTime(Vcb, FileInfo)             \
{                                                      \
    LONGLONG NtTime;                                   \
    KeQuerySystemTime((PLARGE_INTEGER)&NtTime);               \
    UDFSetFileXTime(FileInfo, &NtTime, &NtTime, &NtTime, &NtTime);  \
}

VOID     UDFNormalizeFileName(IN PUNICODE_STRING FName,
                              IN USHORT valueCRC);

__inline LARGE_INTEGER UDFMakeLargeInteger(LONGLONG value) {
    LARGE_INTEGER result;
    result.QuadPart = value;
    return result;
}

#define UDFGetNTFileId(Vcb, fi) \
    UDFMakeLargeInteger((((fi)->Dloc->FELoc.Mapping[0].extLocation - UDFPartStart(Vcb, -2)) + \
                      ((LONGLONG)(ULONG_PTR)Vcb<<32)))

#define UnicodeIsPrint(a) RtlIsValidOemCharacter(&(a))

#define UDFSysGetAllocSize(Vcb, Size) ((Size + Vcb->SectorSize - 1) & ~((LONGLONG)(Vcb->SectorSize - 1)))

NTSTATUS UDFDoesOSAllowFileToBeTargetForRename__(IN PUDF_FILE_INFO FileInfo);
#define UDFDoesOSAllowFileToBeTargetForHLink__  UDFDoesOSAllowFileToBeTargetForRename__
NTSTATUS UDFDoesOSAllowFileToBeUnlinked__(IN PUDF_FILE_INFO FileInfo);
#define UDFDoesOSAllowFileToBeMoved__  UDFDoesOSAllowFileToBeUnlinked__
NTSTATUS UDFDoesOSAllowFilePretendDeleted__(IN PUDF_FILE_INFO FileInfo);
BOOLEAN UDFRemoveOSReferences__(IN PUDF_FILE_INFO FileInfo);

#endif  // _UDF_SYS_SPEC_LIB__H_
