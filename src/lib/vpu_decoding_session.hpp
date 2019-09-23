/*
 * Copyright (c) 2018-2019  AIRTAME ApS
 * All Rights Reserved.
 *
 * See LICENSE.txt for further information.
 */

#pragma once
#include <list>
#include <vector>

#include <assert.h>
#include <stdint.h>

extern "C" {
#include <vpu_io.h>
#include <vpu_lib.h>
}

#include "codec_common.hpp"
#include "codec_logger.hpp"
#include "h264_nal.hpp"
#include "vpu.hpp"
#include "vpu_bitstream_buffer_monitoring.hpp"
#include "vpu_decoder_buffers.hpp"
#include "vpu_dma_pointer.hpp"
#include "vpu_output_frame.hpp"

namespace airtame {

struct Buffer {
    const unsigned char *data;
    size_t size;
};

struct VPUFrameMemoryAndTimestamp {
    VPUDMAPointer dma;
    Timestamp timestamp;
    bool timestamp_assigned;
    bool reserved_for_display;
};

/* This is most important part of VPU decoder after refactoring. Original code
 was written with assumption that we just decode H264. With introduction of VP8
 it is no longer true. I tried obvious approach of putting common decoding
 routines in Decoder base class, and add derived H264Decode and VP8Decoder, but
 ended up having terrible OO design, with big convoluted objects sharing access
 to some parts of internal state, and relying on certain sequence of calls...
 In a word, unacceptable.

 Problem is, codec-specific and codec-agnostic code are not simple two layers,
 they are intertwined. For example how do you open/feed decoder (so something
 like "middle level") is very different in H264 (with SPSes, PPSes and whatnot)
 than in VP8 (with need for IVF wrapping). But then upper level (state machine
 that controls transitions) is exactly the same. On the other hand, low level
 VPU access stuff differs, but in the small details...

 Eventually I arrived at the current design which works as follows:
 DecodingSession class represents everything that happens with the hardware
 decoder from opening for certain conditions to end of decoding session. This
 is exactly how decoder is used - if parameters change you have to flush, close
 and open it again.

 Important thing is that this is not "H264 decoding" or "VP8 decoding" session.
 It stands for _any_ decoding session that follows open/process/flush/close
 semantics. And so some H264 or VP8 details that shows from time to time in
 the code (like reordering, or setting buffers such as H264's ps_save_buffer or
 VP8 mb_prediction_buffer) aren't (in my opinion, at least) breaking OO paradigm.
 This is how our particular HW works - one always passes reordering flag or
 all buffer types for all kinds of codecs it supports. But, depending on the
 particular codec in use, some of these flags/buffers are not used and may be
 simply set to zero. And in our case, upper level (so for example H264Decoder or
 VP8Decoder) handles this - in case of buffers by making sure only these that
 will be needed are actually created, and others will be passed as safe, zeroed
 out VPU memory structures, which simply translate to "buffer empty" information
 inside of VPU */

class VPUDecodingSession {
protected:
    CodecLogger &m_logger;
    DecodingStats &m_stats;
    VPUDecoderBuffers &m_buffers;

    /* These are session invariants that we get from upper level (VP8 or H264
     decoder). Note that despite frame geometry being added to output frames
     we retain this, because for example we need frame geometry before even
     getting first frame. */
    FrameGeometry m_frame_geometry;

    /* When this is true, decoding session will wait to be given back all frames
     before it will close itself. This minimizes peak DMA memory usage (as all
     memory used by session is guaranteed to be released before session gets
     closed) but may be a problem for some use cases (read: Chromium).

     If false, decoding session will close right away, relying on refcounting
     mechanism to eventually dispose of all frames. This may cause bigger DMA
     memory pressure, because next decoding session may be created before all
     frames from previous session were released. */
    bool m_should_wait_for_all_frames = false;

    /* This technically should be argument for open(), but is here to emphasize
     permanent character of switch, and also to use this as state variable for
     upper level */
    bool m_reordering = false;

    /* States of the session:
     - DECODER_CLOSED is initial state, VPU is initialized but decoder is not
     open yet, or is closed already after end of sequence. During the lifetime
     of session decoder may be opened once and then closed just once.
     - DECODER_OPEN is set after decoder was successfully opened for certain
     parameters, and this is the state that remains set during the duration of
     sequence decoding.
     - DECODER_FLUSHING state is needed because of the buffering of frames in
     the bitstream buffer/decoder. So when sequence ends and we want to decode
     another one (with, for example, different resolution) we cannot close/open
     decoder immediately, because then all buffered frames would be lost. What
     we need to do is to flush the decoder, to make sure we get all these frames
     out and only then we can close the decoder.
     - DECODER_WAITING_FOR_ALL_FRAMES occurs after hw decoder flushing is
     completed. At this stage decoder is not "holding" any frames, and all
     of them have been sent for display. But the problem is that display module
     may still have them (for example mapped to GPU texture), and decoder
     reopening may cause resolution change and then we'd need to release DMA
     memory of all frame buffers and allocate new frame buffers. Obviously
     if all frames haven't returned, we can't do that so we wait until we
     get all of them back.

     This state is optional, and session will get into it only if user opts-in
     by setting m_should_wait_for_all_frames to true. Waiting for all frames
     before reopening the decoder reduces peak DMA memory usage and
     fragmentation, but in some cases (Chromium) there is no way to get all the
     frames back.
     */
    enum class DecoderState {
        DECODER_CLOSED,
        DECODER_OPEN,
        DECODER_FLUSHING,
        DECODER_WAITING_FOR_ALL_FRAMES,
    };

    DecoderState m_state = DecoderState::DECODER_CLOSED;

    /* VPU wrapper and decoder handle */
    VPU m_vpu;
    DecHandle m_handle = nullptr;

    /* FrameBuffer array is required by the decoder, it contains all the
     FrameBuffers it can use and it is passed as continuous array to the
     H264VPU::register_framebuffers(). m_frames is complementary, contains
     DMA memory allocation and timestamp information for respective
     frame buffers */
    std::vector<FrameBuffer> m_frame_buffers;
    std::vector<VPUFrameMemoryAndTimestamp> m_frames;

    /* Buffer monitoring to keep track of timestmaps */
    VPUBitstreamBufferMonitoring m_bitstream_monitoring;

    /* Decoded (output) frames queue */
    VPUOutputFrameList m_output_frames;

    /* If that flag is true, decoder wants us to put more stuff into bitstream
     input buffer */
    bool m_wants_data = false;

public:
    VPUDecodingSession(CodecLogger &logger, DecodingStats &stats, VPUDecoderBuffers &buffers,
                       FrameGeometry frame_geometry, bool wait_for_frames, bool reordering);
    ~VPUDecodingSession();

    /* Session control functions - high level "state machine" stuff. Only these
     functions can trigger actual decoding */
    /* This can be called ONLY when session is closed */
    void open(CodStd bitstream_format, FrameBuffer frame_buffer_template, size_t frame_size,
              size_t number_of_display_frames, Timestamp timestamp, bool is_frame, Buffer parts[2]);
    /* This can be called ONLY when should_feed() returns true */
    bool feed(Timestamp timestamp, bool is_frame, Buffer parts[3]);
    /* These can be called in any state */
    void flush();
    void return_output_frame(unsigned long physical_address);

    /* If, and only if this function returns true, then feed() should be
     called. */
    bool should_feed() const
    {
        switch (m_state) {
            /* If decoder is closed, we always want data - it is only way to
             open the decoder after all */
            case DecoderState::DECODER_CLOSED: return true;
            /* If decoder is open, and decoding is in progress, then m_wants_data
             flag is valid, and we use it to signal feeding needs or lack of it */
            case DecoderState::DECODER_OPEN: return m_wants_data;
            /* All the other states (flushing/waiting for frames) won't accept
             data */
            default: return false;
        }
    }

    /* If this returns true module has to give all frames back to the
     decoder or it won't be able to continue */
    bool all_frames_needed_back() const
    {
        return DecoderState::DECODER_WAITING_FOR_ALL_FRAMES == m_state;
    }

    bool is_closed() const
    {
        return DecoderState::DECODER_CLOSED == m_state;
    }

    bool get_reordering() const
    {
        return m_reordering;
    }

    /* Even now that frame geometry is a part of output frame, this is still
     needed because decoder may want to know stream geometry even before first
     frame was sent out */
    const FrameGeometry &get_frame_geometry() const
    {
        return m_frame_geometry;
    }

    VPUOutputFrameList &get_output_frames()
    {
        return m_output_frames;
    }

    const VPUOutputFrameList &get_output_frames() const
    {
        return m_output_frames;
    }

protected:
    /* This is called on any decoder error */
    void decoder_error();

    /* Decoder handling utilities - low level stuff */
    bool open_decoder(CodStd bitstream_format, bool reordering);
    bool get_initial_info(Timestamp timestamp, bool is_frame, Buffer parts[2],
                          DecInitialInfo &initial_info);
    bool allocate_frames(FrameBuffer frame_buffer_template, size_t frame_size,
                         size_t minimal_frame_buffer_count,
                         size_t number_of_addional_display_frames);
    VPUDMAPointer allocate_dma(size_t size);
    bool feed_data(Buffer &part);
    bool wait_for_possible_decoded_frames(VPU::WaitResult &result);
    void close_decoder();
    bool all_frames_returned();

    static void vpu_error_function(const char *msg, void *user_data);
};
} // namespace airtame

