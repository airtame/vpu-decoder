/*
 * Copyright (c) 2019  AIRTAME ApS
 * All Rights Reserved.
 *
 * See LICENSE.txt for further information.
 */

#pragma once

#include "simple_logger.hpp"
#include "stream_handler.hpp"

namespace airtame {
class JPEGStreamHandler : public StreamHandler {
private:
    SimpleLogger m_logger;
    VPUDMAPointer m_bitstream;
    bool m_interleave;

public:
    JPEGStreamHandler(Stream &stream, bool interleave)
        : StreamHandler(stream)
        , m_interleave(interleave)
    {
    }

    virtual ~JPEGStreamHandler()
    {
    }

    void offset(size_t off);
    bool init();
    bool step();
    void swap();
    bool is_interleaved();
};
}
