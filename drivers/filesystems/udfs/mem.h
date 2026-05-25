////////////////////////////////////////////////////////////////////
// Copyright (C) Alexander Telyatnikov, Ivan Keliukh, Yegor Anchishkin, SKIF Software, 1999-2013. Kiev, Ukraine
// All rights reserved
// This file was released under the GPLv2 on June 2015.
////////////////////////////////////////////////////////////////////

#ifndef __MY_MEM_H__
#define __MY_MEM_H__

#define MyFreeMemoryAndPointer(ptr) \
    if (ptr) {                   \
        MyFreePool__(ptr);      \
        ptr = NULL;             \
    }

#define MY_HEAP_ALIGN             63
#define MyAlignSize__(size) (((size)+MY_HEAP_ALIGN)&(~MY_HEAP_ALIGN))

#define PAGE_SIZE_ALIGN             (PAGE_SIZE - 1)
#define AlignToPageSize(size) (((size)+PAGE_SIZE_ALIGN)&(~PAGE_SIZE_ALIGN))

BOOLEAN inline MyAllocInit(VOID) {return TRUE;}
#define MyAllocRelease()

#ifdef TRACK_SYS_ALLOC_CALLERS
  #define MyAllocatePool__(type,size) DebugAllocatePool(NonPagedPool,MyAlignSize__(size), UDF_BUG_CHECK_ID, __LINE__)
  #define MyAllocatePoolTag__(type,size,tag) DebugAllocatePool(NonPagedPool,MyAlignSize__(size), UDF_BUG_CHECK_ID, __LINE__)
#else //TRACK_SYS_ALLOC_CALLERS
  #define MyAllocatePool__(type,size) DbgAllocatePoolWithTag(NonPagedPool,MyAlignSize__(size), 'fNWD')
  #define MyAllocatePoolTag__(type,size,tag) DbgAllocatePoolWithTag(NonPagedPool,MyAlignSize__(size), tag)
#endif //TRACK_SYS_ALLOC_CALLERS
#define MyFreePool__(addr) DbgFreePool((PCHAR)(addr))

/* This function just scares the hell out of GCC */
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wstringop-overflow"
#endif

ULONG inline MyReallocPool__(PCHAR addr, ULONG len, PCHAR *pnewaddr, ULONG newlen) {
    ULONG _len, _newlen;
    _newlen = MyAlignSize__(newlen);
    _len = MyAlignSize__(len);
    PCHAR newaddr;

    ASSERT(len && newlen);

    if (_newlen != _len) {
#ifdef TRACK_SYS_ALLOC_CALLERS
        newaddr = (PCHAR)DebugAllocatePool(NonPagedPool,_newlen, 0x202, __LINE__);
#else //TRACK_SYS_ALLOC_CALLERS
        newaddr = (PCHAR)MyAllocatePool__(NonPagedPool,_newlen);
#endif //TRACK_SYS_ALLOC_CALLERS
        if (!newaddr) {
            __debugbreak();
            *pnewaddr = addr;
            return 0;
        }
        *pnewaddr = newaddr;
        if (_newlen <= _len) {
            RtlCopyMemory(newaddr, addr, newlen);
        } else {
            RtlCopyMemory(newaddr, addr, len);
            RtlZeroMemory(newaddr+len, _newlen - len);
        }
        MyFreePool__(addr);
    } else {
        *pnewaddr = addr;
    }
    return newlen;
}
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic pop
#endif

#define MyCheckArray(base, index)

#endif // __MY_MEM_H__
