/*
 * Copyright (c) 2019  AIRTAME ApS
 * All Rights Reserved.
 *
 * See LICENSE.txt for further information.
 */

#include "vp8_stream_handler.hpp"

namespace airtame {
VP8StreamHandler::VP8StreamHandler(Stream &stream, bool wait_for_frames)
    : StreamHandler(stream)
    , m_number_of_display_frames(2)
    , m_wait_for_frames(wait_for_frames)
{
    const unsigned char *read_pointer = m_stream.get_read_pointer();
    /* Read width, height and number of frames off file */
    size_t header_size = *(uint16_t *)(read_pointer + 6);
    size_t width = *(uint16_t *)(read_pointer + 12);
    size_t height = *(uint16_t *)(read_pointer + 14);
    size_t number_of_frames = *(uint32_t *)(read_pointer + 24);
    fprintf(stderr,
            "IVF file contains %zu frames, resolution %zux%zu, "
            "header size %zu\n",
            number_of_frames, width, height, header_size);
    if (header_size > m_stream.get_size_left()) {
        fprintf(stderr, "IVF header size bigger than file size");
        /* Flush stream to avoid further processing attempts */
        m_stream.flush_bytes(m_stream.get_size_left());
    } else {
        /* Flush header just read */
        m_stream.flush_bytes(header_size);
    }
}

void VP8StreamHandler::offset(size_t off)
{
    if (off > m_stream.get_size_left()) {
        m_stream.flush_bytes(m_stream.get_size_left());
    } else {
        while (off) {
            /* Read in next frame size from frame header */
            size_t frame_size = *(uint32_t *)m_stream.get_read_pointer();
            frame_size += 12; // size doesn't include frame header
            if (frame_size <= m_stream.get_size_left()) {
                m_stream.flush_bytes(frame_size);
                if (off < frame_size) {
                    /* offset was in this frame, so will start with next one */
                    off = 0;
                } else {
                    off -= frame_size;
                }
            } else {
                m_stream.flush_bytes(m_stream.get_size_left());
                off = 0;
            }
        }
    }
}

bool VP8StreamHandler::init()
{
    m_decoder = createVP8Decoder(m_logger, m_number_of_display_frames,
                                 m_wait_for_frames);
    if (!m_decoder) {
        fprintf(stderr, "Couldn't create VP8 decoder");
        return false;
    }
    return m_decoder->init();
}

bool VP8StreamHandler::step()
{
    while (!m_decoder->has_output_frame()) {
        if (end()) {
            return false; /* No new frame */
        }

        if (m_stream.get_size_left()) {
            /* Make sure there is enough data for frame size */
            if (m_stream.get_size_left() < 4) {
                fprintf(stderr, "Unexpected end of stream");
                m_stream.flush_bytes(m_stream.get_size_left());
                continue;
            }

            /* Read in next frame size from frame header. We assume that stream
             read pointer is always at the next frame as IVF/VP8 has no resync
             features. */
            const unsigned char *read_pointer = m_stream.get_read_pointer();
            size_t frame_size = *(uint32_t *)read_pointer;
            if ((frame_size + 12) <= m_stream.get_size_left()) {
                // TODO: we are passing zero as timestamp, but IVF contains 64-bit
                // timestamps just after frame size, so could extract them there
                /* Feed in one NAL. We don't need to release the buffer in any way, so
                 just count number of processed buffers */
                auto count = [this]() {
                    ++m_buffers_out;
                };
                VideoBuffer buffer;
                buffer.timestamp = 0;
                buffer.data = read_pointer + 12,
                buffer.size = frame_size;
                buffer.free_callback = count;
                m_decoder->push_buffer(buffer);
                ++m_buffers_in;

                /* Move on stream */
                m_stream.flush_bytes(frame_size + 12);
            } else {
                fprintf(stderr, "EOF inside of IVF frame");
                m_stream.flush_bytes(m_stream.get_size_left());
                continue;
            }
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

void VP8StreamHandler::swap()
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

bool VP8StreamHandler::is_interleaved()
{
    return true;
}
}
