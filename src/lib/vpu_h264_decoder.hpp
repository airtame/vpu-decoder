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
#include "h264_nal.hpp"
#include "vpu_decoder_buffers.hpp"
#include "vpu_decoding_session.hpp"

namespace airtame {

class VPUH264Decoder {
private:
    CodecLogger &m_logger;
    DecodingStats m_stats;

    size_t m_number_of_display_frames = 0;
    bool m_wait_for_frames = false;

    /* Reordering flags. You need to be careful with that because hw decoder
     doesn't let us switch reordering instantly, but only during opening
     (so after IDR frame). Changing reordering here changes how next open
     operation will set reordering, not how it is set now */
    bool m_will_be_reordering = false; /* How to open decoder next time? */

    /* Decoder buffers that are shared between subsequent decoding session
     instances*/
    VPUDecoderBuffers m_buffers;

    /* Current decoding session - there is always one but it might be closed */
    std::unique_ptr<VPUDecodingSession> m_session;

public:
    VPUH264Decoder(CodecLogger &logger, size_t number_of_display_frames,
                   bool wait_for_frames);
    bool init();

    /* NAL feeding functions
     IMPORTANT: these functions all return a boolean, indicating whether
     given NAL was consumed or not. If it wasn't consumed, it should be
     fed next time. This is to allow for lengthy procedures, like flushing
     the decoder to execute completely - decoder will consume the NAL when
     it is ready for it.
     Also, there is no need for error handling, decoder handles errors
     internally. This is because on error we have to close and re-open the
     decoder, and opening decoder again is possible only in certain situations
     (basically upon IDR NAL). So if there was an error, decoder flushes and
     shuts down, waiting for next IDR NAL. Meanwhile it will accept all the
     other NALs and discard them silently, simply because there is no other
     way it could proceed. */
    /* This is called with picture slice NALs. Parser makes sure that
     there are proper parameter sets to go with this picture slice
     and also signals if this is IDR picture and which parameter sets
     require activation. Returned value indicates whether NALs were
     fed succesfully. False means they'll have to be re-fed */
    bool feed_picture_slice_with_parameter_sets(Timestamp timestamp,
                                                const unsigned char *sps,
                                                size_t sps_size,
                                                const unsigned char *pps,
                                                size_t pps_size,
                                                const unsigned char *slice_nal,
                                                size_t slice_nal_size,
                                                bool is_idr_slice,
                                                bool do_activate_sps,
                                                bool do_activate_pps);
    /* This is end of stream NAL. It can also delay consumption to give
     decoder time to flush out all buffered frames */
    bool feed_end_of_stream(const unsigned char *eos_nal, size_t nal_size);
    /* This function is called with all other NALs. It cannot delay
     consumption on to flush stream, because these NALs cannot cause
     stream flushing. But it still can find space in decoder input
     lacking, or encounter error, and so it returns same consumed flag */
    bool feed_other(const unsigned char *nal, size_t size);
    /* This function is called to signal the decoder that some logic error
     has been detected in parser and so decoding should reset. It also
     returns consumed flag, because we try to flush output and it can take
     a while */
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
    /* Set reordering flag for next open_decoder() call */
    void set_reordering(bool reorder);
    /* Reordering as passed to the decoder during last open_decoder(), could
     differ from m_reordering which sets state that will be used during
     next open_decoder() call */
    bool get_reordering() const
    {
        return m_session->get_reordering();
    }
};

typedef VPUH264Decoder H264Decoder;
}
