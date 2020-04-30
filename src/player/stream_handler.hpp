/*
 * Copyright (c) 2019  AIRTAME ApS
 * All Rights Reserved.
 *
 * See LICENSE.txt for further information.
 */

#pragma once

#include "stream.hpp"
#include "vpu_output_frame.hpp"

namespace airtame {
class StreamHandler {
protected:
    Stream m_stream;
    VPUOutputFrame m_last_frame;
    size_t m_buffers_in;
    size_t m_buffers_out;

public:
    StreamHandler(Stream &stream)
        : m_stream(stream)
        , m_buffers_in(0)
        , m_buffers_out(0)
    {
    }

    virtual ~StreamHandler()
    {
    }

    virtual void offset(size_t off) = 0;
    virtual bool init() = 0;
    virtual bool step() = 0;
    virtual void swap() = 0;
    virtual bool is_interleaved() = 0;

    const VPUOutputFrame &get_last_frame()
    {
        return m_last_frame;
    }
    bool end()
    {
        return !m_stream.get_size_left() && (m_buffers_in == m_buffers_out);
    }
};
}
