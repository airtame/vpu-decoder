/*
 * Copyright (c) 2019  AIRTAME ApS
 * All Rights Reserved.
 *
 * See LICENSE.txt for further information.
 */

#pragma once

#include "simple_logger.hpp"
#include "stream_handler.hpp"
#include "vpu_decoder.hpp"

namespace airtame {
class H264StreamHandler : public StreamHandler {
private:
    SimpleLogger m_logger;
    IVPUDecoder *m_decoder = nullptr;
    size_t m_number_of_display_frames;
    bool m_wait_for_frames;

public:
    H264StreamHandler(Stream &stream, bool wait_for_frames)
        : StreamHandler(stream)
        , m_number_of_display_frames(2)
        , m_wait_for_frames(wait_for_frames)
    {
    }

    virtual ~H264StreamHandler()
    {
        if (m_decoder) {
            delete m_decoder;
        }
    }

    void offset(size_t off);
    bool init();
    bool step();
    void swap();
    bool is_interleaved();
};
}
