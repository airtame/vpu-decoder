/*
 * Copyright (c) 2018-2020  AIRTAME ApS
 * All Rights Reserved.
 *
 * See LICENSE.txt for further information.
 */

#include <chrono>
#include <assert.h>
#include <string.h>
#include "vpu_decoding_session.hpp"

/* Milliseconds to wait for frame completion. Not sure what is sensible value
 here, but I never saw actual decode (as opposed to for example DMA memory
 allocation) taking more than 20-30 msec, so it should suffice */
#define VPU_WAIT_TIMEOUT 50

/* How many timeouts are allowed in series */
#define VPU_MAX_TIMEOUT_COUNTS 3

namespace airtame {

VPUDecodingSession::VPUDecodingSession(CodecLogger &logger, DecodingStats &stats,
                                       VPUDecoderBuffers &buffers, VPUFrameBuffers &frames,
                                       CodecType codec_type, const FrameGeometry &frame_geometry,
                                       size_t number_of_reference_frame_buffers,
                                       size_t number_of_display_frame_buffers,
                                       bool reordering)
    : m_logger(logger)
    , m_stats(stats)
    , m_buffers(buffers)
    , m_frames(frames)
    , m_codec_type(codec_type)
    , m_frame_geometry(frame_geometry)
    , m_number_of_reference_frame_buffers(number_of_reference_frame_buffers)
    , m_number_of_display_frame_buffers(number_of_display_frame_buffers)
    , m_reordering(reordering)
    , m_handle(nullptr)
    , m_initial_info_retrieved(false)
{
}

VPUDecodingSession::~VPUDecodingSession()
{
    /* We log errors here but ignore them - this can be called with decoder
     in a mess state anyway, so what we can do? */
    if (m_handle) {
        if (is_busy()) {
            /* This can happen when doing async decode, and we need to protect
             against it lest the VPU will get unusable */
            vpu_SWReset(m_handle, 0);
        }
        if (RETCODE_SUCCESS != vpu_DecBitBufferFlush(m_handle)) {
            codec_log_error(m_logger, "Could not flush decoder bit buffer");
        }
        /* This informs decoder of end of input data */
        if (RETCODE_SUCCESS != vpu_DecUpdateBitstreamBuffer(m_handle, 0)) {
            codec_log_error(m_logger, "Could not send EOS to decoder");
        }
        vpu_DecClose(m_handle);
    }
}

VPUDecodingSession *VPUDecodingSession::open_for_video(CodecLogger &logger,
                                                       DecodingStats &stats,
                                                       VPUDecoderBuffers &buffers,
                                                       VPUFrameBuffers &frames,
                                                       CodecType codec_type,
                                                       const FrameGeometry &frame_geometry,
                                                       size_t number_of_reference_frame_buffers,
                                                       size_t number_of_display_frame_buffers,
                                                       bool reordering)
{
    /* Verify geometry. Specs say that i.mx6 can decode all video codecs "up to
     1920x1088". But this shouldn't be taken as width/height limit - we know for
     a fact that we can decode 1080x1920. So it is more like number of
     macroblocks limit -> memory/bandwidth/time limit.

     Now, 1920x1088 frame has 120x68 macroblocks, for a total of 8160 mbs.
     And this is what we are going to check against */

    /* It is safe to divide padded width/height by 16, because padding is to the
     macroblock boundary */
    size_t horizontal_mbs = frame_geometry.m_padded_width / 16;
    size_t vertical_mbs = frame_geometry.m_padded_height / 16;
    size_t total_mbs = horizontal_mbs * vertical_mbs;

    if (8160 < total_mbs) {
        codec_log_error(logger, "Frame has more macroblocks (%zu) than Full HD "
                                "image has (8160). VPU can't decode such frames",
                        total_mbs);
        return nullptr;
    }

    CodStd bitstream_format;
    const char *codec;
    if (CodecType::H264 == codec_type) {
        bitstream_format = STD_AVC;
        codec = "h264";
        buffers.init_for_h264();
    } else if (CodecType::VP8 == codec_type) {
        bitstream_format = STD_VP8;
        codec = "VP8";
        buffers.init_for_vp8();
    } else {
        codec_log_error(logger, "Unknown codec type");
        codec = "unknown";
        return nullptr;
    }

    /* We expect bitstream buffer to be mapped to process address space */
    assert(buffers.get_bitstream_buffer().virt_uaddr);

    /* First thing is to open decoder. This is where we decide on bitstream
     format, reordering and also pass in buffers */
    DecHandle handle = open_decoder(buffers, bitstream_format, frame_geometry,
                                    reordering);
    if (handle) {
        /* Previous version didn't have this log, but it really helps when looking
         for issues, so I am adding it back */
        codec_log_info(logger, "Decoder opened in %s mode, true frame size %zux%zu%s",
                       codec, frame_geometry.m_true_width, frame_geometry.m_true_height,
                       reordering ? ", reordering enabled" : "");
    } else {
        codec_log_error(logger, "Couldn't open new decoder instance. This "
                                "usually meants that vpu_Init() wasn't called");
        return nullptr;
    }

    /* OK, decoder opened, so let's make object */
    VPUDecodingSession *new_session
        = new VPUDecodingSession(logger, stats, buffers, frames, codec_type,
                                 frame_geometry, number_of_reference_frame_buffers,
                                 number_of_display_frame_buffers, reordering);
    new_session->m_handle = handle;

    /* That is all */
    return new_session;
}

bool VPUDecodingSession::decode_jpeg(CodecLogger &logger, VPUDMAPointer bitstream,
                                     VPUDMAPointer frame,
                                     const FrameGeometry &frame_geometry,
                                     bool interleave)
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
        codec_log_error(logger, "vpu_DecOpen() failed");
        return false;
    }

    FrameBuffer frame_buffer = prepare_nv12_frame_buffer_template(frame_geometry);
    size_t size = frame_buffer.bufMvCol;
    if (size != (size_t)frame->size) {
        codec_log_error(logger, "Bad output JPEG frame size");
        return false;
    }

    frame_buffer.bufY += frame->phy_addr;
    /* Regardless of interleave or not, Cb starts in the same position */
    frame_buffer.bufCb += frame->phy_addr;
    /* But not so with Cr */
    if (interleave) {
        /* Just leave as-is, buffers are prepared for interleave by default */
        frame_buffer.bufCr += frame->phy_addr;
    } else {
        /* Chroma stride halves, different Cr address */
        frame_buffer.strideC /= 2;
        frame_buffer.bufCr = frame_buffer.bufCb
            + (frame_buffer.strideC * frame_geometry.m_padded_height / 2);
    }
    frame_buffer.bufMvCol += frame->phy_addr; /* Not needed here, but for
                                                sake of completeness */

    /* the datatypes are int, but this is undocumented; determined by looking
     * into the imx-vpu library's vpu_lib.c vpu_DecGiveCommand() definition */
    int rotation_angle = 0;
    int mirror = 0;
    int stride = frame_buffer.strideY;

    // TODO: return value checks!
    vpu_DecGiveCommand(handle, SET_ROTATION_ANGLE, (void *)(&rotation_angle));
    vpu_DecGiveCommand(handle, SET_MIRROR_DIRECTION,(void *)(&mirror));
    vpu_DecGiveCommand(handle, SET_ROTATOR_STRIDE, (void *)(&stride));

    /* The framebuffer array isn't used when decoding motion JPEG data.
     * Instead, the user has to manually specify a framebuffer for the
     * output by sending the SET_ROTATOR_OUTPUT command. */
    if (RETCODE_SUCCESS != vpu_DecGiveCommand(handle, SET_ROTATOR_OUTPUT,
                                              (void *)&frame_buffer)) {
        codec_log_error(logger, "Cant instruct rotator to use our frame for output");
        return false;
    }

    /* Note that instead of calling start_decoding we start decode manually
     here. It is because JPEG decoding start is quite different from video
     decoding. */
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


    /* Start frame decoding */
    if (RETCODE_SUCCESS != vpu_DecStartOneFrame(handle, &params)) {
        codec_log_error(logger, "Failed vpu_DecStartOneFrame");
        /* This function HAS to be called even if vpu_DecStartOneFrame failed */
        DecOutputInfo output_info;
        ::memset(&output_info, 0, sizeof(output_info));
        vpu_DecGetOutputInfo(handle, &output_info);
        vpu_DecClose(handle);
        return false;
    }

    int decoded_index, display_index; /* Not used here */
    VPUDecodeStatus status = wait_for_decode(logger, handle, decoded_index, display_index);
    vpu_DecClose(handle);
    // TODO: perhaps should check if status flags are set properly - not sure
    // which ones should be for JPEG
    return (VPUDecodeStatus::ERROR & status) ? false : true;
}

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

 What we are handling is H264 and VP8, and both guarantee encoded size to be
 16-pel aligned (macroblock size). So:
 - VPU requirements are satisfied automatically
 - VIV GPU width requirements are satisfied for all YUV formats, and if NV12
 (interleaved chromas) format is used then they will even be satisfied for
 interleaved chroma plane alone, meaning that we could treat it as a separate
 texture, for example to improve memory access pattern when converting from
 YCrCb to RGB "by hand"
 - from Linux mmap() man page it seems that logical addresses are page size
 aligned, and `getconf PAGE_SIZE` is 4096 on i.MX 6 so logical address alignment
 requirement also seems to be satisfied automatically */

FrameBuffer VPUDecodingSession::prepare_nv12_frame_buffer_template(const FrameGeometry &frame_geometry)
{
    FrameBuffer frame_buffer;
    /* Luma value for each pixel */
    size_t y_size = frame_geometry.m_padded_width * frame_geometry.m_padded_height;

    /* Interleaved chromas. Each individual chroma plane dimensions are equal
     to luma plane dimensions divided by two - (width / 2, height / 2), and so
     when lumas are interleaved we get (width, height / 2). So size is just
     half of luma plane size */
    size_t cbcr_size = y_size / 2;

    ::memset(&frame_buffer, 0, sizeof(frame_buffer));

    /* There are two planes of image data, bufY points to lumas, and both
     bufCb and bufCr point to interleaved chromas plane (NV12). After that
     plane is space for co-located motion vectors. */
    frame_buffer.strideY = frame_geometry.m_padded_width;
    frame_buffer.strideC = frame_geometry.m_padded_width;

    /* We fill only offsets, it is caller's responsibility to add physical
     address of dma memory to all */
    frame_buffer.bufY = 0;
    frame_buffer.bufCb = y_size;
    frame_buffer.bufCr = y_size;
    /* Note that we DO NOT allow for colocated motion vector memory here, we just
     set offset right after chromas! */
    frame_buffer.bufMvCol = y_size + cbcr_size;

    return frame_buffer;
}

VPUDMAPointer VPUDecodingSession::allocate_dma(size_t size)
{
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

/* Data comes in three parts, because for H264 we may want to feed SPS/PPS/Slice
 and for VP8 we feed IVF header/frame */
bool VPUDecodingSession::feed(const unsigned char *data, size_t size, size_t &size_fed)
{
    PhysicalAddress read_ptr, write_ptr;
    Uint32 num_free_bytes;
    if (RETCODE_SUCCESS
        != vpu_DecGetBitstreamBuffer(m_handle, &read_ptr, &write_ptr, &num_free_bytes)) {
        codec_log_error(m_logger, "Failed vpu_DecGetBitstreamBuffer");
        return false;
    }

    if (!num_free_bytes) {
        /* No space left in buffer */
        size_fed = 0;
        return true;
    }

    if (size > num_free_bytes) {
        /* Make sure we don't try to push too much */
        codec_log_warn(m_logger, "Not enough space on bitstream input buffer");
        size = num_free_bytes;
    }

    size_t write_offset, num_free_bytes_at_end, num_bytes_to_push;

    /* Have to convert write_ptr, which is physical memory address into offset
     we then can use to address logical memory */
    write_offset = write_ptr - m_buffers.get_bitstream_buffer().phy_addr;
    num_free_bytes_at_end = m_buffers.get_bitstream_buffer().size - write_offset;
    num_bytes_to_push = (num_free_bytes_at_end < size) ? num_free_bytes_at_end : size;

    /* Write the bytes to the bitstream buffer, either in one, or in two steps */
    ::memcpy((char *)m_buffers.get_bitstream_buffer().virt_uaddr + write_offset,
             data, num_bytes_to_push);
    if (RETCODE_SUCCESS != vpu_DecUpdateBitstreamBuffer(m_handle, num_bytes_to_push)) {
        codec_log_error(m_logger, "Failed vpu_DecUpdateBitstreamBuffer");
        return false;
    }
    size_fed = num_bytes_to_push;
    data += num_bytes_to_push;
    size -= num_bytes_to_push;
    if (size) {
        /* Wrap-around, copy remaining part to the begin of bitstream buffer */
        ::memcpy((void *)m_buffers.get_bitstream_buffer().virt_uaddr, data, size);
        if (RETCODE_SUCCESS != vpu_DecUpdateBitstreamBuffer(m_handle, size)) {
            codec_log_error(m_logger, "Failed vpu_DecUpdateBitstreamBuffer");
            return false;
        }
        size_fed += size;
    }

    return true;
}

bool VPUDecodingSession::feed_end_of_stream()
{
    /* Update bitstream buffer with zero bytes of data sends end of sequence
     information to the decoder */
    if (RETCODE_SUCCESS != vpu_DecUpdateBitstreamBuffer(m_handle, 0)) {
        codec_log_error(m_logger,
                        "Failed sending end of stream to decoder "
                        "while flushing, will close immediately");
        return false;
    }
    return true;
}

bool VPUDecodingSession::get_bitstream_buffer_free_space_available(size_t &size)
{
    PhysicalAddress read_ptr, write_ptr;
    Uint32 num_free_bytes;
    if (RETCODE_SUCCESS
        != vpu_DecGetBitstreamBuffer(m_handle, &read_ptr, &write_ptr, &num_free_bytes)) {
        codec_log_error(m_logger, "Failed vpu_DecGetBitstreamBuffer");
        return false;
    }
    size = num_free_bytes;
    return true;
}

bool VPUDecodingSession::has_frame_for_decoding() const
{
    if (m_frames.get_number_of_display_frame_buffers()) {
        /* Already have allocated frames, can say how many are left */
        return m_frames.has_frame_for_decoding();
    } else {
        /* No allocated frames yet, new ones will be allocated inside first
         decode() call, so definitely there will be free one */
        return true;
    }
}

void VPUDecodingSession::return_output_frame(unsigned long physical_address)
{
    m_frames.mark_frame_as_returned(physical_address);
}

/* Returns true if VPU is currently decoding a frame. */
bool VPUDecodingSession::is_busy()
{
    return vpu_IsBusy();
}

VPUDecodeStatus VPUDecodingSession::decode_video(const std::shared_ptr<FrameMetaData> &meta,
                                                 VPUOutputFrame &output_frame)
{
    /* IMPORTANT: in theory, VPU should just return apropriate status when
     one starts decoding and no frame is available. BUT we found out (the hard
     way) that for some VP8 streams using more reference frames (not just
     keyframe but altref/golden) decoding will get corrupted if one starts
     decoding without having spare frame. I believe this is actual VPU decoder
     bug. So in order not to run into it, we always make sure we have at least
     one frame for decoding before entering decode. */
    if (!has_frame_for_decoding()) {
        /* This is not decoder error */
        return VPUDecodeStatus::NO_FREE_OUTPUT_BUFFER;
    }

    /* Start the VPU decoding process */
    if (!start_video_decoding()) {
        return VPUDecodeStatus::ERROR;
    }

    /* Wait for decode to finish */
    /* IMPORTANT: note that status may contain FRAME_DECODED or not and both can
     be fine. This is because decoder may actually decode next frame or it may
     decide that before decoding it needs to spit out one (or more) of buffered
     frames. About the only problem with this is that decoder counts those as
     "away" frames, and so even if technically one or more of frames is free to
     decode to (we wouldn't called here in the first place if it wasn't),
     decoder can decide that it lacks frames for decoding. See also comment in
     has_frame_for_decoding()

     So we need to be prepared to return previously decoded frame from here and
     NOT decoding what is in the bitstream input buffer. Upper level code should
     be prepared for that and call decode again, but WITHOUT feeding the
     bitstream data again. */

    return wait_for_video_decode(meta, output_frame);
}

/* Instruct VPU to start decoding a frame.
 WARNING: decoding is asynchronous, and with bistream mode we set in
 open_decoder, it won't end if bistream buffer doesn't contain whole frame (or
 even more) data. */
bool VPUDecodingSession::start_video_decoding()
{
    if (!m_initial_info_retrieved) {
        /* In theory, we could do without this initial info stuff, because our
         own parsers do the job well enough. But unfortunately VPU forces this
         call upon us (will spit "bad call sequence" errors unless we start with
         getting initial info).

         This (first start_decoding() call) is actually first place where we
         _can_ get initial info safely, because while for some codecs (h264)
         just metadata (SPS/PPS) suffices, for others (VP8) we need first
         keyframe. */
        DecInitialInfo initial_info;
        if (!get_initial_info(initial_info)) {
            return false;
        }
        m_initial_info_retrieved = true;

        /* This is probably OK, because this function is just used for video
         and not jpeg, which _CAN_ have other formats than 420. But if one was
         to add other subsampling patterns, member variable should probably be
         added to this class, so that check would go against format instance was
         created for and not this hardcoded value */
        if (FORMAT_420 != initial_info.mjpg_sourceFormat) {
             codec_log_error(m_logger, "Stream not in YUV420 format, cannot display it");
             return false;
        }

        // On the other hand, this is probably fine. I REALLY don't expect to
        // see interlaced streams
        if (initial_info.interlace) {
            /* Analog TV is long dead and we are playing back real-time encoded
             streams. Plus, this gives us simpler alignment calculations */
            codec_log_error(m_logger, "Stream is interlaced, cannot display it");
            return false;
        }

        // TODO: not so simple, because we check number of reference frames in
        // vpu_decoder. But we seem to get 2 frames less and stuff works...WTF?
/*        if ((size_t)initial_info.minFrameBufferCount > m_number_of_reference_frame_buffers) {
            codec_log_warn(m_logger, "Decoder wants %zu reference frames, we only "
                                     "have %zu. Will correct it here but something's "
                                     "fishy",
                           initial_info.minFrameBufferCount, m_number_of_reference_frame_buffers);
            m_number_of_reference_frame_buffers = initial_info.minFrameBufferCount;
        }*/

        /* Now that the decoder is happy we got initial info out of it, we can
         allocate frames for the decoding */
        if (!allocate_frames()) {
            return false;
        }
    }

    assert(!is_busy());

    /* Return frames now, because we are sure decoder isn't working. When it
     works, it ignores returning of the frames. */
    if (!m_frames.return_frames_now(m_handle)) {
        return false;
    }

    /* Final sanity check. We saw this with some VP8 streams and it wasn't
     pretty sight. */
    if (!has_frame_for_decoding()) {
        codec_log_fatal(m_logger,
                        "An attempt was made to call start_decoding() when "
                        "has_frame_for_decoding() == false. While allowed by "
                        "documentation it ends up producing garbled output");
        return false;
    }

    DecParam params;
    ::memset(&params, 0, sizeof(params));

    /* Start frame decoding */
    if (RETCODE_SUCCESS != vpu_DecStartOneFrame(m_handle, &params)) {
        codec_log_error(m_logger, "Failed vpu_DecStartOneFrame");
        /* This function HAS to be called even if vpu_DecStartOneFrame failed */
        DecOutputInfo output_info;
        ::memset(&output_info, 0, sizeof(output_info));
        vpu_DecGetOutputInfo(m_handle, &output_info);
        return false;
    }

    return true;
}

/* High - level wait_for_decode, adding management of our frame buffers on top
 of simple VPU interface, also translating integer indices into apropriate
 frames/flags. This version is used for video decoding - JPEG decoding has no
 need for buffer management */
VPUDecodeStatus VPUDecodingSession::wait_for_video_decode(const std::shared_ptr<FrameMetaData> &meta,
                                                          VPUOutputFrame &output_frame)
{
    int decoded_frame_buffer_index;
    int display_frame_buffer_index;
    VPUDecodeStatus status = wait_for_decode(m_logger, m_handle, decoded_frame_buffer_index,
                                             display_frame_buffer_index);

    if (VPUDecodeStatus::OUTPUT_DECODED & status) {
        m_frames.frame_decoded(decoded_frame_buffer_index, meta);
    }

    if (VPUDecodeStatus::FRAME_GIVEN_FOR_DISPLAY & status) {
        /* Have frame to display, spit it out. Here is where frame can go out
         for display */
        m_frames.frame_to_be_given_for_display(display_frame_buffer_index,
                                               output_frame.dma,
                                               output_frame.meta);
        output_frame.size = output_frame.dma->size;
        output_frame.geometry = m_frame_geometry;
    }

    return status;
}

bool VPUDecodingSession::allocate_frames()
{
    auto before = std::chrono::steady_clock::now();
    FrameBuffer frame_buffer_template
        = prepare_nv12_frame_buffer_template(m_frame_geometry);
    /* Size of frame equals offset to colocated motion vector data, because
     normal codecs don't use it */
    size_t frame_size = frame_buffer_template.bufMvCol;
    if (CodecType::H264 == m_codec_type) {
        /* Except for H264, where we also have to take collocated motion vector
         data into account (technically only when B-frames are present, but only
         for Baseline profile we have guarantee that there won't be any, also
         buffer overrun in DMA memory is dangerous) */
        frame_size += m_frame_geometry.m_padded_width * m_frame_geometry.m_padded_height / 4;
    }
    size_t buffers_size = m_buffers.get_bitstream_buffer().size
        + m_buffers.get_slice_buffer().size + m_buffers.get_ps_save_buffer().size
        + m_buffers.get_mb_prediction_buffer().size;
    size_t number_of_frame_buffers = m_number_of_reference_frame_buffers
                                   + m_number_of_display_frame_buffers;
    codec_log_info(m_logger,
                   "Will need %zu buffers %.2fMB each (%.1fMB total allocation "
                   "including bitstream and other auxilary buffers).",
                   number_of_frame_buffers, double(frame_size) / (1024 * 1024),
                   double(frame_size * number_of_frame_buffers + buffers_size)
                       / (1024 * 1024));

    /* IMPORTANT: this is separated into two vectors, because we want to use
     m_frame_buffers as continuous array of FrameBuffer structures (this is what
     decoder needs anyway) */
    FrameBuffer *frames_array = nullptr;
    if (!m_frames.reserve(frame_size, m_number_of_reference_frame_buffers,
                          m_number_of_display_frame_buffers, frames_array)) {
        return false;
    }

    /* Adjust offsets for new buffers */
    for (size_t i = 0; i < number_of_frame_buffers; i++) {
        frames_array[i].bufCb += frame_buffer_template.bufCb;
        frames_array[i].bufCr += frame_buffer_template.bufCr;
        frames_array[i].bufMvCol += frame_buffer_template.bufMvCol;
    }

    /* Success! */
    // TODO: remove those stupid stats, it makes more sense to have them in
    // specific buffers now
    m_stats.update_dma_allocation_size(frame_size * number_of_frame_buffers + buffers_size);

    /* Finally, we have to let decoder know the buffers it can use */
    DecBufInfo buf_info;
    ::memset(&buf_info, 0, sizeof(buf_info));
    buf_info.avcSliceBufInfo.bufferBase = m_buffers.get_slice_buffer().phy_addr;
    buf_info.avcSliceBufInfo.bufferSize = m_buffers.get_slice_buffer().size;
    buf_info.vp8MbDataBufInfo.bufferBase = m_buffers.get_mb_prediction_buffer().phy_addr;
    buf_info.vp8MbDataBufInfo.bufferSize = m_buffers.get_mb_prediction_buffer().size;

    RetCode code =
        vpu_DecRegisterFrameBuffer(m_handle, frames_array, number_of_frame_buffers,
                                   frame_buffer_template.strideY, &buf_info);

    if (RETCODE_SUCCESS != code) {
        codec_log_error(m_logger, "Failed to register frame buffers");
        return false;
    }

    auto duration = std::chrono::steady_clock::now() - before;
    size_t duration_msec =
        std::chrono::duration_cast<std::chrono::milliseconds>(duration).count();
    codec_log_info(m_logger, "Time for allocate_frames() == %zums",
                   duration_msec);

    return true;
}

bool VPUDecodingSession::get_initial_info(DecInitialInfo &initial_info)
{
    /* Set the force escape flag first (see section 4.3.2.2
     * in the VPU documentation for an explanation why) */
    if (RETCODE_SUCCESS != vpu_DecSetEscSeqInit(m_handle, 1)) {
        codec_log_error(m_logger, "Could not set force escape flag");
        return false;
    }

    /* The actual retrieval */
    ::memset(&initial_info, 0, sizeof(initial_info));
    if (RETCODE_SUCCESS != vpu_DecGetInitialInfo(m_handle, &initial_info)) {
        codec_log_error(m_logger, "Could not get initial decoder info");
        return false;
    }

    /* As recommended in section 4.3.2.2, clear the force
     * escape flag immediately after retrieval is finished */
    if (RETCODE_SUCCESS != vpu_DecSetEscSeqInit(m_handle, 0)) {
        codec_log_error(m_logger, "Could not clear force escape flag");
        return false;
    }

    return true;
}

/* Low - level "wait_for_decode", dealing directly with VPU interface */
VPUDecodeStatus VPUDecodingSession::wait_for_decode(CodecLogger &logger, DecHandle handle,
                                                    int &decoded_frame_buffer_index,
                                                    int &display_frame_buffer_index)
{
    /* Wait a few times, since sometimes, it takes more than
     * one vpu_WaitForInt() call to cover the decoding interval */
    for (int cnt = 0; cnt < VPU_MAX_TIMEOUT_COUNTS; ++cnt) {
        if (RETCODE_SUCCESS != vpu_WaitForInt(VPU_WAIT_TIMEOUT)) {
            codec_log_error(logger, "Decode timed out, will reset decoder!");
            vpu_SWReset(handle, 0);
            return VPUDecodeStatus::ERROR | VPUDecodeStatus::DECODE_TIMEOUT;
        } else {
            break;
        }
    }

    DecOutputInfo output_info;
    ::memset(&output_info, 0, sizeof(output_info));
    if (RETCODE_SUCCESS != vpu_DecGetOutputInfo(handle, &output_info)) {
        codec_log_error(logger, "Failed vpu_DecGetOutputInfo");
        return VPUDecodeStatus::ERROR;
    }

    if (output_info.notSufficientPsBuffer) {
        codec_log_error(logger, "Insufficient PS buffer");
        return VPUDecodeStatus::ERROR;
    }

    if (output_info.notSufficientSliceBuffer) {
        codec_log_error(logger, "Insufficient slice buffer");
        return VPUDecodeStatus::ERROR;
    }

    /* "If stream has errors in the picture header syntax or the first slice
     header syntax of H.264 stream" */
    if (!output_info.decodingSuccess) {
        codec_log_error(logger, "Decoding failed - bitstream syntax error?");
        return VPUDecodeStatus::ERROR;
    }

    /* Now this check is for something that shouldn't happen, because we detect
     parameter change ourselves, but it is nice safety feature */
    if (output_info.decodingSuccess & (1 << 20)) {
        codec_log_error(logger, "Parameter change signalled during decoding");
        return VPUDecodeStatus::ERROR;
    }

    VPUDecodeStatus status = VPUDecodeStatus::NOTHING;
    /* Check if decoding was incomplete (bit #0 is then 0, bit #4 1).
     Incomplete decoding indicates incomplete input data.  */
    if (output_info.decodingSuccess & (1 << 4)) {
        status |= VPUDecodeStatus::NOT_ENOUGH_INPUT_DATA;
    }

    /* Check which frame buffer (if any) contains decoded content */
    if (output_info.indexFrameDecoded >= 0) {
        decoded_frame_buffer_index = output_info.indexFrameDecoded;
        status |= VPUDecodeStatus::OUTPUT_DECODED;
    } else {
        /* No decoded frame available. This is OK, because in flush mode
         decoder will just spit out buffered display frames */
        decoded_frame_buffer_index = -1;
    }

    /* Check which frame buffer (if any) is available for display */
    if (output_info.indexFrameDisplay >= 0) {
        display_frame_buffer_index = output_info.indexFrameDisplay;
        status |= VPUDecodeStatus::FRAME_GIVEN_FOR_DISPLAY;
    } else {
        /* Again, this is OK because for example in h264 with reordering we
         may have nothing to display after first decode, because first frame
         decoded ain't first frame for display */
        display_frame_buffer_index = -1;
    }

    /* Note that it is possible for both display_frame_buffer_index and
     decoded_frame_buffer index to be == -1 at the same time. Even with new,
     carefully measured feeding of the decoder it happens at the end of flushing
     sequence, when there is nothing to decode and when last buffered frame has
     been given away */
    return status;
}

DecHandle VPUDecodingSession::open_decoder(VPUDecoderBuffers &buffers, CodStd bitstream_format,
                                           const FrameGeometry &frame_geometry, bool reordering)
{
    DecOpenParam open_param;
    ::memset(&open_param, 0, sizeof(open_param));
    open_param.bitstreamFormat = bitstream_format;
    /* Bitstream buffer is decoder input buffer, all data that is fed to decoder
     goes through it */
    open_param.bitstreamBuffer = buffers.get_bitstream_buffer().phy_addr;
    open_param.bitstreamBufferSize = buffers.get_bitstream_buffer().size;
    open_param.qpReport = 0;
    open_param.mp4DeblkEnable = 0;

    /* VPU decoder uses interleaved chromas (NV12, two planes) instead of
     separate chroma planes (YUV420, three planes) because 1) VPU documentation
     says "Performance is better both on the VPU and IPU when chroma interleave
     mode is enabled" and 2) This creates memory layout that allows using GL_VIV
     extensions to access lumas via one GL texture unit and interleaved chromas
     via another texture unit, making manual colorspace conversion shader easier
     to write and more texture cache friendly. */
    open_param.chromaInterleave = 1; /* We intearleave the chromas - NV12 format */
    open_param.filePlayEnable = 0; /* Documentation says this is not used on i.MX6 */
    open_param.picWidth = frame_geometry.m_true_width;
    open_param.picHeight = frame_geometry.m_true_height;
    open_param.avcExtension = 0; /* We do not use MVC AVC stereo extensions */
    open_param.dynamicAllocEnable = 0; /* Not used on i.MX6 */
    open_param.streamStartByteOffset = 0; /* Not using extra offset in buffer */
    open_param.mjpg_thumbNailDecEnable = 0; /* Not decoding MJPG */
    /* Second H264 buffer is parameter set save buffer, where SPSes and PPSes
     go. Technically we could make this very small as we keep parameter sets in
     H264Parser anyway, but I am not sure how this buffer is maintained, for
     example how it decides that it can "let go" of old parameter set. So just
     in case... */
    open_param.psSaveBuffer = buffers.get_ps_save_buffer().phy_addr;
    open_param.psSaveBufferSize = buffers.get_ps_save_buffer().size;
    open_param.mapType = 0; /* Not sure what it is, but has to be 0 on i.MX6 */
    open_param.tiled2LinearEnable = 0; /* Ditto - has to do with mapping sth */

    /* "bitstream mode" is how VPU reacts to end of data in the buffer during
     the decode:
     - 0 - "wait for more data" mode - VPU just waits for more data to be fed,
     while interrupt is signalled to the user (so one can use vpu_WaitForInt())
     - 1 - "rollback" mode - VPU resets state to where decoding started.

     Previous code used rollback mode exclusively, but it caused performance
     penalties in every case when decode was called and bitstream buffer wasn't
     filled with complete frame data (like not all h264 slices of particular
     frame, for example) - time spent on decoding attempt then was simply
     wasted, as next time decode was restarted from the very begin.

     For this version of code, I spend a lot of time trying to make "wait for
     more data" mode work, in hope that it will allow for things like
     simultaneous receive and decode for multislice frames, thereby allowing to
     cut latency down. Unfortunately, for the life of me I can't make it work
     with all streams, it seems to just hang when image slices are really small
     (like below 100 bytes). So making do with rollback mode, OFC this version
     never starts decode without making sure that complete frame is properly fed

     If one wants to experiment with "wait for more data" mode mode, it should
     be enough to set this to zero. Current code is very careful and what works
     perfectly in rollback mode (never causing any rollback) in theory should
     work just as well in "wait for more data" mode (never causing decoder to
     wait). But for me it just starts timing out...so unless something changes
     in the future, we are using rollback mode _exclusively_ */
    open_param.bitstreamMode = 1;

    /* Set reordering according to flag. Reordering is needed for decoding of
     streams that contain B frames, but it has costs in both number of frame
     buffers needed and latency. So it should be used only when necessary */
    open_param.reorderEnable = reordering ? 1 : 0;

    /* Not using JPEG line buffer */
    open_param.jpgLineBufferMode = 0;
    DecHandle handle;
    if (RETCODE_SUCCESS != vpu_DecOpen(&handle, &open_param)) {
        /* Now if that doesn't work, not much we can do */
        return nullptr;
    }

    return handle;
}
}
