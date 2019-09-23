/*
 * Copyright (c) 2019  AIRTAME ApS
 * All Rights Reserved.
 *
 * See LICENSE.txt for further information.
 */

#pragma once

#include <functional>

#include "codec_common.hpp"
#include "codec_logger.hpp"
#include "vpu_output_frame.hpp"

namespace airtame {
    /* This is a interface class for VPU-based video stream decoders such as vp8
     and h264 (and others should we ever decide to write them). Common interface
     is required because controlling entity (such as Airtame pipeline module, or
     perhaps Chromium wrapper) doesn't really want to know what kind of stream
     it is handling, except perhaps when ordered to create particular subtype.
     So instead of having to create some kind of polymorphical solution wherever
     decoders are used, we provide one here */
    class IVPUDecoder {
    public:
        virtual ~IVPUDecoder() {}

        /* This function does real initialization of particular decoder instance
         like opening VPU library, allocating DMA memory and so on, with ctors
         providing only trivial stuff like setting initial values for members */
        virtual bool init() = 0;

        /* Use this function to push data into decoder. Buffers may contain
         one or more _complete_ stream chunks (h265 NALs, VP8 frames, ...).
         Fragments of chunks, or extra "trash" bytes are NOT allowed and will
         cause errors. Function may be called at any time, regardless of what
         decoder is doing. Calling it doesn't cause immediate feeding of the
         decoder, it will merely enqueue data for future use when decoder
         requests it. free_callback will be called once the whole buffer is
         consumed and isn't needed anymore */
        virtual void push_buffer(const VideoBuffer &buffer) = 0;

        /* Use this function to return frame that is no longer needed. Usually
         this will mean after it was displayed, or if all frames are being
         requested back */
        virtual void return_output_frame(unsigned long physical_address) = 0;

        /* If wait_for_frames ctor argument was true, then decoder will wait
         for all frames to return (via return_output_frame) before closing
         current session (and allocating frames again for next session). This
         conserves DMA memory usage and helps to reduce fragmentation, but isn't
         possible in some situations.

         If that function returns true decoder will be able to continue ONLY
         after all the frames that were popped out will get returned */
        virtual bool have_to_return_all_output_frames() = 0;

        /* By calling this function user can force decoder flushing (i.e. force
         output of all frames that have been buffered inside decoder). Callback
         will get called once flush will complete */
        using FlushCallback = std::function<void(void)>;
        virtual void start_flushing(FlushCallback flush_callback) = 0;

        /* This returns true if underlying decoder has finished decoding of all
         data that was pushed into it, got all the output frames back and shut
         down. It is intended to use with tests, for example when we want to
         push bitstream into decoder and test if all expected frames were
         produced. Closing occurs automatically, so there is nothing like
         "do close" counterpart. Also, this is probably not needed for anything
         other than testing. */
        virtual bool is_closed() const = 0;

        /* If this function returns true, one is allowed to use get_output_frame
         and pop_output_frame. Speaking in STL terms, these are !.empty(),
         .back() and .pop_back() */
        virtual bool has_output_frame() const = 0;
        /* Return frame at the very end of the output queue, i.e. next frame in
         display order */
        virtual const VPUOutputFrame &get_output_frame() const = 0;
        /* This pops the frame off output queue. Note that is wait_for_frames
         is true, then user is responsible for returning each frame by means of
         return_output_frame */
        virtual void pop_output_frame() = 0;

        /* Reordering flag on decoder. Currently just h264 decoders need this,
         but we also want opaque interface. It doesn't do any harm to set
         reordering for the codec that doesn't support it */
        virtual void set_reordering(bool reorder) = 0;

        /* There are basic decoding statistics that the user acting through
         this interface no longer able to measure. For example there is no
         "decode" method (decoding is hidden behind push_buffer() and
         recycle_out_frame(), and may do 0...n frames at once). So all the
         concrete decoders provide basic statistics themselves */
        virtual const DecodingStats &get_stats() const = 0;
    };

    extern IVPUDecoder *createH264Decoder(CodecLogger &logger, size_t number_of_display_frames,
                                          bool wait_for_frames);
    extern IVPUDecoder *createVP8Decoder(CodecLogger &logger, size_t number_of_display_frames,
                                         bool wait_for_frames);
}
