/*
 * Copyright (c) 2019-2020  AIRTAME ApS
 * All Rights Reserved.
 *
 * See LICENSE.txt for further information.
 */

#include "vp8_stream_handler.hpp"

namespace airtame {
VP8StreamHandler::VP8StreamHandler(Stream &stream)
    : StreamHandler(stream)
    , m_number_of_display_frames(2)
    , m_parser(m_logger, m_packs)
    , m_decoder(m_logger, m_number_of_display_frames)
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
    m_fake_timestamp = 0;
    return true;
}

bool VP8StreamHandler::step()
{
    /* Yeah, this is ALMOST copy-paste from h264 version and it could be done
     in more clever, OO-way. But still, there might be some vp8-specific things
     we might want to add, also this is just an example. */
    while (!m_decoded_frame.has_data()) {
        /* Create at least one frame pack loading subsequent frames */
        while (!m_packs.has_pack_for_consumption() && load_frame()) ;

        /* No need to check completion of frames, VP8 frames are always complete */

        if (!m_packs.has_pack_for_consumption()) {
            /* This is the end */
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

void VP8StreamHandler::swap()
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

bool VP8StreamHandler::is_interleaved()
{
    return true;
}

bool VP8StreamHandler::load_frame()
{
    /* Make sure we are not on EOF */
    if (!m_stream.get_size_left()) {
        return false;
    }

    /* Make sure there is enough data for frame size */
    if (m_stream.get_size_left() < 4) {
        codec_log_error(m_logger, "Unexpected end of stream");
        m_stream.flush_bytes(m_stream.get_size_left());
        return false;
    }


    /* Read in next frame size from frame header. We assume that stream read
     pointer is always at the next frame as IVF/VP8 has no resync features. */
    const unsigned char *read_pointer = m_stream.get_read_pointer();
    size_t frame_size = *(uint32_t *)read_pointer;
    if ((frame_size + 12) <= m_stream.get_size_left()) {
        /* Wrap it up and send for processing */
        VideoBuffer buffer;
        buffer.data = read_pointer + 12;
        buffer.size = frame_size;
        // TODO: we are passing zero as timestamp, but IVF contains 64-bit
        // timestamps just after frame size, so could extract them there
        buffer.meta.reset(new FrameMetaData(m_fake_timestamp++));
        m_parser.process_buffer(buffer);

        /* Move on stream */
        m_stream.flush_bytes(frame_size + 12);
        return true;
    } else {
        fprintf(stderr, "EOF inside of IVF frame");
        m_stream.flush_bytes(m_stream.get_size_left());
        return false;
    }
}
}
