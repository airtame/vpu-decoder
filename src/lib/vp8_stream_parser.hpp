/*
 * Copyright (c) 2018  AIRTAME ApS
 * All Rights Reserved.
 *
 * See LICENSE.txt for further information.
 */

#pragma once

/* Important: we include vpu_vp8_decoder.hpp, but use VP8Decoder type - this
 is to allow for alternative decoder implementations to be included on other
 platforms in the future. Each of platform-specific include files should do
 something like that:
 typedef PlatformSpecificVP8Decoder VP8Decoder;
 so this file can just use VP8Decoder everywhere */
#include "vpu_vp8_decoder.hpp"

namespace airtame {

class VP8StreamParser {
private:
    CodecLogger &m_logger;
    VP8Decoder &m_decoder;
    /* If this flag is true, then frames are passed to decoder. Otherwise, we
     are just skipping frames, looking for keyframe on which we'll
     attempt resyncing */
    bool m_synchronized = false;

public:
    VP8StreamParser(CodecLogger &logger, VP8Decoder &decoder)
        : m_logger(logger)
        , m_decoder(decoder)
    {
    }
    /* Call this with buffers containing complete, single VP8 (not IVF or other
     container!) frames. Number of bytes consumed is returned, and:
        - If it is zero, then call that function with same parameters again
        - If it is bigger, but smaller than length of the buffer, some data
        was consumed but not all, call this function again but passing
        updated data and length
        - If it equals length, then buffer can be discarted */
    size_t process_buffer(Timestamp timestamp, const unsigned char *data,
                          size_t length);
    bool reset();
private:
    bool go_out_of_sync();
};

}
