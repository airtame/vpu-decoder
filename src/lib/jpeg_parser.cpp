/*
 * Copyright (c) 2018-2019  AIRTAME ApS
 * All Rights Reserved.
 *
 * See LICENSE.txt for further information.
 */

#include <stdint.h>

#include "jpeg_parser.hpp"

const unsigned char *at_jpeg_next_marker(const unsigned char *ptr,
                                         const unsigned char *limit)
{
    /* Make sure we have "safe" initial contents of marker state */
    bool had_0xff = false;
    uint32_t marker = 0x0;
    while (ptr != limit) {
        /* JPEG markers have form:
         0xff, (byte between <1, 254>) */
        had_0xff = (0xff == marker);
        /* Load next byte */
        marker = *ptr++;
        if (had_0xff && (0 != marker) && (255 != marker)) {
            /* Found. It is safe to do -2, because to load start code
             ptr had to be incremented at least two times */
            return ptr - 2;
        }
    }
    /* Not found */
    return nullptr;
}
