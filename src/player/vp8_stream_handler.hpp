/*
 * Copyright (c) 2019-2020  AIRTAME ApS
 * All Rights Reserved.
 *
 * See LICENSE.txt for further information.
 */

#pragma once

#include "pack_queue.hpp"
#include "simple_logger.hpp"
#include "stream_handler.hpp"
#include "vp8_stream_parser.hpp"
#include "vpu_decoder.hpp"

namespace airtame {
class VP8StreamHandler : public StreamHandler {
private:
    SimpleLogger m_logger;
    size_t m_number_of_display_frames;
    PackQueue m_packs;
    VP8StreamParser m_parser;
    VPUDecoder m_decoder;
    VPUOutputFrame m_decoded_frame;
    size_t m_fake_timestamp = 0;

public:
    VP8StreamHandler(Stream &stream);
    virtual ~VP8StreamHandler()
    {
    }

    void offset(size_t off);
    bool init();
    bool step();
    void swap();
    bool is_interleaved();

private:
    bool load_frame();
};
}
