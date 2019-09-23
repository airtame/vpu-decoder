/*
 * Copyright (c) 2018-2019  AIRTAME ApS
 * All Rights Reserved.
 *
 * See LICENSE.txt for further information.
 */

/* IMPORTANT: a lot (most) of the code here is taken from libimxvpuapi sources
 and modified/simplified just to handle our specific conditions.
 These functions form common "idioms" of talking to the VPU when decoding.
 Original copyright follows */

/* imxvpuapi implementation on top of the Freescale imx-vpu library
 * Copyright (C) 2015 Carlos Rafael Giani
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301
 * USA
 */

#include <assert.h>
#include <stdint.h>
#include <string.h>
#include "vpu.hpp"

namespace airtame {

/* Documentation referred to in this file is "i.MX 6Dual/6Quad VPU Application
 Programming Interface Linux Reference Manual" */

#define VPU_WAIT_TIMEOUT 500 /* milliseconds to wait for frame completion */
#define VPU_MAX_TIMEOUT_COUNTS 4 /* how many timeouts are allowed in series */

#define VPU_DECODER_DISPLAYIDX_ALL_FRAMES_DISPLAYED -1
#define VPU_DECODER_DISPLAYIDX_SKIP_MODE_NO_FRAME_TO_DISPLAY -2
#define VPU_DECODER_DISPLAYIDX_NO_FRAME_TO_DISPLAY -3

#define VPU_DECODER_DECODEIDX_ALL_FRAMES_DECODED -1
#define VPU_DECODER_DECODEIDX_FRAME_NOT_DECODED -2

/* So this function began life as libimxvpuapi's imx_vpu_calc_framebuffer_sizes
 and then got gradually cut down just to serve our requirements. There was a lot
 of stuff dedicated to different YUV layouts, interlace, interleave, alignment
 and what not.

 Code below assumes NV12 so one luma plane and one interleaved chroma plane,
 just like described there: https://wiki.videolan.org/YUV#NV12
 Apart from that we assume no interlace (come on, it is 2018!) and H264/VP8, so:

 On alignment of our framebuffers: There are two sets of requirements:
 - One stems from what VPU wants, and it seems to be 16-pel alignment of
 widths and heights (not using interlace) and 4-byte alignment of frame_buffer
 members
 - Another one from what VIV GPU extensions we are using wants, and these are
 as follows ("i.MX Graphics User's Guide"):
 - "Width must be 16 pixel aligned"
 - "Logical address must be 8-byte aligned"

 What we are handling is H264 and VP8, and both guarantee encoded size to be 16-pel
 aligned (macroblock size). So:
 - VPU requirements are satisfied automatically
 - VIV GPU width requirements are satisfied for all YUV formats, and if NV12
 (interleaved chromas) format is used then they will even be satisfied for
 interleaved chroma plane alone, meaning that we could treat it as a separate
 texture, for example to improve memory access pattern when converting from
 YCrCb to RGB "by hand"
 - from Linux mmap() man page it seems that logical addresses are page size
 aligned, and `getconf PAGE_SIZE` is 4096 on i.MX 6 so logical address alignment
 requirement also seems to be satisfied automatically */

size_t VPU::prepare_nv12_frame_buffer_template(size_t frame_width, size_t frame_height,
                                               FrameBuffer &frame_buffer)
{
    assert(frame_width > 0);
    assert(frame_height > 0);
    assert(!(frame_width % 16));
    assert(!(frame_height % 16));

    /* Luma value for each pixel */
    size_t y_size = frame_width * frame_height;

    /* Interleaved chromas. Each individual chroma plane dimensions are equal
     to luma plane dimensions divided by two - (width / 2, height / 2), and so
     when lumas are interleaved we get (width, height / 2). So size is just
     half of luma plane size */
    size_t cbcr_size = y_size / 2;

    ::memset(&frame_buffer, 0, sizeof(frame_buffer));

    /* There are two planes of image data, bufY points to lumas, and both
     bufCb and bufCr point to interleaved chromas plane (NV12). After that
     plane is space for co-located motion vectors. */
    frame_buffer.strideY = frame_width;
    frame_buffer.strideC = frame_width;

    /* We fill only offsets, it is caller's responsibility to add physical
     address of dma memory to all */
    frame_buffer.bufY = 0;
    frame_buffer.bufCb = y_size;
    frame_buffer.bufCr = y_size;
    /* Note that we DO NOT allow for colocated motion vector memory here, we just
     set offset right after chromas! */
    frame_buffer.bufMvCol = y_size + cbcr_size;

    /* Return number of bytes of physical DMA memory to allocate */
    return y_size + cbcr_size;
}

bool VPU::get_initial_info(DecHandle handle, DecInitialInfo &initial_info)
{
    /* Set the force escape flag first (see section 4.3.2.2
     * in the VPU documentation for an explanation why) */
    if (RETCODE_SUCCESS != vpu_DecSetEscSeqInit(handle, 1)) {
        error("Could not set force escape flag");
        return false;
    }

    /* The actual retrieval */
    ::memset(&initial_info, 0, sizeof(initial_info));
    if (RETCODE_SUCCESS != vpu_DecGetInitialInfo(handle, &initial_info)) {
        error("Could not get initial decoder info");
        return false;
    }

    /* As recommended in section 4.3.2.2, clear the force
     * escape flag immediately after retrieval is finished */
    if (RETCODE_SUCCESS != vpu_DecSetEscSeqInit(handle, 0)) {
        error("Could not clear force escape flag");
        return false;
    }
    return true;
}

/* Return free space on bitstream buffer, in order to check before even
 trying to feed data */
bool VPU::get_bitstream_buffer_free_space_available(DecHandle handle, size_t &size)
{
    PhysicalAddress read_ptr, write_ptr;
    Uint32 num_free_bytes;
    if (RETCODE_SUCCESS
        != vpu_DecGetBitstreamBuffer(handle, &read_ptr, &write_ptr, &num_free_bytes)) {
        error("Failed vpu_DecGetBitstreamBuffer");
        return false;
    }
    size = num_free_bytes;
    return true;
}

bool VPU::get_bitstream_buffer_read_index(DecHandle handle, const vpu_mem_desc &bitstream_buffer,
                                          size_t &read_idx)
{
    PhysicalAddress read_ptr, write_ptr;
    Uint32 num_free_bytes;
    if (RETCODE_SUCCESS
        != vpu_DecGetBitstreamBuffer(handle, &read_ptr, &write_ptr, &num_free_bytes)) {
        error("Failed vpu_DecGetBitstreamBuffer");
        return false;
    }
    /* Read index is simply offset in physical memory of bitstream buffer */
    read_idx = read_ptr - bitstream_buffer.phy_addr;
    return true;
}

bool VPU::get_bitstream_buffer_write_index(DecHandle handle, const vpu_mem_desc &bitstream_buffer,
                                           size_t &write_idx)
{
    PhysicalAddress read_ptr, write_ptr;
    Uint32 num_free_bytes;
    if (RETCODE_SUCCESS
        != vpu_DecGetBitstreamBuffer(handle, &read_ptr, &write_ptr, &num_free_bytes)) {
        error("Failed vpu_DecGetBitstreamBuffer");
        return false;
    }
    /* Write index is simply offset in physical memory of bitstream buffer */
    write_idx = write_ptr - bitstream_buffer.phy_addr;
    return true;
}

/* This is taken nearly verbatim from libimxvpuapi, and got simplified a bit */
bool VPU::feed_data(DecHandle handle, const vpu_mem_desc &bitstream_buffer,
                    const unsigned char *data, size_t size)
{
    PhysicalAddress read_ptr, write_ptr;
    Uint32 num_free_bytes;
    if (RETCODE_SUCCESS
        != vpu_DecGetBitstreamBuffer(handle, &read_ptr, &write_ptr, &num_free_bytes)) {
        error("Failed vpu_DecGetBitstreamBuffer");
        return false;
    }

    size_t write_offset, num_free_bytes_at_end, num_bytes_to_push;

    /* Have to convert write_ptr, which is physical memory address into offset
     we then can use to address logical memory */
    write_offset = write_ptr - bitstream_buffer.phy_addr;
    num_free_bytes_at_end = bitstream_buffer.size - write_offset;
    num_bytes_to_push = (num_free_bytes_at_end < size) ? num_free_bytes_at_end : size;

    /* Write the bytes to the bitstream buffer, either in one, or in two steps */
    ::memcpy((char *)bitstream_buffer.virt_uaddr + write_offset, data, num_bytes_to_push);
    if (RETCODE_SUCCESS != vpu_DecUpdateBitstreamBuffer(handle, num_bytes_to_push)) {
        error("Failed vpu_DecUpdateBitstreamBuffer");
        return false;
    }
    data += num_bytes_to_push;
    size -= num_bytes_to_push;
    if (size) {
        /* Wrap-around, copy remaining part to the begin of bitstream buffer */
        ::memcpy((void *)bitstream_buffer.virt_uaddr, data, size);
        if (RETCODE_SUCCESS != vpu_DecUpdateBitstreamBuffer(handle, size)) {
            error("Failed vpu_DecUpdateBitstreamBuffer");
            return false;
        }
    }

    return true;
}

/* This was also taken from libimxvpuapi and modified. Important thing to
 know about this function is that it returns both decoded and display frame
 indices. These indices refer to the array passed as "framebuffers" argument to
 register_framebuffers. Decoded index may be different from display index because
 H264 buffers frames internally and if stream uses reordering, decoded frame may
 not be available for display for quite some time */

    // TODO: bitstream_buffer is no longer used here?
bool VPU::wait_for_decoding(DecHandle handle, DecParam &params,
                            WaitResult &result, int &decoded_frame_index, int &display_frame_index)
{
    /* Assign safe defaults to returned values */
    result = NOTHING;
    decoded_frame_index = display_frame_index = -1;

    DecOutputInfo output_info;
    ::memset(&output_info, 0, sizeof(output_info));

    /* Start frame decoding */
    if (RETCODE_SUCCESS != vpu_DecStartOneFrame(handle, &params)) {
        error("Failed vpu_DecStartOneFrame");
        /* According to libimxvpuapi sources, have to do this anyway
         after vpuDecStartOneFrame, because the latter "locks" the VPU
         and getting output info "unlocks" the VPU back */
        vpu_DecGetOutputInfo(handle, &output_info);
        return false;
    }

    /* Wait a few times, since sometimes, it takes more than
     * one vpu_WaitForInt() call to cover the decoding interval */
    for (int cnt = 0; cnt < VPU_MAX_TIMEOUT_COUNTS; ++cnt) {
        if (RETCODE_SUCCESS != vpu_WaitForInt(VPU_WAIT_TIMEOUT)) {
            result = WaitResult(result | WAIT_TIMEOUT);
            error("Timed out wait for frame completion");
        } else {
            break;
        }
    }

    /* As above, have to call vpu_DecGetOutputInfo regardless of wait status */
    if (RETCODE_SUCCESS != vpu_DecGetOutputInfo(handle, &output_info)) {
        error("Failed vpu_DecGetOutputInfo");
        return false;
    }

    if (output_info.notSufficientPsBuffer) {
        error("Insufficient PS buffer");
    }

    if (output_info.notSufficientSliceBuffer) {
        error("Insufficient slice buffer");
    }

    /* "If stream has errors in the picture header syntax or the first slice
     header syntax of H.264 stream" */
    if (!output_info.decodingSuccess) {
        error("Decoding failed - bitstream syntax error?");
    }

    /* Now this check is for something that shouldn't happen, because we detect
     parameter change ourselves, but it is nice safety feature */
    if (output_info.decodingSuccess & (1 << 20)) {
        result = WaitResult(result | PARAMETERS_CHANGED);
    }

    /* Check if there were enough output framebuffers. Not enough framebuffers
     mean usually that some were marked for display and weren't returned yet */
    if (output_info.indexFrameDecoded == VPU_DECODER_DECODEIDX_ALL_FRAMES_DECODED) {
        result = WaitResult(result | NOT_ENOUGH_OUTPUT_BUFFERS);
    }

    /* Check if decoding was incomplete (bit #0 is then 0, bit #4 1).
     * Incomplete decoding indicates incomplete input data. */
    if (output_info.decodingSuccess & (1 << 4)) {
        result = WaitResult(result | NOT_ENOUGH_INPUT_DATA);
    }

    /* Report dropped frames */
    if (((result & NOT_ENOUGH_INPUT_DATA) == 0)
        && (output_info.indexFrameDecoded == VPU_DECODER_DECODEIDX_FRAME_NOT_DECODED)
        && ((output_info.indexFrameDisplay == VPU_DECODER_DISPLAYIDX_NO_FRAME_TO_DISPLAY)
            || (output_info.indexFrameDisplay
                == VPU_DECODER_DISPLAYIDX_SKIP_MODE_NO_FRAME_TO_DISPLAY))) {
        result = WaitResult(result | FRAME_DROPPED);
    }

    /* Check if information about the decoded frame is available.
     * In particular, the index of the framebuffer where the frame is being
     * decoded into is essential with formats like h.264, which allow for both
     * delays between decoding and presentation, and reordering of frames.
     * With the indexFrameDecoded value, it is possible to know which framebuffer
     * is associated with what input buffer. This is necessary to properly
     * associate context information which can later be retrieved again when a
     * frame can be displayed.
     * indexFrameDecoded can be negative, meaning there is no frame currently being
     * decoded. This typically happens when the drain mode is enabled, since then,
     * there will be no more input data. */

    if (output_info.indexFrameDecoded >= 0) {
        decoded_frame_index = output_info.indexFrameDecoded;
        result = WaitResult(result | DECODED_FRAME);
    }

    /* Check if information about a displayable frame is available.
     * A frame can be presented when it is fully decoded. In that case,
     * indexFrameDisplay is >= 0. If no fully decoded and displayable
     * frame exists (yet), indexFrameDisplay is -2 or -3 (depending on the
     * currently enabled frame skip mode). If indexFrameDisplay is -1,
     * all frames have been decoded. This typically happens after drain
     * mode was enabled.
     * This index is later used to retrieve the context that was associated
     * with the input data that corresponds to the decoded and displayable
     * frame (see above). available_decoded_frame_idx stores the index for
     * this precise purpose. Also see imx_vpu_dec_get_decoded_frame(). */

    if (output_info.indexFrameDisplay >= 0) {
        display_frame_index = output_info.indexFrameDisplay;
        result = WaitResult(result | FRAME_AVAILABLE_FOR_DISPLAY);
    } else if (output_info.indexFrameDisplay == VPU_DECODER_DISPLAYIDX_ALL_FRAMES_DISPLAYED) {
        result = WaitResult(result | END_OF_SEQUENCE);
    } else {
        /* There can also be -2 or -3 values, so:
         VPU_DECODER_DISPLAYIDX_SKIP_MODE_NO_FRAME_TO_DISPLAY
         VPU_DECODER_DISPLAYIDX_NO_FRAME_TO_DISPLAY
         but we are not using skip mode here, even on errors (buffer is flushed
         then and we skip to next IDR frame ourselves, with parser keeping all
         SPS/PPSes safe), and "nothing available" is our default result anyway */
    }
    return true;
}

/* Values here "inherited" from libimxvpuapi, and tuned down a bit */
/* WARNING: bitstream buffer size should be 4-byte aligned and a multiple
 of 1024.
 MA: 3MB used in libimxvpuapi is a lot, in the order of ten seconds for our
 typical Full HD stream. Also, we buffer NALs in pipeline frames, and so
 this value could probably be cut down to around 1MB */
#define VPU_DEC_H264_MAIN_BITSTREAM_BUFFER_SIZE (1024 * 1024 * 1)

/* VPU documentation 3.2.3.2 seems to recommend to use half a size of one
 YUV frame. In our case 1920*1088*1.5/2 so exactly like integer computation
 below */
#define VPU_MAX_SLICE_BUFFER_SIZE (1920 * 1088 * 15 / 20)

/* MA: libimxvpuapi used half a MB here and this probably just lazy, because
 there max 32 SPS and 256 PPS allowed, and typically they are very small.
 Plus, we save SPS/PPS in H264Parser anyway, and feed them JIT. So we'll
 probably hardly see more than 1SPS and perhaps a few PPSes per video sequence.
 And finally, I have yet to see H264 stream that uses more than one SPS at a
 time (for example AirPlay seems to replace previous SPS rather than send both).
 WARNING: address should be 4-aligned and size a multiple of 1024 */
#define VPU_PS_SAVE_BUFFER_SIZE (1024 * 128)

/* Now this was HUGE, because it was 5+MB. Now we have around 3, which is
 saner, but still a bit high */

size_t VPU::h264_get_recommended_bitstream_buffer_size()
{
    return VPU_DEC_H264_MAIN_BITSTREAM_BUFFER_SIZE;
}

size_t VPU::h264_get_recommended_ps_save_buffer_size()
{
    return VPU_PS_SAVE_BUFFER_SIZE;
}

size_t VPU::h264_get_recommended_slice_buffer_size()
{
    return VPU_MAX_SLICE_BUFFER_SIZE;
}

size_t VPU::h264_prepare_nv12_frame_buffer_template(size_t frame_width, size_t frame_height,
                                                    FrameBuffer &frame_buffer)
{
    size_t size = prepare_nv12_frame_buffer_template(frame_width, frame_height, frame_buffer);

    /* For the life of me I can't find any info on how one should calculate
     mvcol_size (colocated motion vector data size) but libimxvpuapi seems to
     initialize this to the number of values on single chroma plane (so half of
     interleaved chromas size or quarter of luma size in our case) and more for
     formats with more chroma samples. BTW this seems dumb because chroma sizes
     don't affect final picture size, and motion vector are applied to
     macroblocks or subdivisions of these down to 4x4 level, but they are for
     "luma and corresponding chroma samples" so they depend on picture size
     (luma size), not on the chroma size! No format I know of uses separate MVs
     for lumas and chromas.

     I think it is so because original VPU sample code used Video for Linux
     (V4L), YUV420 format and took luma and chroma buffers from V4L,
     so just colocated motion vector data had to be allocated. They got it
     using the size of single chroma buffer because it is luma / 4, then
     everybody using VPU in turn assumed that MVs are proportional to chroma
     size and followed the suit with extra space being allocated when chromas
     are bigger.

     We are constraining ourself to chromas being a quarter of luma size anyway,
     so even if I am missing something, it is still safe.

     Also, documentation of VPU has this to say:
     "The co-located motion vector is only required for B-frame decoding
     in MPEG-2, AVC MP/HP..." meaning probably Main and High Profiles, and we
     do use B-frames when doing YT casting or Android/AirPlay.

     Hmm, macroblocks are 16x16 but prediction can go down to 4x4. Meaning that
     there can be a motion vector for each 4x4 pel square. So total y_size / 16
     motion vectors, and that would give us 4 bytes per motion vectors or
     2 bytes per component (horizontal/vertical) when buffer is allocated as
     luma_size / 4. Now it is suprisingly hard to find motion vector range
     (not search range!) in H264, and I don't want to spend two days going
     through MV reconstruction logic, but there are two hints:

     1) H264 standard Table A-1 says that highest Vertical MV component range is
     from -512 to +511.75, MVs are in quad-pel units so 10bits for -512 to +511
     and two bits for quad pel precision gives 12bits per component max...
     2) Same source, Table A-5 says that maximum luma resolution is 4096x2304,
     so maximum absolute motion vector component value that even makes sense is
     4096, so 12 bits. Add 1 bit for sign, and 2 bits for quad-pel and 15 bit is
     the result, so it still should fit in two bytes just fine.
     3) Other sources claim there are higher profiles still, and that they allow
     for up to 8k, but again, that is just one bit more, still fits in 16 bits.
     And i.MX6 DualLite is limited to display FullHD @30FPS, so it wouldn't make
     much sense for the decoder to handle MVs that allow for more than sixteen
     times that (area wise).

     Technically B-frames may have two sets of MVs, but I guess one set is
     always provided for in decoder, because this is what any other inter coded
     frame (P for example) needs, and they state that colocated mv is just for
     B-frames...

     And if all that sounds like hair-splitting, please remember that ALL H264
     frame buffers have to have colocated MVs, and for Full HD mvcol_size is
     half a MB, so with max number of reference frames that VPU may need (18)
     and our overhead on top of that (currently 2) we may be talking about 10 MB
     of extra DMA memory! */

    size_t mvcol_size = frame_width * frame_height / 4;

    /* Return number of bytes of physical DMA memory to allocate */
    return size + mvcol_size;
}

/* Values here "inherited" from libimxvpuapi, and tuned down a bit */
/* WARNING: bitstream buffer size should be 4-byte aligned and a multiple of 1024.
 MA: 3MB used in libimxvpuapi is a lot, in the order of ten seconds for our
 typical Full HD stream. Also, we buffer NALs in pipeline frames, and so
 this value could probably be cut down to around 1MB */
#define VPU_DEC_VP8_MAIN_BITSTREAM_BUFFER_SIZE (1024 * 1024 * 1)

/* TODO: taken from libimxvpuapi, try to verify */
#define VPU_DEC_VP8_MB_PRED_BUFFER_SIZE (68 * (1920 * 1088 / 256))

size_t VPU::vp8_get_recommended_bitstream_buffer_size()
{
    return VPU_DEC_VP8_MAIN_BITSTREAM_BUFFER_SIZE;
}

size_t VPU::vp8_get_recommended_mb_prediction_buffer_size()
{
    return VPU_DEC_VP8_MB_PRED_BUFFER_SIZE;
}

void VPU::error(const char *msg)
{
    (*m_error_function)(msg, m_error_function_user_data);
}
}
