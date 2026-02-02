////////////////////////////////////////////////////////////////////
// Copyright (C) Alexander Telyatnikov, Ivan Keliukh, Yegor Anchishkin, SKIF Software, 1999-2013. Kiev, Ukraine
// All rights reserved
// This file was released under the GPLv2 on June 2015.
////////////////////////////////////////////////////////////////////
/*
    Module:
            Namesup.cpp

    Abstract: FileName support routines
*/

#include "udffs.h"

VOID
UDFDissectName(
    _In_ PIRP_CONTEXT IrpContext,
    IN OUT PUNICODE_STRING RemainingName,
    OUT PUNICODE_STRING FinalName,
    OUT PBOOLEAN IsStream
    )
{
    ULONG NameLength;
    PWCHAR NextWchar;

    //
    //  Check for stream prefix (':')
    //
    if (RemainingName->Length >= sizeof(WCHAR) &&
        RemainingName->Buffer[0] == L':') {

        *IsStream = TRUE;

        // Skip the ':' prefix
        RemainingName->Buffer++;
        RemainingName->Length -= sizeof(WCHAR);
        RemainingName->MaximumLength -= sizeof(WCHAR);
    } else {

        *IsStream = FALSE;

        //
        //  Skip leading backslashes (only for non-stream paths)
        //
        while (RemainingName->Length >= sizeof(WCHAR) &&
               RemainingName->Buffer[0] == L'\\') {
            RemainingName->Buffer++;
            RemainingName->Length -= sizeof(WCHAR);
            RemainingName->MaximumLength -= sizeof(WCHAR);
        }
    }

    //
    //  Find the offset of the next component separator (backslash or colon).
    //  Use Length for bounds checking - do NOT rely on null terminator!
    //
    for (NameLength = 0, NextWchar = RemainingName->Buffer;
         (NameLength < RemainingName->Length) && (*NextWchar != L'\\') && (*NextWchar != L':');
         NameLength += sizeof(WCHAR), NextWchar++);

    //
    //  Set FinalName to the extracted component
    //
    FinalName->Buffer = RemainingName->Buffer;
    FinalName->MaximumLength = FinalName->Length = (USHORT)NameLength;

    //
    //  Adjust RemainingName past the component.
    //  For ':' separator: do NOT consume it (keep in RemainingName for stream suffix).
    //  For '\' separator: consume it.
    //
    if (NameLength == RemainingName->Length) {
        // Last component — nothing left
        RemainingName->Length = 0;
    } else if (RemainingName->Buffer[NameLength / sizeof(WCHAR)] == L':') {
        // ':' separator — keep it in RemainingName
        RemainingName->MaximumLength -= (USHORT)NameLength;
        RemainingName->Length -= (USHORT)NameLength;
        RemainingName->Buffer = (PWCHAR)((PCHAR)RemainingName->Buffer + NameLength);
    } else {
        // '\' separator — consume it
        RemainingName->MaximumLength -= (USHORT)(NameLength + sizeof(WCHAR));
        RemainingName->Length -= (USHORT)(NameLength + sizeof(WCHAR));
        RemainingName->Buffer = (PWCHAR)((PCHAR)RemainingName->Buffer + NameLength + sizeof(WCHAR));
    }

} // end UDFDissectName()

BOOLEAN
UDFIsNameValid(
    IN PUNICODE_STRING SearchPattern,
    OUT BOOLEAN* StreamOpen,
    OUT ULONG* SNameIndex
) {
    LONG   Index, l;
    BOOLEAN _StreamOpen = FALSE;
    PWCHAR Buffer;
    WCHAR c, c0;

    if (StreamOpen) (*StreamOpen) = FALSE;
    // We can't create nameless file or too long path
    if (!(l = SearchPattern->Length/sizeof(WCHAR)) ||
        (l>UDF_X_PATH_LEN)) return FALSE;
    Buffer = SearchPattern->Buffer;
    for(Index = 0; Index<l; Index++, Buffer++) {
        // Check for disallowed characters
        c = (*Buffer);
        if ((c == L'*') ||
           (c == L'>') ||
           (c == L'\"') ||
           (c == L'/') ||
           (c == L'<') ||
           (c == L'|') ||
           ((c >= 0x0000) && (c <= 0x001f)) ||
           (c == L'?')) return FALSE;
        // check if this a Stream path (& validate it)
        if (!(_StreamOpen) && // sub-streams are not allowed
            (Index<(l-1)) && // stream name must be specified
           ((_StreamOpen) = (c == L':'))) {
            if (StreamOpen) (*StreamOpen) = TRUE;
            if (SNameIndex) (*SNameIndex) = Index;
        }
        // According to NT IFS documentation neither SPACE nor DOT can be
        // a trailing character
        if (Index && (c == L'\\') ) {
           if ((c0 == L' ') ||
              (_StreamOpen) || // stream is not a directory
              (c0 == L'.')) return FALSE;
        }
        c0 = c;
    }
    // According to NT IFS documentation neither SPACE nor DOT nor COLONS can be
    // a trailing character
    if (c0 == L' ' ||
        c0 == L':' ||
        c0 == L'.') {

        return FALSE;
    }

    return TRUE;
} // end UDFIsNameValid()


/*

Routine Description:

    This routine will compare two Unicode strings.
    PtrSearchPattern may contain wildcards

Return Value:

    BOOLEAN - TRUE if the expressions match, FALSE otherwise.

*/
BOOLEAN
UDFIsNameInExpression(
    IN PVCB Vcb,
    IN PUNICODE_STRING FileName,
    IN PUNICODE_STRING PtrSearchPattern,
    OUT PBOOLEAN DosOpen,
    IN BOOLEAN IgnoreCase,
    IN BOOLEAN ContainsWC,
    IN BOOLEAN CanBe8dot3,
    IN BOOLEAN KeepIntact // passed to UDFDOSName
    )
{
    BOOLEAN             Match = TRUE;
    UNICODE_STRING      ShortName;
    WCHAR               Buffer[13];

    if (!PtrSearchPattern) return TRUE;
    // we try to open file by LFN by default
    if (DosOpen) (*DosOpen) = FALSE;
    //  If there are wildcards in the expression then we call the
    //  appropriate FsRtlRoutine.
    if (ContainsWC) {
        Match = FsRtlIsNameInExpression( PtrSearchPattern, FileName, IgnoreCase, NULL );
    //  Otherwise do a direct memory comparison for the name string.
    } else if (RtlCompareUnicodeString(FileName, PtrSearchPattern, IgnoreCase)) {
        Match = FALSE;
    }

    if (Match) return TRUE;

    // check if SFN can match this pattern
    if (!CanBe8dot3)
        return FALSE;

    // try to open by SFN
    ShortName.Buffer = (PWCHAR)(&Buffer);
    ShortName.MaximumLength = 13*sizeof(WCHAR);
    UDFDOSName(Vcb, &ShortName, FileName, KeepIntact);

    // PtrSearchPattern is upcased if we are called with IgnoreCase=TRUE
    // DOSName is always upcased
    // thus, we can use case-sensetive compare here to improve performance
    if (ContainsWC) {
        Match = FsRtlIsNameInExpression( PtrSearchPattern, &ShortName, FALSE, NULL );
    //  Otherwise do a direct memory comparison for the name string.
    } else if (!RtlCompareUnicodeString(&ShortName, PtrSearchPattern, FALSE)) {
        Match = TRUE;
    }
    if (DosOpen && Match) {
        // remember that we've opened file by SFN
        (*DosOpen) = TRUE;
    }
    return Match;
} // end UDFIsNameInExpression()


BOOLEAN
__fastcall
UDFIsMatchAllMask(
    IN PUNICODE_STRING Name,
   OUT BOOLEAN* DosOpen
    )
{
    USHORT i;
    PWCHAR Buffer;

    if (DosOpen)
        *DosOpen = FALSE;
    Buffer = Name->Buffer;
    if (Name->Length == sizeof(WCHAR)) {
        // Win32-style wildcard
        if ((*Buffer) != L'*')
            return FALSE;
        return TRUE;
    } else
    if (Name->Length == sizeof(WCHAR)*(8+1+3)) {
        // DOS-style wildcard
        for(i=0;i<8;i++,Buffer++) {
            if ((*Buffer) != DOS_QM)
                return FALSE;
        }
        if ((*Buffer) != DOS_DOT)
            return FALSE;
        Buffer++;
        for(i=9;i<12;i++,Buffer++) {
            if ((*Buffer) != DOS_QM)
                return FALSE;
        }
        if (DosOpen)
            *DosOpen = TRUE;
        return TRUE;
    } else
    if (Name->Length == sizeof(WCHAR)*(3)) {
        // DOS-style wildcard
        if (Buffer[0] != DOS_STAR)
            return FALSE;
        if (Buffer[1] != DOS_DOT)
            return FALSE;
        if (Buffer[2] != DOS_STAR)
            return FALSE;
        if (DosOpen)
            *DosOpen = TRUE;
        return TRUE;
    } else {
        return FALSE;
    }
} // end UDFIsMatchAllMask()

BOOLEAN
__fastcall
UDFCanNameBeA8dot3(
    IN PUNICODE_STRING Name
    )
{
    if (Name->Length >= 13 * sizeof(WCHAR))
        return FALSE;

    ULONG i,l;
    ULONG dot_pos=0;
    ULONG ext_len=0;
    PWCHAR buff = Name->Buffer;

    l = Name->Length / sizeof(WCHAR);

    for(i=0; i<l; i++, buff++) {
        if ( ((*buff) == L'.') ||
            ((*buff) == DOS_DOT) ) {
            if (dot_pos)
                return FALSE;
            dot_pos = i+1;
        } else
        if (dot_pos) {
            ext_len++;
            if (ext_len > 3)
                return FALSE;
        } else
        if (i >= 8) {
            return FALSE;
        }
    }
    return TRUE;
} // end UDFCanNameBeA8dot3()

/*
    Extracts the file name from FileInfo's FileIdent descriptor.

    This function:
    1. Gets the pointer to compressed name in FileIdent
    2. Decompresses it using UDFDecompressUnicode
    3. Returns the allocated UNICODE_STRING

    The caller is responsible for freeing FileName->Buffer using MyFreePool__ or ExFreePool.

    Parameters:
        FileInfo - Pointer to UDF_FILE_INFO structure
        FileName - Output UNICODE_STRING structure (caller provides, we allocate Buffer)

    Returns:
        STATUS_SUCCESS - Name extracted successfully
        STATUS_INVALID_PARAMETER - FileInfo or FileIdent is NULL
        STATUS_INSUFFICIENT_RESOURCES - Memory allocation failed
*/
NTSTATUS
UDFGetFileNameFromFileInfo(
    IN PUDF_FILE_INFO FileInfo,
    OUT PUNICODE_STRING FileName
    )
{
    PFILE_IDENT_DESC FileIdent;
    uint8* CompressedName;
    SIZE_T NameLength;

    // Validate parameters
    if (!FileInfo || !FileName) {
        return STATUS_INVALID_PARAMETER;
    }

    FileIdent = FileInfo->FileIdent;
    if (!FileIdent) {
        // No FileIdent - this might be root directory or special case
        // Return empty string
        FileName->Buffer = NULL;
        FileName->Length = 0;
        FileName->MaximumLength = 0;
        return STATUS_SUCCESS;
    }

    // Get name length from FileIdent
    NameLength = FileIdent->lengthFileIdent;
    if (NameLength == 0) {
        // Empty name - return empty string
        FileName->Buffer = NULL;
        FileName->Length = 0;
        FileName->MaximumLength = 0;
        return STATUS_SUCCESS;
    }

    // Get pointer to compressed name
    // Name is stored after FileIdent structure + ImpUse field
    CompressedName = ((uint8*)(FileIdent + 1)) + FileIdent->lengthOfImpUse;

    // Decompress the name
    // UDFDecompressUnicode allocates buffer and fills FileName
    UDFDecompressUnicode(FileName, CompressedName, NameLength, NULL);

    if (!FileName->Buffer) {
        // Decompression failed
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    return STATUS_SUCCESS;
} // end UDFGetFileNameFromFileInfo()

/*
    Creates an uppercase copy of a Unicode string.

    This function allocates a buffer and converts the source string to uppercase
    for case-insensitive comparisons.

    The caller is responsible for freeing DestName->Buffer using ExFreePool.

    Parameters:
        DestName - Output UNICODE_STRING (caller provides, we allocate Buffer)
        SourceName - Input string to uppercase

    Returns:
        STATUS_SUCCESS - String converted successfully
        STATUS_INVALID_PARAMETER - Invalid parameters
        STATUS_INSUFFICIENT_RESOURCES - Memory allocation failed
*/
NTSTATUS
UDFUpcaseString(
    OUT PUNICODE_STRING DestName,
    IN PUNICODE_STRING SourceName
    )
{
    PWCHAR Buffer;
    ULONG i;

    // Validate parameters
    if (!DestName || !SourceName || !SourceName->Buffer) {
        return STATUS_INVALID_PARAMETER;
    }

    // Handle empty string
    if (SourceName->Length == 0) {
        DestName->Buffer = NULL;
        DestName->Length = 0;
        DestName->MaximumLength = 0;
        return STATUS_SUCCESS;
    }

    // Allocate buffer for uppercase string
    Buffer = (PWCHAR)ExAllocatePoolWithTag(PagedPool,
                                           SourceName->Length + sizeof(WCHAR),
                                           TAG_FILE_NAME);
    if (!Buffer) {
        DestName->Buffer = NULL;
        DestName->Length = 0;
        DestName->MaximumLength = 0;
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    // Copy and uppercase each character
    for (i = 0; i < SourceName->Length / sizeof(WCHAR); i++) {
        Buffer[i] = RtlUpcaseUnicodeChar(SourceName->Buffer[i]);
    }

    // Null terminate
    Buffer[i] = 0;

    // Set output string
    DestName->Buffer = Buffer;
    DestName->Length = SourceName->Length;
    DestName->MaximumLength = SourceName->Length + sizeof(WCHAR);

    return STATUS_SUCCESS;
} // end UDFUpcaseString()

/*
    Generates an 8.3 (DOS) short name from a long file name.

    This function uses the UDF-specific algorithm (UDFDOSName) to generate
    DOS-compatible short names based on UDF revision.

    The caller is responsible for freeing ShortName->Buffer using ExFreePool.

    Parameters:
        Vcb - Volume control block (for UDF revision)
        FileName - Long file name to convert
        ShortName - Output short name (8.3 format)

    Returns:
        STATUS_SUCCESS - Short name generated successfully
        STATUS_INVALID_PARAMETER - Invalid parameters
        STATUS_INSUFFICIENT_RESOURCES - Memory allocation failed
*/
NTSTATUS
UDFGenerateShortName(
    IN PVCB Vcb,
    IN PUNICODE_STRING FileName,
    OUT PUNICODE_STRING ShortName
    )
{
    PWCHAR Buffer;

    // Validate parameters
    if (!Vcb || !FileName || !ShortName || !FileName->Buffer) {
        return STATUS_INVALID_PARAMETER;
    }

    // Handle empty string
    if (FileName->Length == 0) {
        ShortName->Buffer = NULL;
        ShortName->Length = 0;
        ShortName->MaximumLength = 0;
        return STATUS_SUCCESS;
    }

    // Allocate buffer for 8.3 name (max 12 chars + null terminator)
    // Format: 8 chars + '.' + 3 chars = 12 + 1
    Buffer = (PWCHAR)ExAllocatePoolWithTag(PagedPool,
                                           13 * sizeof(WCHAR),
                                           TAG_FILE_NAME);
    if (!Buffer) {
        ShortName->Buffer = NULL;
        ShortName->Length = 0;
        ShortName->MaximumLength = 0;
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    // Set up output string structure
    ShortName->Buffer = Buffer;
    ShortName->MaximumLength = 13 * sizeof(WCHAR);

    // Generate DOS name using UDF algorithm
    // KeepIntact = FALSE means we want to generate a proper 8.3 name
    UDFDOSName(Vcb, ShortName, FileName, FALSE);

    // UDFDOSName sets Length field
    // If generation failed, Length will be 0
    if (ShortName->Length == 0) {
        // Failed to generate short name
        ExFreePool(Buffer);
        ShortName->Buffer = NULL;
        ShortName->MaximumLength = 0;
        return STATUS_OBJECT_NAME_INVALID;
    }

    return STATUS_SUCCESS;
} // end UDFGenerateShortName()

