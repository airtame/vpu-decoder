/*
 * Copyright (c) 2013-2019  AIRTAME ApS
 * All Rights Reserved.
 *
 * See LICENSE.txt for further information.
 */

#include "h264_bitstream.hpp"
#include <cassert>
#include <stdio.h>

/* Reads an unsigned integer of up to 24 bits taken from the bitstream.
 24 bits should suffice because longer values in H264 stream are encoded
 as Exp-Golomb codes, with zero-bit string indicating length of code (and
 number of less significant bits to follow. Now, we know that H264 start
 code is 23 zero bits followed by one (0x000001), and start-code emulation
 is carefully avoided in the stream. So Exp-Golob codes are probably limited
 to 22-long, and so reading 24 bits is enough.

 Previous version of code here used 64-bit values for extra safety which is
 unfortunately performance problem on 32-bit ARM CPU we have on the i.mx6 */
H264Bitstream::Result H264Bitstream::read_un_bits(size_t n)
{
    assert(n <= 24);

    H264Bitstream::Result error(0, true), ok(0, false);

    if (!n) {
        /* This behavior is specified by H.264 and read_uev_bits depends
         on it */
        return ok;
    }

    /* Make sure we have enough bits */
    while (m_bits_available < n) {
        if (!m_data_size) {
            /* Technically, H264 read_bits() returns zeros when reading beyond
             the stream end, but to detect errors we'd have to understand whole
             stream then, and that ain't practical */
            return error;
        }
        /* Load next byte (bit string in "stream order", significant bits first) */
        m_bits |= (*m_data++) << (24 - m_bits_available);
        m_bits_available += 8;
        --m_data_size;
    }

    /* OK, now we know we have at least n bits on top of m_bits, return these */
    ok.value = m_bits >> (32 - n);

    /* Shift these out */
    m_bits <<= n;
    m_bits_available -= n;

    return ok;
}

/* From ITU-T Rec. H.264 (05/2003, 7.4.2.1
 * ue(v): unsigned integer Exp-Golomb-coded syntax element with the left bit first.
 * The parsing process for this descriptor is specified in subclause 9.1.
 *
 * The parsing process for these syntax elements begins with reading the bits starting at the
 * current location in the bitstream up to and including the first non-zero bit, and counting the
 * number of leading bits that are equal to 0. This process shall be equivalent to the following:
 *
 * leadingZeroBits = -1;
 * for (b = 0; !b; leadingZeroBits++ )
 *      b = read_bits( 1 )
 *
 * The variable codeNum is then assigned as follows:
 *      codeNum = 2^leadingZeroBits – 1 + read_bits( leadingZeroBits )
 *
 * where the value returned from read_bits( leadingZeroBits ) is interpreted as a binary
 * representation of an unsigned integer with most significant bit written first.
 *
 */
H264Bitstream::Result H264Bitstream::read_uev_bits()
{
    H264Bitstream::Result error(0, true), ok(0, false);
    int leading_zero_bits = 0;
    /* Technically, this is how standard _describes_ Exp-Golomb code, and it doesn'
     * mean it really should be _implemented_ this way (reading bit by bit with buffer
     * bounds checking and all that stuff). Performance code should have at least
     * "pre load max size" (which we know is < 24, and we can buffer that) followed
     * by cheaper "peek n bits without bounds checking". Could even use some machine
     * "find topmost nonzero bit" instruction for really fast approach.
     *
     * But right now shooting for safe solution, not performant one */
    while (1) {
        auto leading_0_bit = read_un_bits(1);
        if (leading_0_bit.error) {
            return error;
        }
        if (leading_0_bit.value) {
            break;
        }
        leading_zero_bits++;
    }

    auto suffix_bits = read_un_bits(leading_zero_bits);
    if (suffix_bits.error) {
        return error;
    }

    /* We expect Exp-Golomb codes to have at most 22-bits long zero-prefix due to
     * H264 avoiding start code emulation
     */
    assert(leading_zero_bits < 23);

    /* Using 32-bit values here helps with performance on ARM, and Exp-Golomb codes
     that one can expect in H264 stream should fit in 23 bits anyway */
    ok.value = ((uint32_t)1 << leading_zero_bits) - 1 + suffix_bits.value;

    return ok;
}

/* See 9.1.1 "Mapping process for signed Exp-Golomb codes" */
H264Bitstream::Result H264Bitstream::read_sev_bits()
{
    /* Read unsigned Exp-Golomb code */
    Result result = read_uev_bits();
    if (result.error) {
        return result;
    }
    /* Convert value to signed */
    bool plus = result.value & 0x1; /* Even numbers are converted to positive ones */
    int32_t value = (result.value + 1) >> 1; /* ceil(value/2) */
    result.value = plus ? value : -value;
    return result;
}
