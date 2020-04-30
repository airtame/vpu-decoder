/*
 * Copyright (c) 2019-2020  AIRTAME ApS
 * All Rights Reserved.
 *
 * See LICENSE.txt for further information.
 */

#pragma once

#include "h264_stream_parser.hpp"
#include "pack_queue.hpp"
#include "simple_logger.hpp"
#include "stream_handler.hpp"
#include "vpu_decoder.hpp"

namespace airtame {
class H264StreamHandler : public StreamHandler {
private:
    SimpleLogger m_logger;
    size_t m_number_of_display_frames;
    PackQueue m_packs;
    H264StreamParser m_parser;
    VPUDecoder m_decoder;
    VPUOutputFrame m_decoded_frame;
    size_t m_fake_timestamp = 0;

public:
    H264StreamHandler(Stream &stream)
        : StreamHandler(stream)
        , m_number_of_display_frames(2)
        , m_parser(m_logger, m_packs, false)
        , m_decoder(m_logger, m_number_of_display_frames)
    {
    }

    virtual ~H264StreamHandler()
    {
    }

    void offset(size_t off);
    bool init();
    bool step();
    void swap();
    bool is_interleaved();

private:
    bool load_nal();
};
}
