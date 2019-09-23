/*
 * Copyright (c) 2019  AIRTAME ApS
 * All Rights Reserved.
 *
 * See LICENSE.txt for further information.
 */

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
    m_decoder = createH264Decoder(m_logger, m_number_of_display_frames,
                                  m_wait_for_frames);
    if (!m_decoder) {
        fprintf(stderr, "Couldn't create h264 decoder");
        return false;
    }
    m_decoder->set_reordering(true);
    return m_decoder->init();
}

bool H264StreamHandler::step()
{
    /* Now, h264 is not simple, and there is no easy 1-1 relationship between
     NALs put in and frames what went out - one NAL may cause anything from
     zero to n frames out, several NALs may still not build single frame out,
     this is why this loop */
    while (!m_decoder->has_output_frame()) {
        if (end()) {
            return false; /* No new frame */
        }

        if (m_stream.get_size_left()) {
            /* Make sure there is enough data for start code */
            if (m_stream.get_size_left() < 4) {
                fprintf(stderr, "Unexpected end of stream");
                m_stream.flush_bytes(m_stream.get_size_left());
                continue;
            }

            /* Can feed in next NAL - for all but first frame it is expected at the
             stream read_pointer */
            const unsigned char *read_pointer = m_stream.get_read_pointer();
            if (!at_h264_next_start_code(read_pointer, read_pointer + 4)) {
                /* Nope, this happens when there is garbage at the start of the
                 file or perhaps after nonzero offset was skipped */
                const unsigned char *nal = at_h264_next_start_code(
                    read_pointer, read_pointer + m_stream.get_size_left());
                if (nal) {
                    /* OK, just skip all bytes before this start code */
                    m_stream.flush_bytes(nal - read_pointer);
                    read_pointer = m_stream.get_read_pointer();
                } else {
                    /* No NAL at all, guess offset was too big? Flush to end of
                     stream, so we won't re-enter */
                    m_stream.flush_bytes(m_stream.get_size_left());
                    return false; /* No new frame possible, then */
                }
            }

            /* Look for next start code in the stream _after_ current start code */
            const unsigned char *next_nal = at_h264_next_start_code(
                read_pointer + 4, read_pointer + m_stream.get_size_left() - 4);

            /* Size is either to next code - if found - or all remaining bytes */
            size_t size = next_nal ? next_nal - read_pointer : m_stream.get_size_left();

            /* Feed in one NAL. We don't need to release the buffer in any way, so
             just count number of processed buffers */
            auto count = [this]() {
                ++m_buffers_out;
            };
            VideoBuffer buffer;
            buffer.timestamp = 0;
            buffer.data = read_pointer;
            buffer.size = size;
            buffer.free_callback = count;
            m_decoder->push_buffer(buffer);
            ++m_buffers_in;

            /* Move on stream */
            m_stream.flush_bytes(size);
        }

        /* Check if we should return frames. This doesn't do any harm if
         decoder was opened in "don't wait for frames" mode. Do it only when
         output queue is empty, or we miss some display frames */
        if (m_decoder->have_to_return_all_output_frames() && !m_decoder->has_output_frame()
            && m_last_frame.dma) {
            m_decoder->return_output_frame(
                m_last_frame.dma.get()->phy_addr
            );
            m_last_frame.dma.reset();
        }
    }

    return true;
}

void H264StreamHandler::swap()
{
    if (m_last_frame.dma && m_decoder->has_output_frame()) {
        /* We have frame _and_ decoder has next frame ready, can return current
         one */
        m_decoder->return_output_frame(
            m_last_frame.dma.get()->phy_addr
        );
    }

    if (m_decoder->has_output_frame()) {
        /* Update m_last_frame */
        m_last_frame = m_decoder->get_output_frame();
        m_decoder->pop_output_frame();
    }
}

bool H264StreamHandler::is_interleaved()
{
    return true;
}
}
