/*
 * Copyright (c) 2018-2019  AIRTAME ApS
 * All Rights Reserved.
 *
 * See LICENSE.txt for further information.
 */

#pragma once
#include <stddef.h>
#include <memory>

#include "codec_logger.hpp"
#include "vpu_decoder_buffers.hpp"
#include "vpu_decoding_session.hpp"

namespace airtame {

class VPUVP8Decoder {
private:
    CodecLogger &m_logger;
    DecodingStats m_stats;

    size_t m_number_of_display_frames = 0;
    bool m_wait_for_frames;

    /* Decoder buffers that are shared between subsequent decoding session
     instances*/
    VPUDecoderBuffers m_buffers;

    /* Current decoding session - there is always one but it might be closed */
    std::unique_ptr<VPUDecodingSession> m_session;

public:
    VPUVP8Decoder(CodecLogger &logger, size_t number_of_display_frames,
                  bool wait_for_frames);
    bool init();

    /* Frame feeding functions
     IMPORTANT: these functions all return a boolean, indicating whether
     given frame was consumed or not. If it wasn't consumed, it should be
     fed next time. This is to allow for lengthy procedures, like flushing
     the decoder to execute completely - decoder will consume the frame when
     it is ready for it.
     Also, there is no need for error handling, decoder handles errors
     internally. This is because on error we have to close and re-open the
     decoder, and opening decoder again is possible only in certain situations
     (basically upon keyframe). So if there was an error, decoder flushes and
     shuts down, waiting for next keyframe. Meanwhile it will accept all the
     other frames and discard them silently, simply because there is no other
     way it could proceed. */
    bool feed_keyframe(Timestamp timestamp, const unsigned char *keyframe, size_t size,
                       size_t width, size_t height);
    bool feed_interframe(Timestamp timestamp, const unsigned char *frame, size_t size);
    bool reset();

    VPUDecodingSession &get_session()
    {
        return *m_session;
    }

    const VPUDecodingSession &get_session() const
    {
        return *m_session;
    }

    const DecodingStats &get_stats() const
    {
        return m_stats;
    }

    void set_reordering(bool)
    {
        /* Ignored */
    }

private:
    bool wrap_and_feed_frame(Timestamp timestamp, const unsigned char *frame,
                             size_t size);
};

typedef VPUVP8Decoder VP8Decoder;
}
