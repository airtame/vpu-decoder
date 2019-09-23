/*
 * Copyright (c) 2019  AIRTAME ApS
 * All Rights Reserved.
 *
 * See LICENSE.txt for further information.
 */

#include "vpu_decoder.hpp"

#include "h264_stream_parser.hpp"
#include "vp8_stream_parser.hpp"

namespace airtame {
template <class ParserType, class DecoderType>
class VPUDecoderT : public IVPUDecoder {
private:
    /* Both h264 and vp8 decoding contains two elements, decoder proper
     (with decoding session inside) and stream parser that helps to feed
     decoder properly) */
    DecoderType m_decoder;
    ParserType m_parser;

    std::list<VideoBuffer> m_input_buffers;
    FlushCallback m_flush_callback = 0;

public:
    VPUDecoderT(CodecLogger &logger, size_t number_of_display_frames,
                bool wait_for_frames)
        : m_decoder(logger, number_of_display_frames, wait_for_frames)
        , m_parser(logger, m_decoder)
    {
    }

    ~VPUDecoderT()
    {
        while (!m_input_buffers.empty()) {
            /* We don't need that buffer anymore */
            m_input_buffers.front().free_callback();
            m_input_buffers.pop_front();
        }
    }

    bool init() override
    {
        return m_decoder.init();
    }

    void push_buffer(const VideoBuffer &buffer) override
    {
        if (m_flush_callback) {
            /* If we are flushing, we don't accept more data so release buffer
             immediately */
            buffer.free_callback();
        } else {
            /* Enqueue buffer */
            m_input_buffers.push_back(buffer);
            /* This only makes sense if m_input_buffers were empty, but it is safe
             to call it anyway */
            try_to_feed();
        }
    }

    void return_output_frame(unsigned long physical_address) override
    {
        /* It is always possible to return displayed frame to decoder */
        m_decoder.get_session().return_output_frame(physical_address);

        if (m_flush_callback) {
            /* If we are flushing, we are not feeding, just checking if the
             flush ended */
            if (m_decoder.get_session().is_closed()) {
                m_flush_callback();
                m_flush_callback = 0;
            }
        } else {
            /* Processing triggered by return of the frame may cause decoder
             to want new data, so give it a try */
            try_to_feed();
        }
    }

    bool have_to_return_all_output_frames() override
    {
        return m_decoder.get_session().all_frames_needed_back();
    }

    void start_flushing(FlushCallback flush_callback) override
    {
        m_flush_callback = flush_callback;
        m_decoder.get_session().flush();
        if (m_decoder.get_session().is_closed()) {
            /* Succeeded without returning frames */
            m_flush_callback();
            m_flush_callback = 0;
        }
    }

    bool is_closed() const override
    {
        return m_decoder.get_session().is_closed();
    }

    bool has_output_frame() const override
    {
        return !m_decoder.get_session().get_output_frames().empty();
    }

    const VPUOutputFrame &get_output_frame() const override
    {
        assert(!m_decoder.get_session().get_output_frames().empty());
        return m_decoder.get_session().get_output_frames().front();
    }

    void pop_output_frame() override
    {
        return m_decoder.get_session().get_output_frames().pop_front();
    }

    void set_reordering(bool reorder) override
    {
        m_decoder.set_reordering(reorder);
    }

    const DecodingStats &get_stats() const override
    {
        return m_decoder.get_stats();
    }

private:
    void try_to_feed()
    {
        if (m_input_buffers.empty()) {
            /* No input buffers, nothing to feed */
            return;
        }

        if (m_decoder.get_session().should_feed()) {
            /* Feed decoder (via stream parser). We do that ONLY if decoder
             wants data. Note - decoder still may decide not to consume
             data now and return zero, this is OK and may hapen for example
             when it encounters begin of new stream and decides to wait for
             frames to be given back before it re-opens */
            size_t consumed_bytes = m_parser.process_buffer(m_input_buffers.front().timestamp,
                                                            m_input_buffers.front().data,
                                                            m_input_buffers.front().size);
            m_input_buffers.front().data += consumed_bytes;
            m_input_buffers.front().size -= consumed_bytes;

            if (!m_input_buffers.front().size) {
                /* Buffer fully consumed, notify buffer owner and get rid of it */
                m_input_buffers.front().free_callback();
                m_input_buffers.pop_front();
            }
        }
    }
};

IVPUDecoder *createH264Decoder(CodecLogger &logger, size_t number_of_display_frames,
                               bool wait_for_frames)
{
    return new VPUDecoderT<H264StreamParser, H264Decoder>(logger, number_of_display_frames,
                                                          wait_for_frames);
}

IVPUDecoder *createVP8Decoder(CodecLogger &logger, size_t number_of_display_frames,
                              bool wait_for_frames)
{
    return new VPUDecoderT<VP8StreamParser, VP8Decoder>(logger, number_of_display_frames,
                                                        wait_for_frames);
}
}
