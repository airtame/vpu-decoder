/*
 * Copyright (c) 2019  AIRTAME ApS
 * All Rights Reserved.
 *
 * See LICENSE.txt for further information.
 */

#include "jpeg_stream_handler.hpp"
#include "vpu_jpeg_decoder.hpp"

namespace airtame {

void JPEGStreamHandler::offset(size_t off)
{
    if (off) {
        fprintf(stderr, "JPEG decoder doesn't handle nonzero offsets, ignoring\n");
    }
}

bool JPEGStreamHandler::init()
{
    if (!VPUJPEGDecoder::parse_jpeg_header(m_stream.get_read_pointer(),
                                           m_stream.get_size_left(),
                                           m_last_frame.geometry)) {
        /* Wrong JPEG format (like progressive) or not JPEG stream */
        return false;
    }
    fprintf(stderr, "JPEG file %zux%zu\n", m_last_frame.geometry.m_true_width,
            m_last_frame.geometry.m_true_height);

    m_last_frame.dma = VPUJPEGDecoder::produce_jpeg_frame(m_last_frame.geometry);
    return m_last_frame.dma ? true : false;
}

bool JPEGStreamHandler::step()
{
    if (end()) {
        /* Already decoded */
        return false;
    }

    /* Not decoded yet, need to load bitstream into DMA memory */
    VPUDMAPointer bitstream = VPUJPEGDecoder::load_bitstream(m_stream.get_read_pointer(),
                                                             m_stream.get_size_left());
    m_stream.flush_bytes(m_stream.get_size_left());
    return VPUJPEGDecoder::decode(m_last_frame.geometry, bitstream, m_last_frame.dma,
                                  m_interleave);
}

void JPEGStreamHandler::swap()
{
    // no-op
}

bool JPEGStreamHandler::is_interleaved()
{
    return m_interleave;
}
}
