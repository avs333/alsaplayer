/*

libdemac - A Monkey's Audio decoder

$Id$

Copyright (C) Dave Chapman 2007

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program; if not, write to the Free Software
Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110, USA

*/

#include <string.h>
#include <inttypes.h>

#include "demac.h"
#include "filter.h"
#include "demac_config.h"

#ifdef ANDROID 
#include "vector_math16_armv7.h"
//#include "vector_math16_armv6.h"
//#include "vector_math16_armv5te.h"
//#include "vector_math_generic.h"
#else
#include "vector_math_generic.h"
#endif

#ifndef FILTER_TYPE_DEFINED
#define FILTER_TYPE_DEFINED
struct filter_t {
    filter_int* coeffs; /* ORDER entries */

    /* We store all the filter delays in a single buffer */
    filter_int* history_end;

    filter_int* delay;
    filter_int* adaptcoeffs;

    int avg;
};
#endif

/* We name the functions according to the ORDER and FRACBITS
   pre-processor symbols and build multiple .o files from this .c file
   - this increases code-size but gives the compiler more scope for
   optimising the individual functions, as well as replacing a lot of
   variables with constants.
*/

/* for apply/init */
#define _FLT_FUNC(A,B,C) A ## _filter_ ## B ## _ ## C
#define FLT_FUNC(A,B,C) _FLT_FUNC(A,B,C)

/* Some macros to handle the fixed-point stuff */

/* Convert from (32-FRACBITS).FRACBITS fixed-point format to an
   integer (rounding to nearest). */
#define FP_HALF  (1 << (FRACBITS - 1))   /* 0.5 in fixed-point format. */
#define FP_TO_INT(x) ((x + FP_HALF) >> FRACBITS)  /* round(x) */

#ifdef CPU_ARM
#if ARM_ARCH >= 6
#define SATURATE(x) ({int __res; asm("ssat %0, #16, %1" : "=r"(__res) : "r"(x)); __res; })
#else /* ARM_ARCH < 6 */
/* Keeping the asr #31 outside of the asm allows loads to be scheduled between
   it and the rest of the block on ARM9E, with the load's result latency filled
   by the other calculations. */
#define SATURATE(x) ({ \
    int __res = (x) >> 31; \
    asm volatile ( \
        "teq %0, %1, asr #15\n\t" \
        "moveq %0, %1\n\t" \
        "eorne %0, %0, #0xff\n\t" \
        "eorne %0, %0, #0x7f00" \
        : "+r" (__res) : "r" (x) : "cc" \
    ); \
    __res; \
})
#endif /* ARM_ARCH */
#else /* CPU_ARM */
#define SATURATE(x) (LIKELY((x) == (int16_t)(x)) ? (x) : ((x) >> 31) ^ 0x7FFF)
#endif

/* Apply the filter with state f to count entries in data[] */

static void ICODE_ATTR_DEMAC ST(do_apply_filter_3980)(struct filter_t* f,
                                                  int32_t* data, int count)
{
    int res;
    int absres; 

#ifdef PREPARE_SCALARPRODUCT
    PREPARE_SCALARPRODUCT
#endif

    while(LIKELY(count--))
    {
#ifdef FUSED_VECTOR_MATH
        if (LIKELY(*data != 0)) {
            if (*data < 0)
                res = ST(vector_sp_add)(f->coeffs, f->delay - ORDER,
                                    f->adaptcoeffs - ORDER);
            else
                res = ST(vector_sp_sub)(f->coeffs, f->delay - ORDER,
                                    f->adaptcoeffs - ORDER);
        } else {
            res = ST(scalarproduct)(f->coeffs, f->delay - ORDER);
        }
        res = FP_TO_INT(res);
#else
        res = FP_TO_INT(ST(scalarproduct)(f->coeffs, f->delay - ORDER));

        if (LIKELY(*data != 0)) {
            if (*data < 0)
                ST(vector_add)(f->coeffs, f->adaptcoeffs - ORDER);
            else
                ST(vector_sub)(f->coeffs, f->adaptcoeffs - ORDER);
        }
#endif

        res += *data;

        *data++ = res;

        /* Update the output history */
        *f->delay++ = SATURATE(res);

        /* Version 3.98 and later files */

        /* Update the adaption coefficients */
        absres = (res < 0 ? -res : res);

        if (UNLIKELY(absres > 3 * f->avg))
            *f->adaptcoeffs = ((res >> 25) & 64) - 32;
        else if (3 * absres > 4 * f->avg)
            *f->adaptcoeffs = ((res >> 26) & 32) - 16;
        else if (LIKELY(absres > 0))
            *f->adaptcoeffs = ((res >> 27) & 16) - 8;
        else
            *f->adaptcoeffs = 0;

        f->avg += (absres - f->avg) / 16;

        f->adaptcoeffs[-1] >>= 1;
        f->adaptcoeffs[-2] >>= 1;
        f->adaptcoeffs[-8] >>= 1;

        f->adaptcoeffs++;

        /* Have we filled the history buffer? */
        if (UNLIKELY(f->delay == f->history_end)) {
            memmove(f->coeffs + ORDER, f->delay - (ORDER*2),
                    (ORDER*2) * sizeof(filter_int));
            f->adaptcoeffs = f->coeffs + ORDER*2;
            f->delay = f->coeffs + ORDER*3;
        }
    }
}

static void ICODE_ATTR_DEMAC ST(do_apply_filter_3970)(struct filter_t* f,
                                                  int32_t* data, int count)
{
    int res;
    
#ifdef PREPARE_SCALARPRODUCT
    PREPARE_SCALARPRODUCT
#endif

    while(LIKELY(count--))
    {
#ifdef FUSED_VECTOR_MATH
        if (LIKELY(*data != 0)) {
            if (*data < 0)
                res = ST(vector_sp_add)(f->coeffs, f->delay - ORDER,
                                    f->adaptcoeffs - ORDER);
            else
                res = ST(vector_sp_sub)(f->coeffs, f->delay - ORDER,
                                    f->adaptcoeffs - ORDER);
        } else {
            res = ST(scalarproduct)(f->coeffs, f->delay - ORDER);
        }
        res = FP_TO_INT(res);
#else
        res = FP_TO_INT(ST(scalarproduct)(f->coeffs, f->delay - ORDER));

        if (LIKELY(*data != 0)) {
            if (*data < 0)
                ST(vector_add)(f->coeffs, f->adaptcoeffs - ORDER);
            else
                ST(vector_sub)(f->coeffs, f->adaptcoeffs - ORDER);
        }
#endif

        /* Convert res from (32-FRACBITS).FRACBITS fixed-point format to an
           integer (rounding to nearest) and add the input value to
           it */
        res += *data;

        *data++ = res;

        /* Update the output history */
        *f->delay++ = SATURATE(res);

        /* Version ??? to < 3.98 files (untested) */
        f->adaptcoeffs[0] = (res == 0) ? 0 : ((res >> 28) & 8) - 4;
        f->adaptcoeffs[-4] >>= 1;
        f->adaptcoeffs[-8] >>= 1;

        f->adaptcoeffs++;

        /* Have we filled the history buffer? */
        if (UNLIKELY(f->delay == f->history_end)) {
            memmove(f->coeffs + ORDER, f->delay - (ORDER*2),
                    (ORDER*2) * sizeof(filter_int));
            f->adaptcoeffs = f->coeffs + ORDER*2;
            f->delay = f->coeffs + ORDER*3;
        }
    }
}

static struct filter_t ST(filter)[2] IBSS_ATTR_DEMAC;

static void ST(do_init_filter)(struct filter_t* f, filter_int* buf)
{
    f->coeffs = buf;
    f->history_end = buf + ORDER*3 + FILTER_HISTORY_SIZE;

    /* Init pointers */
    f->adaptcoeffs = f->coeffs + ORDER*2;
    f->delay = f->coeffs + ORDER*3;

    /* Zero coefficients and history buffer */
    memset(f->coeffs, 0, ORDER*3 * sizeof(filter_int));

    /* Zero the running average */
    f->avg = 0;
}

void FLT_FUNC(init,ORDER,FRACBITS) (filter_int* buf)
{
    ST(do_init_filter)(&ST(filter)[0], buf);
    ST(do_init_filter)(&ST(filter)[1], buf + ORDER*3 + FILTER_HISTORY_SIZE);
}

void ICODE_ATTR_DEMAC FLT_FUNC(apply,ORDER,FRACBITS) (int fileversion, int channel,
                                   int32_t* data, int count)
{
    if (fileversion >= 3980)
        ST(do_apply_filter_3980)(&ST(filter)[channel], data, count);
    else
        ST(do_apply_filter_3970)(&ST(filter)[channel], data, count);
}

