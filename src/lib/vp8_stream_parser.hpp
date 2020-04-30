/*
 * Copyright (c) 2018-2020  AIRTAME ApS
 * All Rights Reserved.
 *
 * See LICENSE.txt for further information.
 */

#pragma once

#include "codec_logger.hpp"
#include "pack_queue.hpp"

namespace airtame {

class VP8StreamParser {
private:
    CodecLogger &m_logger;

    /* Frame list */
    PackQueue &m_frames;

    /* Last keyframe geometry */
    FrameGeometry m_geometry;

public:
    VP8StreamParser(CodecLogger &logger, PackQueue &frames)
        : m_logger(logger)
        , m_frames(frames)
    {
    }
    /* Call this with buffers containing complete, single VP8 (not IVF or other
     container!) frames */
    void process_buffer(const VideoBuffer &buffer);
private:
    void push_sequence_header(size_t width, size_t height);
    void push_frame(const VideoBuffer &buffer, const std::string &description);
};
}
