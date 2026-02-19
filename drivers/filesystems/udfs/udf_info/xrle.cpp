////////////////////////////////////////////////////////////////////
// Copyright (C) Alexander Telyatnikov, Ivan Keliukh, Yegor Anchishkin, SKIF Software, 1999-2013. Kiev, Ukraine
// All rights reserved
// This file was released under the GPLv2 on June 2015.
////////////////////////////////////////////////////////////////////
/*
 * XRLE - eXtreme Run Length Encoding
 *
 * Original algorithm by Konstantinos Agiannis, BSD 2-Clause license.
 * Adapted for Windows kernel-mode use in the UDF file system driver.
 *
 * Used to compress the free-space bitmap snapshot (FSBM_OldBitmap) in
 * memory, reducing non-paged pool usage when mounting large volumes.
 *
 *      Format:
 *           32 bits         32 bits                  64 bits
 *      +----------------+---------------+----------+----------------+
 *      | literal length | repeat length | literals | word to repeat |
 *      +----------------+---------------+----------+----------------+
 */

#include "udf.h"
#include "xrle.h"

#define U64 ULONGLONG
#define U32 ULONG

SIZE_T xrle_compress(void * out, const void * in, SIZE_T in_size)
{
    U64 *in_pos = (U64 *) in, *out_pos = (U64 *) out;
    U32 tail = (U32)(in_size & 7), *descr, repeat;
    U64 *in_limit = (U64 *)((char *)in + in_size - tail), previous, next;

    if (in_size < 16) {
        RtlCopyMemory(out, in, in_size);
        return in_size;
    }
    /*
     * Main encode loop. Each iteration emits one block descriptor followed
     * by the literal run and the repeated word.  The loop terminates via
     * goto when the end of the input buffer is reached:
     *   - 'end'     – no repeated word found before end-of-input
     *   - 'end_all' – the repeated run extended to end-of-input
     */
    for (;;) {
        repeat = 1;
        descr = (U32 *)out_pos;
        out_pos++;
        next = *in_pos;

        do {
            in_pos++;
            previous = next;
            if (in_pos == in_limit)
                goto end;
            next = *in_pos;
            *out_pos++ = previous;
        } while (previous != next);

        do {
            in_pos++;
            repeat++;
            if (in_pos >= in_limit)
                goto end_all;

        } while (previous == *in_pos);

        /* encode */
        descr[0] = (U32)(out_pos - ((U64 *)descr) - 2);
        descr[1] = repeat;
    }
end:
    *out_pos++ = previous;
end_all:
    descr[0] = (U32)(out_pos - ((U64 *)descr) - 2);
    descr[1] = repeat;
    RtlCopyMemory(out_pos, (char *)in + in_size - tail, tail);
    return (char *)out_pos - (char *)out + tail;
}

SIZE_T xrle_decompress(void * out, const void * in, SIZE_T in_size)
{
    U64 *in_pos = (U64 *) in, *out_pos = (U64 *) out, word;
    U32 tail = (U32)(in_size & 7), lit_len, repeat, i;
    U64 *in_limit = (U64 *)((char *)in + in_size - tail);

    if (in_size < 16) {
        RtlCopyMemory(out, in, in_size);
        return in_size;
    }
    while (in_pos < in_limit) {
        lit_len = ((U32 *) in_pos)[0];
        repeat = ((U32 *) in_pos)[1];
        in_pos++;

        for (i = 0; i < lit_len; i++)
            out_pos[i] = in_pos[i];

        in_pos += lit_len;
        out_pos += lit_len;

        word = *in_pos++;

        for (i = 0; i < repeat; i++)
            out_pos[i] = word;
        out_pos += repeat;
    }
    RtlCopyMemory(out_pos, (char *)in + in_size - tail, tail);
    return (char *)out_pos - (char *)out + tail;
}
