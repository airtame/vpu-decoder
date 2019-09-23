/*
 * Copyright (c) 2018-2019  AIRTAME ApS
 * All Rights Reserved.
 *
 * See LICENSE.txt for further information.
 */

#include <stdio.h>
#include <string.h>

#include "jpeg_parser.hpp"
#include "vpu.hpp"
#include "vpu_jpeg_decoder.hpp"

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
        // TODO: same is also used in DecodingSession, I guess it is time
        // to move code (probably into dma_frame.cpp)
        vpu_mem_desc *dma = new vpu_mem_desc();
        if (!dma) {
            return VPUDMAPointer(nullptr);
        }
        ::memset(dma, 0, sizeof(vpu_mem_desc));
        dma->size = size;
        if (RETCODE_FAILURE == IOGetPhyMem(dma)) {
            delete dma;
            return VPUDMAPointer(nullptr);
        }

        VPUDMAPointer pointer(dma, dma_pointer_deleter);
        if (RETCODE_FAILURE == IOGetVirtMem(dma)) {
            return VPUDMAPointer(nullptr);
        }

        ::memcpy((void *)dma->virt_uaddr, jpeg, size);
        return pointer;
    }

    VPUDMAPointer VPUJPEGDecoder::produce_jpeg_frame(const FrameGeometry &geometry)
    {
        FrameBuffer frame_buffer;
        size_t size = VPU::prepare_nv12_frame_buffer_template(geometry.m_padded_width,
                                                              geometry.m_padded_height,
                                                              frame_buffer);
        // TODO: same is also used in DecodingSession, I guess it is time
        // to move code (probably into dma_frame.cpp)
        vpu_mem_desc *dma = new vpu_mem_desc();
        if (!dma) {
            return VPUDMAPointer(nullptr);
        }
        ::memset(dma, 0, sizeof(vpu_mem_desc));
        dma->size = size;
        if (RETCODE_FAILURE == IOGetPhyMem(dma)) {
            delete dma;
            return VPUDMAPointer(nullptr);
        }
        return VPUDMAPointer(dma, dma_pointer_deleter);
    }

    bool VPUJPEGDecoder::decode(const FrameGeometry &geometry, VPUDMAPointer bitstream,
                                VPUDMAPointer output, bool interleave)
    {
        DecHandle handle;
        DecOpenParam open_param;
        ::memset(&open_param, 0, sizeof(open_param));
        open_param.bitstreamFormat = STD_MJPG; /* Meaning JPEG */
        open_param.bitstreamMode = 1;
        open_param.jpgLineBufferMode = 1;
        open_param.chromaInterleave = interleave ? 1 : 0;

        if (RETCODE_SUCCESS != vpu_DecOpen(&handle, &open_param)) {
            /* Now if that doesn't work, not much we can do */
            fprintf(stderr, "vpu_DecOpen() failed");
            return false;
        }

        FrameBuffer frame_buffer;
        size_t size = VPU::prepare_nv12_frame_buffer_template(geometry.m_padded_width,
                                                              geometry.m_padded_height,
                                                              frame_buffer);
        if (size != (size_t)output->size) {
            fprintf(stderr, "bad output size");
            return false;
        }

        frame_buffer.bufY += output->phy_addr;
        /* Regardless of interleave or not, Cb starts in the same position */
        frame_buffer.bufCb += output->phy_addr;
        /* But not so with Cr */
        if (interleave) {
            /* Just leave as-is, buffers are prepared for interleave by default */
            frame_buffer.bufCr += output->phy_addr;
        } else {
            /* Chroma stride halves, different Cr address */
            frame_buffer.strideC /= 2;
            frame_buffer.bufCr = frame_buffer.bufCb
                + (frame_buffer.strideC * geometry.m_padded_height / 2);
        }
        frame_buffer.bufMvCol += output->phy_addr; /* Not needed here, but for
                                                    sake of completeness */

        /* the datatypes are int, but this is undocumented; determined by looking
         * into the imx-vpu library's vpu_lib.c vpu_DecGiveCommand() definition */
        int rotation_angle = 0;
        int mirror = 0;
        int stride = frame_buffer.strideY;

        // TODO: checks!
        vpu_DecGiveCommand(handle, SET_ROTATION_ANGLE, (void *)(&rotation_angle));
        vpu_DecGiveCommand(handle, SET_MIRROR_DIRECTION,(void *)(&mirror));
        vpu_DecGiveCommand(handle, SET_ROTATOR_STRIDE, (void *)(&stride));

        /* The framebuffer array isn't used when decoding motion JPEG data.
         * Instead, the user has to manually specify a framebuffer for the
         * output by sending the SET_ROTATOR_OUTPUT command. */
        if (RETCODE_SUCCESS != vpu_DecGiveCommand(handle, SET_ROTATOR_OUTPUT,
                                                  (void *)&frame_buffer)) {
            fprintf(stderr, "Cant instruct rotator to use our frame for output");
            return false;
        }


        VPU vpu(vpu_error_function, nullptr);
        VPU::WaitResult result;
        int idx0, idx1;

        DecParam params;
        ::memset(&params, 0, sizeof(params));

        /* For JPEG bitstream buffer already contains everything that is needed,
         so just tell that to decoder */

        /* There is an error in the specification. It states that chunkSize
         * is not used in the i.MX6. This is untrue; for motion JPEG, this
         * must be nonzero. */
        params.chunkSize = bitstream->size;

        /* Set the virtual and physical memory pointers that point to the
         * start of the frame. These always point to the beginning of the
         * bitstream buffer, because the VPU operates in line buffer mode
         * when decoding motion JPEG data. */
        params.virtJpgChunkBase = (unsigned char *)(bitstream->virt_uaddr);
        params.phyJpgChunkBase = bitstream->phy_addr;

        bool wait = vpu.wait_for_decoding(handle, params, result, idx0, idx1);
        vpu_DecClose(handle);
        return wait;
    }

    void VPUJPEGDecoder::vpu_error_function(const char *msg, void *)
    {
        fprintf(stderr, "VPU ERROR FUNC: %s\n", msg);
    }
}
