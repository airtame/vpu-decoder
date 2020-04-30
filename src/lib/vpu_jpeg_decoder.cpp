/*
 * Copyright (c) 2018-2019  AIRTAME ApS
 * All Rights Reserved.
 *
 * See LICENSE.txt for further information.
 */

#include <stdio.h>
#include <string.h>

#include "jpeg_parser.hpp"
#include "vpu_jpeg_decoder.hpp"
#include "vpu_decoding_session.hpp"

namespace airtame {
    bool VPUJPEGDecoder::parse_jpeg_header(const unsigned char *jpeg, size_t size,
                                           FrameGeometry &geometry)
    {
        const unsigned char *limit = jpeg + size;
        const unsigned char *marker = at_jpeg_next_marker(jpeg, limit);

        while (marker) {
            /* Right now I am not doing real parsing of JPEG stream, just look
             for Start of frame family of markers */
            MarkerType m = (MarkerType)marker[1];
            if (MarkerType::SOF0 == m) {
                /* This is the one we understand and support */
                marker += 5; /* Skip two bytes of marker and two bytes of size
                              field and one byte of sample precision */
                /* Read two bytes of height */
                size_t height = (size_t)marker[0] * 256 + marker[1];
                marker += 2;
                /* Read two bytes of width */
                size_t width = (size_t)marker[0] * 256 + marker[1];
                marker += 2;

                /* These are "true" values, this ctor will compute padded
                 values as well */
                geometry = FrameGeometry(width, height);

                /* Read number of components */
                size_t number_of_components = marker[0];
                marker += 1;

                if (3 != number_of_components) {
                    fprintf(stderr, "Number of components %zu, 3 expected\n",
                            number_of_components);
                    /* Unsupported number of components */
                    return false;
                }

                /* Read three components */
                size_t horizontal_sampling_factors[3];
                size_t vertical_sampling_factors[3];
                size_t max_horizontal_factor = 0;
                size_t max_vertical_factor = 0;
                for (size_t c = 0; c < number_of_components; c++) {
                    /* Skip component id */
                    marker += 1;
                    horizontal_sampling_factors[c] = (marker[0] & 0xf0) >> 4;
                    vertical_sampling_factors[c] = marker[0] & 0x0f;
                    if (max_horizontal_factor < horizontal_sampling_factors[c]) {
                        max_horizontal_factor = horizontal_sampling_factors[c];
                    }
                    if (max_vertical_factor < vertical_sampling_factors[c]) {
                        max_vertical_factor = vertical_sampling_factors[c];
                    }
                    marker += 2; /* Move over sampling factor and quatization
                                  table destination selector into next component */
                }

                /* See standard A.1.1 for explanation of this. But generally,
                 max factor is fraction denominator common for all sampling factors,
                 and particular factor is fraction numerator. And we want first
                 field (lumas) to have 1:1 sampling factors in both vertical
                 and horizontal, and both second and third fields should have
                 1:2 downsaming, so: */
                if (horizontal_sampling_factors[0] != max_horizontal_factor) {
                    fprintf(stderr, "Bad horizontal sampling factor for field 0\n");
                    return false;
                }
                if (vertical_sampling_factors[0] != max_vertical_factor) {
                    fprintf(stderr, "Bad vertical sampling factor for field 0\n");
                    return false;
                }
                if (horizontal_sampling_factors[1] * 2 != max_horizontal_factor) {
                    fprintf(stderr, "Bad horizontal sampling factor for field 1\n");
                    return false;
                }
                if (vertical_sampling_factors[1] * 2 != max_vertical_factor) {
                    fprintf(stderr, "Bad vertical sampling factor for field 1\n");
                    return false;
                }
                if (horizontal_sampling_factors[2] * 2 != max_horizontal_factor) {
                    fprintf(stderr, "Bad horizontal sampling factor for field 2\n");
                    return false;
                }
                if (vertical_sampling_factors[2] * 2 != max_vertical_factor) {
                    fprintf(stderr, "Bad vertical sampling factor for field 2\n");
                    return false;
                }

                /* OK, we have 420, that is luma field is width x height, and
                 both chromas are width / 2 x height / 2 */
                return true;

            } else if ((MarkerType::SOF1 <= m) && (m <= MarkerType::SOF11)) {
                /* This could be SOF of the type we don't understand/support but
                 there is also DHT and JPG marker within same range, so eliminate
                 those */
                if ((MarkerType::DHT != m) && (MarkerType::DAC != m)) {
                    /* Unsupported jpeg frame type */
                    fprintf(stderr, "Not baseline jpeg, marker %x\n", (unsigned)m);
                    return false;
                }
                /* Else there is a chance still */
            }

            /* Go to next marker */
            marker = at_jpeg_next_marker(marker + 2, limit);
        }

        fprintf(stderr, "End of stream before SOF0 marker\n");

        return false;
    }

    VPUDMAPointer VPUJPEGDecoder::load_bitstream(const unsigned char *jpeg, size_t size)
    {
        VPUDMAPointer pointer = VPUDecodingSession::allocate_dma(size);
        if (RETCODE_FAILURE == IOGetVirtMem(&*pointer)) {
            return VPUDMAPointer(nullptr);
        }

        ::memcpy((void *)pointer->virt_uaddr, jpeg, size);
        return pointer;
    }

    VPUDMAPointer VPUJPEGDecoder::produce_jpeg_frame(const FrameGeometry &geometry)
    {
        FrameBuffer frame_buffer
            = VPUDecodingSession::prepare_nv12_frame_buffer_template(geometry);
        return VPUDecodingSession::allocate_dma(frame_buffer.bufMvCol);
    }

    bool VPUJPEGDecoder::decode(CodecLogger &logger, const FrameGeometry &geometry,
                                VPUDMAPointer bitstream, VPUDMAPointer output,
                                bool interleave)
    {
        return VPUDecodingSession::decode_jpeg(logger, bitstream, output, geometry,
                                               interleave);
    }
}
