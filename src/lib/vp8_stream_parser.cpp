/*
 * Copyright (c) 2018  AIRTAME ApS
 * All Rights Reserved.
 *
 * See LICENSE.txt for further information.
 */

#include <assert.h>
#include "vp8_stream_parser.hpp"

namespace airtame {

/* Important: this code assumes that there is always single VP8 frame per buffer,
 and that it starts at offset #0. This is because raw VP8 contains not enough data
 to let us succesfully skip the frame without doing full decoding */

/* Uncomment this to log messages about frames being fed to the decoder */
//#define FRAME_LOGGING

size_t VP8StreamParser::process_buffer(Timestamp timestamp, const unsigned char *data, size_t length)
{
    /* RFC 6386, 9.1, "Uncompressed Data Chunk" */
    if (length < 3) {
        codec_log_error(m_logger, "VP8 frame data truncated");
        // TODO: we have a bug in h264 stream parser, should return size not bool!
        return go_out_of_sync() ? length : 0;
    }
    /* Read keyframe, version and show_frame flag */
    uint32_t chunk = data[0] | (data[1] << 8) | (data[2] << 16);
    // WTF: it should by exact opposite! But works this way
    // See https://tools.ietf.org/html/rfc6386#section-19.1
    bool keyframe = (chunk & 0x1) ? false : true;
    chunk >>= 1;
#ifdef FRAME_LOGGING
    size_t version = chunk & 0x7;
#endif
    chunk >>= 3;
#ifdef FRAME_LOGGING
    bool show_frame = (chunk & 0x1) ? true : false;
#endif
    if (keyframe) {
        if (length < 10) {
            codec_log_error(m_logger, "VP8 keyframe data header truncated");
            return go_out_of_sync() ? length : 0;
        }
        /* Keyframes have three byte fixed start code here */
        if ((0x9d != data[3]) || (0x01 != data[4]) || (0x2a != data[5])) {
            codec_log_error(m_logger, "VP8 keyframe does not contain start code");
            return go_out_of_sync() ? length : 0;
        }
/* This code taken straight out of RFC */
#if defined(__ppc__) || defined(__ppc64__)
#define swap2(d) ((d & 0x000000ff) << 8) | ((d & 0x0000ff00) >> 8)
#else
#define swap2(d) d
#endif
        size_t width = swap2(*(unsigned short *)(data + 6)) & 0x3fff;
        size_t height = swap2(*(unsigned short *)(data + 8)) & 0x3fff;
#ifdef FRAME_LOGGING
        size_t horiz_scale = swap2(*(unsigned short *)(data + 6)) >> 14;
        size_t vert_scale = swap2(*(unsigned short *)(data + 8)) >> 14;
        /* OK, now we can log and call decoder */
        codec_log_info(m_logger,
                       "%s keyframe (%zux%zu, scale %zux%zu, version %zu) @%" PRIi64 "ms"
                       " will be fed",
                       show_frame ? "Visible" : "Hidden", width, height, horiz_scale, vert_scale,
                       version, timestamp);
#endif
        return m_decoder.feed_keyframe(timestamp, data, length, width, height) ? length : 0;
    } else {
#ifdef FRAME_LOGGING
        codec_log_info(m_logger, "%s interframe (version %zu) @%" PRIi64 "ms will be fed",
                       show_frame ? "Visible" : "Hidden", version, timestamp);
#endif
        return m_decoder.feed_interframe(timestamp, data, length) ? length : 0;
    }
}

bool VP8StreamParser::reset()
{
    if (m_decoder.reset()) {
        m_synchronized = false;
        return true;
    } else {
        return false;
    }
}

bool VP8StreamParser::go_out_of_sync()
{
    if (m_synchronized) {
        /* Avoid repeating the message */
        codec_log_error(m_logger, "VP8 parser going out of sync till next keyframe");
        m_synchronized = false;
    }
    return m_decoder.reset(); /* We may need to re-enter here many times */
}
}
