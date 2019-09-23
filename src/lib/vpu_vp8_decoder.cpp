/*
 * Copyright (c) 2018-2019  AIRTAME ApS
 * All Rights Reserved.
 *
 * See LICENSE.txt for further information.
 */

#include "vpu_vp8_decoder.hpp"
#include "ivf.h"
#include <string.h>

namespace airtame {

VPUVP8Decoder::VPUVP8Decoder(CodecLogger &logger, size_t number_of_display_frames,
                             bool wait_for_frames)
    : m_logger(logger)
    , m_number_of_display_frames(number_of_display_frames)
    , m_wait_for_frames(wait_for_frames)
{
    /* Create dumb session, to avoid nullptr checks */
    m_session.reset(new VPUDecodingSession(m_logger, m_stats, m_buffers, FrameGeometry(),
                                           wait_for_frames, false));
}

bool VPUVP8Decoder::init()
{
    if (m_buffers.init_for_vp8()) {
        return true;
    } else {
        codec_log_error(m_logger, "Couldn't create buffers - DMA memory too small?");
        return false;
    }
}

bool VPUVP8Decoder::feed_keyframe(Timestamp timestamp, const unsigned char *keyframe, size_t size,
                                  size_t width, size_t height)
{
    /* Combined IVF headers have 44 bytes. Call this to make sure that buffers
     instance is aware of stream needs */
    m_buffers.update_wanted_bitstream_buffer_size(size + 44);

    /* Proper geometry can be constructed out of width and height, because these
     are true picture dimensions, and so one can compute padded dimensions.
     Moreover VP8 doesn't have crop rectangle, so leaving those at zero */
    FrameGeometry new_frame_geometry(width, height);
    /* If session is closed, or if geometry has changed, we need to reopen */
    if (m_session->is_closed() || (new_frame_geometry != m_session->get_frame_geometry())
        || m_buffers.should_grow_bitstream_buffer()) {
        /* Yeah, we are reopening */
        if (!m_session->is_closed()) {
            m_session->flush();
            if (!m_session->is_closed()) {
                /* Decoder has to wait for frames to be returned, so do not continue
                 now */
                return false;
            }
        }

        /* Make sure there is enough place in bitstream input buffer - BTW we can
         do it because after flushing decoder is closed anyway */
        if (m_buffers.should_grow_bitstream_buffer()) {
            size_t old_size = (size_t)m_buffers.get_bitstream_buffer().size;
            if (!m_buffers.grow_bitstream_buffer()) {
                codec_log_fatal(m_logger, "Failed to grow bitstream input buffer");
                return true; /* Make compiler happy */
            }
            size_t new_size = (size_t)m_buffers.get_bitstream_buffer().size;
            codec_log_warn(m_logger,
                           "Old bitstream input buffer size %.2fMB was "
                           " too small for upcoming frame, growing to %.2fMB",
                           double(old_size) / (1024 * 1024), double(new_size) / (1024 * 1024));
        }

        /* Prepare frame buffer template for VP8 decoding */
        FrameBuffer frame_buffer_template;
        size_t frame_buffer_size = VPU::prepare_nv12_frame_buffer_template(
            new_frame_geometry.m_padded_width, new_frame_geometry.m_padded_height,
            frame_buffer_template);

        /* Prepare IVF sequence header and frame header rolled in one, for first
         keyframe in stream.
         WARNING: technically we should ensure that values are little endian.
         But honestly, this is module written for particular SoC in mind, so
         it is not likely we're ever going to compile it for another chip/OS */
        unsigned char combined_ivf_headers[44];
        ::memset(combined_ivf_headers, 0, sizeof(combined_ivf_headers));
        /* Four bytes of magic number */
        ::memcpy(combined_ivf_headers, IVF_MAGIC_NUMBER, 4);
        /* Two bytes of version, zeroed */
        /* Two bytes of seq header size */
        *(uint16_t *)(combined_ivf_headers + 6) = 32;
        /* Four bytes of VP8 FOURCC */
        ::memcpy(combined_ivf_headers + 8, IVF_VP8_FOURCC, 4);
        /* Four bytes of image width/height */
        *(uint16_t *)(combined_ivf_headers + 12) = new_frame_geometry.m_true_width;
        *(uint16_t *)(combined_ivf_headers + 14) = new_frame_geometry.m_true_height;
        /* Four bytes of frame rate numerator and denominator */
        *(uint16_t *)(combined_ivf_headers + 16) = 1;
        *(uint16_t *)(combined_ivf_headers + 18) = 1;
        /* Rest of the stuff, including number of frames we leave zeroed */
        /* Now frame header, four bytes of frame size */
        *(uint32_t *)(combined_ivf_headers + 32) = size;
        /* 8 byte presentation timestamp we leave zeroed, too */

        /* Create new session */
        m_session.reset(new VPUDecodingSession(m_logger, m_stats, m_buffers, new_frame_geometry,
                                               m_wait_for_frames, false));
        /* Open new session. It handles errors internally, that is we don't care
         about success or failure here, we can always feed frames */
        Buffer open_parts[2]
            = { { combined_ivf_headers, sizeof(combined_ivf_headers) }, { keyframe, size } };
        m_session->open(STD_VP8, frame_buffer_template, frame_buffer_size,
                        m_number_of_display_frames, timestamp, true, open_parts);
        return true; /* Keyframe taken */
    } else {
        /* Just feed keyframe with IVF frame header */
        return wrap_and_feed_frame(timestamp, keyframe, size);
    }
}

bool VPUVP8Decoder::feed_interframe(Timestamp timestamp, const unsigned char *frame, size_t size)
{
    /* Frame IVF header have 12 bytes. Call this to make sure that buffers
     instance is aware of stream needs */
    m_buffers.update_wanted_bitstream_buffer_size(size + 12);

    return wrap_and_feed_frame(timestamp, frame, size);
}

bool VPUVP8Decoder::reset()
{
    codec_log_info(m_logger, "Decoder flush forced by upper level");
    m_session->flush();
    return true;
}

bool VPUVP8Decoder::wrap_and_feed_frame(Timestamp timestamp, const unsigned char *frame, size_t size)
{
    uint32_t header[3];
    ::memset(header, 0, sizeof(header));
    /* First four bytes is the size, and next eight is presentation timestamp
     which we leave zeroed */
    header[0] = size;
    Buffer parts[3] = { { (const unsigned char *)header, sizeof(header) }, { frame, size } };
    return m_session->feed(timestamp, true, parts);
}
} // namespace airtame
