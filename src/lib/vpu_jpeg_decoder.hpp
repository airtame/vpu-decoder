/*
 * Copyright (c) 2018-2019  AIRTAME ApS
 * All Rights Reserved.
 *
 * See LICENSE.txt for further information.
 */

#pragma once

#include <stddef.h>

#include "codec_common.hpp"
#include "vpu_dma_pointer.hpp"

namespace airtame {
/* Unlike H264 and VP8 decoder this is extremely simple, and doesn't contain
 decoder/session internally. This is because 1) there is no such thing as
 JPEG decoding state, even for MJPEG we should be able to "one shot decode"
 frames, and that won't need any previous information and won't carry information
 for future either and 2) JPEG decoding is very simple */

// TODO: add logger to all the functions, so that details of why stream was
// rejected and other problems can be complained about
class VPUJPEGDecoder {
public:
    /* This is used to obtain width, height of the JPEG image in provided buffer.
     Returns false on error or if the image is not Baseline 420 */
    static bool parse_jpeg_header(const unsigned char *jpeg, size_t size,
                                  FrameGeometry &geometry);
    /* Load bitstream into DMA memory */
    static VPUDMAPointer load_bitstream(const unsigned char *jpeg, size_t size);
    /* Produce NV12 frame (compatible with other frames produced by our decoders)
     suitable for decoding Baseline 420 jpeg */
    static VPUDMAPointer produce_jpeg_frame(const FrameGeometry &geometry);
    /* Bitstream should contain all jpeg data, output should be frame created
     by routine above */
    static bool decode(const FrameGeometry &geometry, VPUDMAPointer bitstream,
                       VPUDMAPointer output, bool interleaved = true);

private:
    static void vpu_error_function(const char *msg, void *user_data);
};
}
