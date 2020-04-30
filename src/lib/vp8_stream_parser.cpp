/*
 * Copyright (c) 2018-2020  AIRTAME ApS
 * All Rights Reserved.
 *
 * See LICENSE.txt for further information.
 */

#include <assert.h>
#include <string.h>
#include "ivf.h"
#include "vp8_stream_parser.hpp"


namespace airtame {

/* Important: this code assumes that there is always single VP8 frame per buffer,
 and that it starts at offset #0. This is because raw VP8 contains not enough data
 to let us succesfully skip the frame without doing full decoding */
void VP8StreamParser::process_buffer(const VideoBuffer &buffer)
{
    /* RFC 6386, 9.1, "Uncompressed Data Chunk" */
    if (buffer.size < 3) {
        codec_log_error(m_logger, "VP8 frame data truncated");
        return;
    }
    /* Read keyframe, version and show_frame flag */
    uint32_t chunk = buffer.data[0] | (buffer.data[1] << 8) | (buffer.data[2] << 16);
    // WTF: it should by exact opposite! But works this way
    // See https://tools.ietf.org/html/rfc6386#section-19.1
    std::string description("VP8 ");
    bool keyframe = (chunk & 0x1) ? false : true;
    chunk >>= 1;
    size_t version = chunk & 0x7;
    chunk >>= 3;
    bool show_frame = (chunk & 0x1) ? true : false;
    if (!show_frame) {
        description += "invisible ";
    }
    if (keyframe) {
        description += "keyframe ";
    } else {
        description += "frame ";
    }
    description += "version ";
    description += std::to_string(version);
    // TODO: support for "not showing frames"
    assert(show_frame);
    if (keyframe) {
        if (buffer.size < 10) {
            codec_log_error(m_logger, "VP8 keyframe data header truncated");
            return;
        }
        /* Keyframes have three byte fixed start code here */
        if ((0x9d != buffer.data[3]) || (0x01 != buffer.data[4]) || (0x2a != buffer.data[5])) {
            codec_log_error(m_logger, "VP8 keyframe does not contain start code");
            return;
        }
/* This code taken straight out of RFC */
#if defined(__ppc__) || defined(__ppc64__)
#define swap2(d) ((d & 0x000000ff) << 8) | ((d & 0x0000ff00) >> 8)
#else
#define swap2(d) d
#endif
        size_t width = swap2(*(unsigned short *)(buffer.data + 6)) & 0x3fff;
        size_t height = swap2(*(unsigned short *)(buffer.data + 8)) & 0x3fff;
        /* Start a new frame */
        m_frames.push_new_pack();
        m_frames.back().m_can_reopen_decoding = true;
        if ((m_geometry.m_true_width != width) || (m_geometry.m_true_height != height)) {
            /* Need sequence header before keyframe when geometry changes, and
             ONLY then - feeding sequence header before every keyframe more or
             less works, but for some streams it produces ugly artifacts on
             keyframes. I think this is VP8 bug */
            push_sequence_header(m_geometry.m_true_width, m_geometry.m_true_height);
            m_geometry = FrameGeometry(width, height);
        }
    } else {
        /* Interframe pushed on its own */
        m_frames.push_new_pack();
        m_frames.back().m_can_reopen_decoding = false;
    }
    push_frame(buffer, description);
    /* These are common to all the VP8 frames */
    m_frames.back().m_codec_type = CodecType::VP8;
    m_frames.back().m_geometry = m_geometry;
    /* All VP8 streams can use up to 4 reference frames (last keyframe, altref
     frame, golden frame and most recently decoded frame). I think particular
     streams can use less, but this is not signalled anyhow, so always put in 4
     */
    m_frames.back().m_maximum_number_of_reference_frames = 4;

    /* NO droppable frames for VP8, because every frame save for keyframes uses
     previous frame as one of references */
    m_frames.back().m_can_be_dropped = false;
    m_frames.back().m_is_complete = true;
    m_frames.back().meta = buffer.meta;
    m_frames.back().m_needs_reordering = false;
    m_frames.back().m_needs_flushing = false;
}

void VP8StreamParser::push_sequence_header(size_t width, size_t height)
{
    constexpr size_t ivf_header_size = 32;
    unsigned char *ivf_header = new unsigned char[ivf_header_size];
    ::memset(ivf_header, 0, ivf_header_size);
    /* Four bytes of magic number */
    ::memcpy(ivf_header, IVF_MAGIC_NUMBER, 4);
    /* Two bytes of version, zeroed */
    /* Two bytes of seq header size */
    *(uint16_t *)(ivf_header + 6) = ivf_header_size;
    /* Four bytes of VP8 FOURCC */
    ::memcpy(ivf_header + 8, IVF_VP8_FOURCC, 4);
    /* Four bytes of image width/height */
    *(uint16_t *)(ivf_header + 12) = width;
    *(uint16_t *)(ivf_header + 12) = height;
    /* Rest of the stuff, including frame rate, number of frames, etc we leave
     zeroed out */
    m_frames.push_chunk(ivf_header, ivf_header_size, "IVF sequence header");
    m_frames.attach_free_callback([ivf_header]{ delete[] ivf_header;});
}

void VP8StreamParser::push_frame(const VideoBuffer &buffer, const std::string &description)
{
    constexpr size_t frame_header_size = 3;
    uint32_t *header = new uint32_t[frame_header_size];
    ::memset(header, 0, frame_header_size * 4);
    /* First four bytes is the size, and next eight is presentation timestamp
     which we leave zeroed */
    header[0] = buffer.size;
    m_frames.push_chunk((const unsigned char *)header, frame_header_size * 4,
                        "IVF frame header");
    m_frames.attach_free_callback([header]{ delete[] header;});
    m_frames.push_chunk(buffer.data, buffer.size, description.c_str());
    m_frames.attach_free_callback(buffer.free_callback);
}
}
