/*
 * Copyright (c) 2018-2019  AIRTAME ApS
 * All Rights Reserved.
 *
 * See LICENSE.txt for further information.
 */

#include <string.h>

#include "vpu_h264_decoder.hpp"

namespace airtame {

VPUH264Decoder::VPUH264Decoder(CodecLogger &logger, size_t number_of_display_frames,
                               bool wait_for_frames)
    : m_logger(logger)
    , m_number_of_display_frames(number_of_display_frames)
    , m_wait_for_frames(wait_for_frames)
{
    /* Create dumb session, to avoid nullptr checks */
    m_session.reset(
        new VPUDecodingSession(m_logger, m_stats, m_buffers, FrameGeometry(),
                               wait_for_frames, m_will_be_reordering));
}

bool VPUH264Decoder::init()
{
    /* Init buffers */
    if (m_buffers.init_for_h264()) {
        return true;
    } else {
        codec_log_error(m_logger, "Couldn't create buffers - DMA memory too small?");
        return false;
    }
}

/* This is universal function for feeding picture slices with associated
 parameter sets. Parser will pass picture slice, sps/pps as well as flags
 indicating what to do with them. If is_idr_slice flag is set then this is
 IDR (instant decoder refresh) image and we can (re)open the decoder.
 If do_activate_sps flag is set then this is IDR image slice and also decoder
 has to be reopened because SPS parameters have changed. Note that if there
 is new IDR picture but using identical SPS we do not reopen. Then,
 activate_pps matters only for non-idr images (because for IDR ones we feed both
 SPS/PPS anyway) and it signals whether PPS has to be fed before slice NAL */
bool VPUH264Decoder::feed_picture_slice_with_parameter_sets(Timestamp timestamp,
                                                            const unsigned char *sps, size_t sps_size,
                                                            const unsigned char *pps, size_t pps_size,
                                                            const unsigned char *slice_nal,
                                                            size_t slice_nal_size, bool is_idr_slice,
                                                            bool do_activate_sps, bool do_activate_pps)
{
    bool reopen = false;
    size_t required_size = sps_size + pps_size + slice_nal_size;

    /* Call this to make sure that buffers instance is aware of stream needs */
    m_buffers.update_wanted_bitstream_buffer_size(required_size);

    /* Reopening possible only in IDR slices */
    if (is_idr_slice) {
        /* Opening necessary if SPS has changed */
        reopen = do_activate_sps;
        /* Or session is closed - need a new one */
        if (m_session->is_closed()) {
            reopen = true;
        }
        /* Or need to change reordering */
        if (m_will_be_reordering != m_session->get_reordering()) {
            reopen = true;
        }
        /* Or resize input bitstream buffer. Note: we assume here that keyframe
         or more precisely, IDR slice will be biggest one. Which is not always
         true - extra big non-IDR slice will cause error and will break decoding
         in the middle of the sequence, and will resume on next IDE frame.
         Also: I understand that decoder gets slice buffer for keeping the
         decoded information from the slices, so there is never a need of keeping
         more than one slice NAL in input buffer */
        if (m_buffers.should_grow_bitstream_buffer()) {
            reopen = true;
        }
        /* We do not check for geometry change here, as geometry change means
         SPS change and that is signalled via do_activate_sps */
    }

    if (reopen) {
        /* Force PPS activation, too */
        do_activate_pps = true;
    }

    /* If we are reopening, make sure all frames were flushed from decoder
     and it was closed properly after end of sequence */
    if (reopen && !m_session->is_closed()) {
        /* It is safe to call it here */
        m_session->flush();
        if (!m_session->is_closed()) {
            /* Decoder has to wait for frames to be returned, so do not continue
             now */
            return false;
        }
    }

    if (reopen) {
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
                           " too small for upcoming NAL, growing to %.2fMB",
                           double(old_size) / (1024 * 1024), double(new_size) / (1024 * 1024));
        }

        /* Parse SPS manually */
        SpsNalInfo sps_info;
        if (!at_h264_get_sps_info(sps, sps_size, sps_info)) {
            codec_log_error(m_logger, "SPS parser failure");
            return false;
        }

        /* Get frame geometry info */
        FrameGeometry frame_geometry
            = FrameGeometry(sps_info.padded_frame_width, sps_info.padded_frame_height,
                            sps_info.true_frame_width, sps_info.true_frame_height,
                            sps_info.true_crop_offset_left, sps_info.true_crop_offset_top);

        /* This is useful when debugging, but there is no need to log every dimension
         change - it can happen a lot of times during Airplay session or single window
         streaming */
        codec_log_trace(m_logger, "Dimensions %zu x %zu", sps_info.padded_frame_width,
                        sps_info.padded_frame_height);

        /* Prepare frame buffer template for H264 decoding */
        FrameBuffer frame_buffer_template;
        size_t frame_buffer_size = VPU::h264_prepare_nv12_frame_buffer_template(
            frame_geometry.m_padded_width, frame_geometry.m_padded_height, frame_buffer_template);

        /* Create new session */
        m_session.reset(
            new VPUDecodingSession(m_logger, m_stats, m_buffers, frame_geometry, m_wait_for_frames,
                                   m_will_be_reordering));

        /* Open new session. It handles errors internally, that is we don't care
         about success or failure here, we can always feed frames */
        Buffer open_parts[2] = { { sps, sps_size }, { nullptr, 0 } };
        m_session->open(STD_AVC, frame_buffer_template, frame_buffer_size,
                        m_number_of_display_frames, timestamp, false, open_parts);
    }

    /* Now feed SPS/PPS and slice */
    Buffer feed_parts[3] = { { sps, do_activate_sps ? sps_size : 0 },
                             { pps, do_activate_pps ? pps_size : 0 },
                             { slice_nal, slice_nal_size } };

    return m_session->feed(timestamp, true, feed_parts);
}

bool VPUH264Decoder::feed_end_of_stream(const unsigned char *eos_nal, size_t size)
{
    m_session->flush();
    return true;
}

/* NAL that are not IDR frame slices enter here */
bool VPUH264Decoder::feed_other(const unsigned char *nal, size_t size)
{
    /* Call this to make sure that buffers instance is aware of stream needs */
    m_buffers.update_wanted_bitstream_buffer_size(size);

    /* Feed data */
    Buffer parts[3] = { { nal, size }, { nullptr, 0 }, { nullptr, 0 } };
    return m_session->feed(0, false, parts);
}

bool VPUH264Decoder::reset()
{
    codec_log_info(m_logger, "Decoder flush forced by upper level");
    m_session->flush();
    return true;
}

void VPUH264Decoder::set_reordering(bool reorder)
{
    /* Now the old code allowed calling this at any time, and immediately
     triggered hw decoder reopening which is probably bad idea, because
     proper opening requires IDR picture.

     So right now we just set new value, and on next IDR picture we'll reopen
     decoder */
    m_will_be_reordering = reorder;
}
} // namespace airtame
