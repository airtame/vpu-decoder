/*
 * Copyright (c) 2018-2019  AIRTAME ApS
 * All Rights Reserved.
 *
 * See LICENSE.txt for further information.
 */

#pragma once

#include <stddef.h>
extern "C" {
#include <vpu_io.h>
#include <vpu_lib.h>
}

namespace airtame {
class VPU {
public:
    typedef void (*ErrorFunction)(const char *msg, void *user_data);
private:
    ErrorFunction m_error_function;
    void *m_error_function_user_data;
public:
    VPU(ErrorFunction error_function, void *user_data)
        : m_error_function(error_function)
        , m_error_function_user_data(user_data)
    {
    }
    /* Result returned from wait_for_decoding. Note that it maybe NOTHING and
     that isn't a problem. And this is enum, not enum class, because I wanted
     bitwise operations and looks like it isn't easy to implement them if enum
     class itself is in another class scope */
    enum WaitResult {
        NOTHING = 0x0,
        DECODED_FRAME = 0x1,
        FRAME_AVAILABLE_FOR_DISPLAY = 0x2,
        FRAME_DROPPED = 0x4,
        END_OF_SEQUENCE = 0x8,
        NOT_ENOUGH_INPUT_DATA = 0x10,
        NOT_ENOUGH_OUTPUT_BUFFERS = 0x20,
        PARAMETERS_CHANGED = 0x40,
        WAIT_TIMEOUT = 0x80
    };

    /* Common functions */
    static size_t prepare_nv12_frame_buffer_template(size_t frame_width,
                                                     size_t frame_height,
                                                     FrameBuffer &frame_buffer);
    bool allocate_buffer(vpu_mem_desc &buffer);
    bool get_initial_info(DecHandle handle, DecInitialInfo &initial_info);
    bool get_bitstream_buffer_free_space_available(DecHandle handle, size_t& size);
    bool get_bitstream_buffer_read_index(DecHandle handle,
                                         const vpu_mem_desc &bitstream_buffer,
                                         size_t &read_index);
    bool get_bitstream_buffer_write_index(DecHandle handle,
                                          const vpu_mem_desc &bitstream_buffer,
                                          size_t &write_index);
    bool feed_data(DecHandle m_handle, const vpu_mem_desc &bitstream_buffer,
                   const unsigned char *data, size_t size);
    bool wait_for_decoding(DecHandle handle,
                           DecParam &params,
                           WaitResult &result, int &decoded_frame_index,
                           int &display_frame_index);
    /* H264 functions */
    static size_t h264_get_recommended_bitstream_buffer_size();
    static size_t h264_get_recommended_ps_save_buffer_size();
    static size_t h264_get_recommended_slice_buffer_size();
    
    static size_t h264_prepare_nv12_frame_buffer_template(size_t frame_width,
                                                          size_t frame_height,
                                                          FrameBuffer &frame_buffer);
    /* VP8 functions */
    static size_t vp8_get_recommended_bitstream_buffer_size();
    static size_t vp8_get_recommended_mb_prediction_buffer_size();
protected:
    void error(const char *msg);
};
}
