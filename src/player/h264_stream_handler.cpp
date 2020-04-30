/*
 * Copyright (c) 2019  AIRTAME ApS
 * All Rights Reserved.
 *
 * See LICENSE.txt for further information.
 */

#include "h264_nal.hpp"
#include "h264_stream_handler.hpp"

namespace airtame {

void H264StreamHandler::offset(size_t off)
{
    /* Super simple for h264, because of stream resync capabilities */
    if (off > m_stream.get_size_left()) {
        off = m_stream.get_size_left();
    }

    m_stream.flush_bytes(off);
}

bool H264StreamHandler::init()
{
    m_fake_timestamp = 0;
    return true;
}

bool H264StreamHandler::step()
{
    /* Now, h264 is not simple, and there is no easy 1-1 relationship between
     NALs put in and frames what went out - one NAL may cause anything from
     zero to n frames out, several NALs may still not build single frame out,
     this is why this loop */
    while (!m_decoded_frame.has_data()) {
        /* Create at least one frame pack loading subsequent NALs */
        while (!m_packs.has_pack_for_consumption() && load_nal()) ;

        if (!m_packs.empty() && !m_packs.front().m_is_complete) {
            codec_log_error(m_logger, "Incomplete frame pack at the end of input");
        }

        if (!m_packs.has_pack_for_consumption()) {
            codec_log_info(m_logger, "Fed %zu packs, was given %zu decoded frames",
                           m_packs.get_number_of_packs_popped(),
                           m_decoder.get_number_of_frames_given());
            return false;
        }

        /* OK, have complete frame pack, can try to decode */
        while (!m_decoded_frame.has_data() && m_decoder.has_frame_for_decoding()
               && m_packs.has_pack_for_consumption()) {
            m_decoded_frame = m_decoder.step(m_packs);
        }
    }

    return true;
}

void H264StreamHandler::swap()
{
    if (m_decoded_frame.has_data()) {
        if (m_last_frame.dma) {
            /* We have next frame to display, can give old one back */
            m_decoder.return_output_frame(m_last_frame.dma.get()->phy_addr);
        }

        /* Update m_last_frame */
        m_last_frame = m_decoded_frame;
        m_decoded_frame.reset();
    }
}

bool H264StreamHandler::is_interleaved()
{
    return true;
}

bool H264StreamHandler::load_nal()
{
    /* Make sure we are not on EOF */
    if (!m_stream.get_size_left()) {
        return false;
    }

    /* Make sure there is enough data for start code */
    if (m_stream.get_size_left() < 4) {
        codec_log_error(m_logger, "Unexpected end of stream");
        return false;
    }

    /* Can feed in next NAL - for all but first frame it is expected at the
        stream read_pointer */
    const unsigned char *read_pointer = m_stream.get_read_pointer();
    if (!at_h264_next_start_code(read_pointer, read_pointer + 4)) {
        /* Nope, this happens when there is garbage at the start of the file or
         perhaps after nonzero offset was skipped */
        const unsigned char *nal = at_h264_next_start_code(
            read_pointer, read_pointer + m_stream.get_size_left());
        if (nal) {
            /* OK, just skip all bytes before this start code */
            m_stream.flush_bytes(nal - read_pointer);
            read_pointer = m_stream.get_read_pointer();
        } else {
            /* No NAL at all, guess offset was too big? Flush to end of stream,
             so we won't re-enter */
            m_stream.flush_bytes(m_stream.get_size_left());
            return false; /* No new frame possible, then */
        }
    }

    /* Look for next start code in the stream _after_ current start code */
    const unsigned char *next_nal = at_h264_next_start_code(
        read_pointer + 4, read_pointer + m_stream.get_size_left() - 4);

    /* Size is either to next code - if found - or all remaining bytes */
    size_t size = next_nal ? next_nal - read_pointer : m_stream.get_size_left();

    /* Wrap it up and send for processing */
    VideoBuffer buffer;
    buffer.data = read_pointer;
    buffer.size = size;
    buffer.meta.reset(new FrameMetaData(m_fake_timestamp++));
    m_parser.process_buffer(buffer);

    /* Move on stream */
    m_stream.flush_bytes(size);

    if (!m_stream.get_size_left() && !m_packs.empty()
        && (!m_packs.back().m_is_complete || !m_packs.back().m_needs_flushing)) {
        codec_log_warn(m_logger, "Terminating stream at the end of input, no EOS detected");
        m_packs.back().m_is_complete = true;
        m_packs.back().m_needs_flushing = true;
    }

    /* Mark end*/
    return true;
}
}
