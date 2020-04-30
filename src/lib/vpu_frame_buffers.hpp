/*
* Copyright (c) 2018-2020  AIRTAME ApS
* All Rights Reserved.
*
* See LICENSE.txt for further information.
*/

#pragma once

#include <vector>

#include "codec_common.hpp"
#include "codec_logger.hpp"
#include "vpu_dma_pointer.hpp"

namespace airtame {
struct VPUFrameMemoryAndMetadata {
    VPUDMAPointer dma;
    /* Metadata is assigned on the decode, and removed when frame is given for
     display. Could also use it as "not yet displayed" flag */
    std::shared_ptr<FrameMetaData> meta;
    /* This frame was given away for display ant not given back yet, it can't be
     used by decode. Note that inverse is not always true - when flag is false
     it doesn't mean that it is free, it might be a reference frame. This can be
     also used to count frames that are available for decoding - if number of
     frames with reserved_for_display flag is less than number of display frame
     buffers reserve, then we can decode */
    bool given_for_display = false;
    /* This flag means that frame was given for display but got returned, and
     before next decode starts, decoder needs to be told that frame is free by
     clearing display flag on that frame */
    bool clear_display_flag = false;
};

class VPUFrameBuffers {
private:
    CodecLogger &m_logger;

    /* All frame buffers will be of this size */
    size_t m_frame_buffer_size = 0;

    /* Decoding of video stream needs some basic number of "reference" frame
     buffers - these are the frames in the past or future, that motion
     compensation refers to (duh). Number depends on the codec and stream in
     question. To decode, one needs to have at least that number plus one
     (frame being decoded at the moment) */
    size_t m_number_of_reference_frame_buffers = 0;

    /* Display reserve - how many frames are there above required number of
     reference frames. That many frames can be sent out for display */
    size_t m_number_of_display_frame_buffers = 0;

    /* FrameBuffer array is required by the decoder, it contains all the
     FrameBuffers it can use and it is passed as continuous array to the
     vpu_DecRegisterFrameBuffer. m_frames is complementary, contains extra
     infomation we need for respective frame buffers */
    std::vector <FrameBuffer> m_decoder_buffers;
    std::vector <VPUFrameMemoryAndMetadata> m_frames;
public:
    VPUFrameBuffers(CodecLogger &logger)
        : m_logger(logger)
    {
    }

    /* This should be called to make sure that there are at least given number
     of both reference and display frame buffers, at least of given size,
     and it returns the pointer to the array of them as required when calling
     vpu_DecRegisterFrameBuffer. All of these will be available until next
     reserve() call */
    bool reserve(size_t frame_buffer_size, size_t number_of_reference_frame_buffers,
                 size_t number_of_display_frame_buffers, FrameBuffer *(&decoder_buffers));

    /* Frame with given physical addres is no longer needed for display purposes
     */
    void mark_frame_as_returned(unsigned long physical_address);
    // TODO: not really nice interface, as it is the only function that "knows"
    // about the decoder, think of other way
    bool return_frames_now(DecHandle decoder);
    bool has_frame_for_decoding() const;
    void frame_decoded(size_t index, const std::shared_ptr<FrameMetaData> &meta);
    void frame_to_be_given_for_display(size_t index, VPUDMAPointer &dma_return,
                                       std::shared_ptr<FrameMetaData> &meta_return);
    // Accessors
    size_t get_number_of_reference_frame_buffers() const
    {
        return m_number_of_reference_frame_buffers;
    }

    size_t get_number_of_display_frame_buffers() const
    {
        return m_number_of_display_frame_buffers;
    }
};
} // namespace airtame
