/*
 * Copyright (c) 2018-2019  AIRTAME ApS
 * All Rights Reserved.
 *
 * See LICENSE.txt for further information.
 */

#include <assert.h>
#include <string.h>
#include <time.h>
#include "vpu_decoding_session.hpp"

namespace airtame {

VPUDecodingSession::VPUDecodingSession(CodecLogger &logger, DecodingStats &stats,
                                       VPUDecoderBuffers &buffers, FrameGeometry frame_geometry,
                                       bool wait_for_frames, bool reordering)
    : m_logger(logger)
    , m_stats(stats)
    , m_buffers(buffers)
    , m_frame_geometry(frame_geometry)
    , m_should_wait_for_all_frames(wait_for_frames)
    , m_reordering(reordering)
    , m_vpu(vpu_error_function, this)
{
}

VPUDecodingSession::~VPUDecodingSession()
{
    close_decoder();
}

/* So this a beast of a function, but I wanted it to be atomic operation from
 higher level point of view. Technically, it could also be ctor, because it
handles errors internally and always returns success to the upper level, but
I like the idea of putting dumb decoding session never to be opened during the
initialization (ctor call) of decoder, which simplifies things there a bit.
Data comes in two parts because for VP8 we need to feed IVF headers before
keyframe */
void VPUDecodingSession::open(CodStd bitstream_format, FrameBuffer frame_buffer_template,
                              size_t frame_size, size_t number_of_display_frames,
                              Timestamp timestamp, bool is_frame, Buffer parts[2])
{
    /* We expect bitstream buffer to be mapped to process address space */
    assert(m_buffers.get_bitstream_buffer().virt_uaddr);
    assert(DecoderState::DECODER_CLOSED == m_state);

    /* Verify geometry (was passed to the ctor). Specs say that DG1/DG2 can
     decode all video codecs "up to 1920x1088". But this shouldn't be taken as
     width/height limit - we know for a fact that we can decode 1080x1920.
     So it is more like number of macroblocks limit -> memory/bandwidth/time
     limit.

     Now, 1920x1088 frame has 120x68 macroblocks, for a total of 8160 mbs.
     And this is what we are going to check against */

    /* It is safe to divide padded width/height by 16, because padding is to the
     macroblock boundary */
    size_t horizontal_mbs = m_frame_geometry.m_padded_width / 16,
           vertical_mbs = m_frame_geometry.m_padded_height / 16,
           total_mbs = horizontal_mbs * vertical_mbs;

    if (8160 < total_mbs) {
        codec_log_error(m_logger, "Frame has more macroblocks (%zu) than Full HD "
                                  "image has (8160). VPU can't decode such frames",
                      total_mbs);
        return;
    }

    /* First thing is to open decoder. This is where we decide on bitstream
     format, reordering and also pass in buffers */
    if (!open_decoder(bitstream_format, m_reordering)) {
        close_decoder();
        return;
    }

    /* Once decoder was opened, we need to get initial info out of it. This means
     feeding of first stream chunks (SPS to be activated in case of H264, or
     IVF sequence/frame header and first keyframe of VP8). We can be sure that
     there is enough space in the bitstream buffer for these chunks, because
     caller makes sure bitstream buffer is bigger than sum of initial chunks
     sizes _and_ bitstream buffer is empty now */
    DecInitialInfo initial_info;
    if (!get_initial_info(timestamp, is_frame, parts, initial_info)) {
        /* Still safe to close this way */
        close_decoder();
        return;
    }

    size_t buffers_size = m_buffers.get_bitstream_buffer().size + m_buffers.get_slice_buffer().size
        + m_buffers.get_ps_save_buffer().size;
    size_t wanted_buffers = initial_info.minFrameBufferCount + number_of_display_frames;

    codec_log_info(m_logger,
                   "Initial decoder info retrieved. Decoder wants %d "
                   "frame buffers and display reserve is %zu for a total "
                   "of %zu buffers %.2fMB each (%.1fMB total including bitstream "
                   "and other auxilary buffers).",
                   initial_info.minFrameBufferCount, number_of_display_frames, wanted_buffers,
                   double(frame_size) / (1024 * 1024),
                   double(frame_size * wanted_buffers + buffers_size) / (1024 * 1024));

    /* And finally, we got to allocate frame buffers and finish setup */
    if (!allocate_frames(frame_buffer_template, frame_size, initial_info.minFrameBufferCount,
                         number_of_display_frames)) {
        close_decoder();
        return;
    }

    /* Success! */
    m_stats.update_dma_allocation_size(frame_size * wanted_buffers + buffers_size);

    /* And try to decode - for example VP8 adds first keyframes as initial info,
     so should be possible to get something back */
    VPU::WaitResult result;
    if (wait_for_possible_decoded_frames(result)) {
        /* No error here means all is well */
        m_state = DecoderState::DECODER_OPEN;
    } else {
        /* Note that even if we decode some frames in call above, they are still
         not taken outside, and so we still can just close_decoder() and switch
         to DECODER_CLOSED state just like that */
        codec_log_error(m_logger,
                        "Failed initial wait for decoded frames in session open() "
                        "function, will skip rest of this sequence");
        close_decoder();
    }
}

/* Data comes in three parts, because for H264 we may want to feed SPS/PPS/Slice
 and for VP8 we feed IVF header/frame */
bool VPUDecodingSession::feed(Timestamp timestamp, bool is_frame, Buffer parts[3])
{
    if (DecoderState::DECODER_CLOSED == m_state) {
        /* Decoder is closed so skipping these chunks. */
        return true;
    }
    if (DecoderState::DECODER_OPEN != m_state) {
        /* Flushing or waiting, do not skip chunks, they might be useful in
         next session */
        return false;
    }

    /* OK, decoder is open */
    size_t size = parts[0].size + parts[1].size + parts[2].size;
    size_t space_available;
    if (!m_vpu.get_bitstream_buffer_free_space_available(m_handle, space_available)) {
        codec_log_error(m_logger, "Couldn't determine free space available on "
                                  "bitstream buffer");
        decoder_error();
        return false; /* We want to retry with given data again */
    }

    /* Check if bitstream buffer can contain the data */
    if (size > space_available) {
        /* Oooops. Nothing we can do within this session. Upper level will
         re-open with proper buffer sizes, but some frames will have to be
         lost as reopening is possible only on keyframes */
        codec_log_error(m_logger,
                        "Bitstream buffer free space too small (%d), %zu is "
                        "needed. Looks like we are not on keyframe, "
                        "will have to re-open the decoder!",
                        space_available, size);

        /* Flush to output any buffered frames that can go out */
        flush();
        /* IMPORTANT: we return false here, because if data that couldn't fit to
         bitstream was (which is likely) h264 IDR frame (or similar for other
         codecs) then we want to re-use that very frame in opening attempt! */
        return false;
    }

    /* Save bitstream buffer write index that will be begin of this chunk */
    size_t begin;
    if (!m_vpu.get_bitstream_buffer_write_index(m_handle, m_buffers.get_bitstream_buffer(),
                                                begin)) {
        codec_log_error(m_logger,
                        "Couldn't get initial bitstream write index before "
                        "feeding");
        decoder_error();
        return false;
    }

    if (!feed_data(parts[0]) || !feed_data(parts[1]) || !feed_data(parts[2])) {
        codec_log_error(m_logger, "Couldn't feed the decoder");
        decoder_error();
        return false;
    }

    /* Now get bitstream buffer write index again */
    size_t end;
    if (!m_vpu.get_bitstream_buffer_write_index(m_handle, m_buffers.get_bitstream_buffer(), end)) {
        codec_log_error(m_logger,
                        "Couldn't get initial bitstream write index after "
                        "feeding");
        decoder_error();
        return false;
    }

    /* Great, just have to add information to chunk queue */
    m_bitstream_monitoring.push_chunk(begin, end, timestamp, is_frame);

    /* And try to decode */
    VPU::WaitResult result;
    if (!wait_for_possible_decoded_frames(result)) {
        codec_log_error(m_logger,
                        "Failed decoding, will skip the rest of the sequence");
        decoder_error();
        return false;
    }
    return true; /* NAL consumed */
}

void VPUDecodingSession::flush()
{
    if (DecoderState::DECODER_CLOSED == m_state) {
        /* Nothing to do. This may happen if we were called from
         reset() method being called from parser detecting stream
         error even before decoder was opened for the first time */
        return;
    }

    if (DecoderState::DECODER_OPEN == m_state) {
        codec_log_info(m_logger, "Flushing the decoder");
        /* If we are here that means we have to initialize decoder flushing */
        /* Tell decoder that it is at the end of input, and so should output
         all buffered frames */
        if (RETCODE_SUCCESS != vpu_DecUpdateBitstreamBuffer(m_handle, 0)) {
            codec_log_error(m_logger,
                            "Failed sending end of stream to decoder "
                            "while flushing, will close immediately");
            decoder_error();
            return;
        }
        m_state = DecoderState::DECODER_FLUSHING;
    }

    /* This is a while loop, because we want to get it over
     with soon, but it could also be one call at a time, then
     we'd produce one frame for one call here */
    while (DecoderState::DECODER_FLUSHING == m_state) {
        VPU::WaitResult result;
        if (!wait_for_possible_decoded_frames(result)) {
            codec_log_info(m_logger,
                           "Error when waiting for decoded frame while "
                           "flushing, will close immediately");
            decoder_error();
            return;
        }

        if (VPU::WaitResult::NOT_ENOUGH_OUTPUT_BUFFERS & result) {
            /* This is expected - decoder has used up all output
             buffers available, so we have to wait for these to
             return after display */
            return;
        }

        if ((VPU::WaitResult::END_OF_SEQUENCE & result)
            || (VPU::WaitResult::NOT_ENOUGH_INPUT_DATA & result)) {
            /* Finally - decoder has eaten through all previously input
             data and is ready to close */
            close_decoder();
            /* State depends on m_should_wait_for_all_frames thought */
            m_state = (m_should_wait_for_all_frames && !all_frames_returned())
                ? DecoderState::DECODER_WAITING_FOR_ALL_FRAMES
                : DecoderState::DECODER_CLOSED;
        }
    }
}

void VPUDecodingSession::return_output_frame(unsigned long physical_address)
{
    size_t idx;
    /* Now we have to identify which frame it was using the physical address */
    for (idx = 0; idx < m_frames.size(); idx++) {
        if (m_frames[idx].dma->phy_addr == physical_address) {
            /* Frame found */
            m_frames[idx].reserved_for_display = false;
            break;
        }
    }
    /* We might be given a frame that is not in use. This is because DMA buffers
     are now reference counted so the client might hold on to them after the
     decoder is flushed and reset.

     So do nothing if an invalid frame is given. */
    if (idx >= m_frames.size()) {
        return;
    }

    if ((DecoderState::DECODER_WAITING_FOR_ALL_FRAMES == m_state)
        && all_frames_returned()) {
        /* All frames were returned, now can safely close decoder */
        codec_log_info(m_logger, "All frames returned OK, closing decoder");
        close_decoder();
        m_state = DecoderState::DECODER_CLOSED;
    }

    if ((DecoderState::DECODER_OPEN == m_state) || (DecoderState::DECODER_FLUSHING == m_state)) {
        /* OK, we are returning frame to functioning decoder, and having frame
         available may allow for decoding, so tell VPU that it can now re-use buffer */
        if (RETCODE_SUCCESS != vpu_DecClrDispFlag(m_handle, idx)) {
            /* Not much we can do here, and it means that decoder will run out
             of memory soon, so... */
            codec_log_fatal(m_logger, "Could not return displayed frame back to decoder");
            decoder_error();
            return;
        }

        /* Now it may be possible to get some frames out of decoder that were
         stuck in input bitstream buffer due to lack of output buffers. */
        VPU::WaitResult result;
        if (!wait_for_possible_decoded_frames(result)) {
            codec_log_error(m_logger,
                            "Error while returing displayed frame, skipping "
                            "the rest of the sequence");
            decoder_error();
            return;
        }
    }
}

/* This is called when some operation encountered error. We won't be flushing
 (it will most likely fail with decoder in erroneous state anyway) but we may
 want to go through waiting for all frames still */
void VPUDecodingSession::decoder_error()
{
    close_decoder();
    /* State depends on m_should_wait_for_all_frames thought */
    m_state = (m_should_wait_for_all_frames && !all_frames_returned())
        ? DecoderState::DECODER_WAITING_FOR_ALL_FRAMES
        : DecoderState::DECODER_CLOSED;
}

bool VPUDecodingSession::open_decoder(CodStd bitstream_format, bool reordering)
{
    DecOpenParam open_param;
    ::memset(&open_param, 0, sizeof(open_param));
    open_param.bitstreamFormat = bitstream_format;
    /* Bitrstream buffer is decoder input buffer, all data that is fed to decoder
     goes through it */
    open_param.bitstreamBuffer = m_buffers.get_bitstream_buffer().phy_addr;
    open_param.bitstreamBufferSize = m_buffers.get_bitstream_buffer().size;
    open_param.qpReport = 0;
    open_param.mp4DeblkEnable = 0;

    /* VPU decoder now uses interleaved chromas (NV12, two planes) instead of
     separate chroma planes (YUV420, three planes) because 1) VPU documentation
     says "Performance is better both on the VPU and IPU when chroma interleave
     mode is enabled" and 2) This creates memory layout that allows using
     GL_VIV extensions to access lumas via one GL texture unit and interleaved
     chromas via another texture unit, making manual colorspace conversion
     shader easier to write and more texture cache friendly.

     This is also fixed in display module, so assuming NV12 throught the code. */
    open_param.chromaInterleave = 1; /* We intearleave the chromas - NV12 format */
    open_param.filePlayEnable = 0; /* Documentation says this is not used on i.MX6 */
    open_param.picWidth = m_frame_geometry.m_true_width;
    open_param.picHeight = m_frame_geometry.m_true_height;
    open_param.avcExtension = 0; /* We do not use MVC AVC stereo extensions */
    open_param.dynamicAllocEnable = 0; /* Not used on i.MX6 */
    open_param.streamStartByteOffset = 0; /* Not using extra offset in buffer */
    open_param.mjpg_thumbNailDecEnable = 0; /* Not decoding MJPG */
    /* Second H264 buffer is parameter set save buffer, where SPSes and PPSes
     go. Technically we could make this very small as we keep parameter sets in
     H264Parser anyway, but I am not sure how this buffer is maintained, for
     example how it decides that it can "let go" of old parameter set. So just
     in case... */
    open_param.psSaveBuffer = m_buffers.get_ps_save_buffer().phy_addr;
    open_param.psSaveBufferSize = m_buffers.get_ps_save_buffer().size;
    open_param.mapType = 0; /* Not sure what it is, but has to be 0 on i.MX6 */
    open_param.tiled2LinearEnable = 0; /* Ditto - has to do with mapping sth */
    open_param.bitstreamMode = 1; /* How VPU signals that buffer is empty, and
                                   this is "rollback mode" i.e. no interrupt but
                                   if read pointer reaches write pointer then
                                   rollback is made to last NAL start (I presume) */
    /* Set reordering according to flag. Reordering is needed for decoding of
     streams that contain B frames, but it has costs in both number of frame
     buffers needed and latency. So it should be used only when necessary */
    open_param.reorderEnable = reordering ? 1 : 0;
    /* Not using JPEG line buffer */
    open_param.jpgLineBufferMode = 0;
    if (RETCODE_SUCCESS != vpu_DecOpen(&m_handle, &open_param)) {
        /* Now if that doesn't work, not much we can do */
        codec_log_error(m_logger, "Can't open the decoder");
        return false;
    }
    codec_log_info(m_logger, "Decoder in %s mode", (bitstream_format == STD_AVC) ? "H264" : "VP8");
    return true;
}

bool VPUDecodingSession::get_initial_info(Timestamp timestamp, bool is_frame, Buffer parts[2],
                                          DecInitialInfo &initial_info)
{
    /* We have opened decoder, then we can feed in initial chunks (for example
     SPS for H264, IVF headers and keyframe for VP8) to get initial info about
     resources needed for decoding of stream */
    size_t space_available = 0;
    if (!m_vpu.get_bitstream_buffer_free_space_available(m_handle, space_available)
        || (space_available < (parts[0].size + parts[1].size))) {
        /* This is kinda impossible, but hey, lets be a bit paranoid */
        codec_log_error(m_logger, "Bitstream input buffer too small to fit initial chunks");
        return false;
    }

    /* Bitstream buffer write index points to being of chunks we are about to
     feed */
    size_t begin;
    if (!m_vpu.get_bitstream_buffer_write_index(m_handle, m_buffers.get_bitstream_buffer(),
                                                begin)) {
        codec_log_error(m_logger,
                        "Couldn't get initial bitstream write index before "
                        "initial chunks");
        return false;
    }
    /* IMPORTANT: note that we return immediatelly on error here. No flushing!
     This is because we're just feeding initial information, just after opening,
     and so decoder can't contain any buffered frames. */
    if (!feed_data(parts[0]) || !feed_data(parts[1])) {
        codec_log_error(m_logger, "Couldn't feed to get initial info from the decoder");
        return false;
    }

    /* IMPORTANT: please note that we only use initial_info for format and
     interlace checks. Apart from that, minFrameBufferCount member will be used
     to determine number of frames to allocate. What is important is that we
     take NO geometry information from initial info. This is because it is either
     not working at all (like crop information for H264), or mislabelled or
     inconsistend between the codecs, for example picWidth/picHeight are padded
     (frame) dimensions for H264, but true (image) dimensions for VP8.
     So this data is obtained by parsing actual H264 SPS or VP8 keyframe by hand
   */
    if (!m_vpu.get_initial_info(m_handle, initial_info)) {
        codec_log_error(m_logger, "Couldn't get initial info from the decoder");
        return false;
    }

    if (FORMAT_420 != initial_info.mjpg_sourceFormat) {
        codec_log_error(m_logger, "Stream not in YUV420 format, cannot display it");
        return false;
    }

    if (initial_info.interlace) {
        /* Analog TV is long dead and we are playing back real-time encoded
         streams. Plus, this gives us simpler alignment calculations */
        codec_log_error(m_logger, "Stream is interlaced, cannot display it");
        return false;
    }
    size_t end;
    if (!m_vpu.get_bitstream_buffer_write_index(m_handle, m_buffers.get_bitstream_buffer(), end)) {
        codec_log_error(m_logger,
                        "Couldn't get initial bitstream write index after "
                        "initial info retrieval");
        return false;
    }
    /* Finally, add information to chunk queue */
    m_bitstream_monitoring.push_chunk(begin, end, timestamp, is_frame);
    return true;
}

bool VPUDecodingSession::allocate_frames(FrameBuffer frame_buffer_template, size_t frame_size,
                                         size_t minimal_frame_buffer_count,
                                         size_t number_of_display_frames)
{
    /* We clear these only now, when reallocating new set. This is because even
     after decoding is finished some output frames may be held by client */
    m_frames.clear();
    m_frame_buffers.clear();

    /* Wanted buffers is computed this way because minimal_frame_buffers_count
     is how many frames decoder wants to be able just to _decode_ the stream:
     (Quote is from "i.MX6 VPU Application Programming Interface Reference
     Manual")
     (By "reference frame number" they seem to refer to SPS field
     "num_ref_frames")
     "When this option is disabled, the minimum number of frame buffers is
     reference frame number + 2. Whenever one frame decoding is complete,
     a display (or decoded) output is provided from the VPU, so the decoder
     operation is the same as a normal decoder operation.

     But when this option is enabled, the minimum number of frame buffers is
     MAX(reference frame number, 16) + 2 for the worst case. After decoding
     one frame, the VPU cannot provide a display output because display order
     can be different from the decoding order. In the worst case, the first
     display output is provided from the VPU after decoding 17 frames.
     Because of this characteristic of display reordering, the VPU AVC decoder
     always decodes display delay + 1 frames during the first call of the
     picture decoding when display reordering is enabled in the stream.

     In practice, there are many streams which do not use display reordering,
     but the flag in the header is enabled. In this case, the host application
     must allocate unnecessarily more frame buffers and apply large delays.
     Considering this practical cases, this option for forced-disable of
     display reordering is provided in the VPU API."

     I tested how would decoder work without any additional frame (assuming
     that it reserves at least one of these frames for display). It worked for
     most of the videos but for example hung in Florence and the Machine video
     "No light" decoded with enabled reordering. All frames were returned to
     the decoder, feeding/waiting code worked as well, but no frames were given
     for display. This always happened in same stream position.

     Meaning that just to be on the safe side we add up our number wanted
     for decoding to what decoder requests here, and it cannot be zero */

    assert(number_of_display_frames);
    size_t wanted_buffers = minimal_frame_buffer_count + number_of_display_frames;

    /* IMPORTANT: this is separated into two vectors, because we want to use
     m_frame_buffers as continuous array of FrameBuffer structures (this is what
     decoder needs anyway) */
    m_frame_buffers.reserve(wanted_buffers);
    m_frames.reserve(wanted_buffers);

    /* Prepare new buffers */
    for (size_t i = 0; i < wanted_buffers; i++) {
        /* Push back another FrameBuffer/FrameMemoryAndTimestamp pair */
        m_frame_buffers.push_back(frame_buffer_template);
        m_frame_buffers.back().myIndex = i;
        m_frames.push_back(VPUFrameMemoryAndTimestamp());
        m_frames.back().timestamp = 0;
        m_frames.back().timestamp_assigned = false;
        m_frames.back().reserved_for_display = false;
        /* Alloc DMA memory */
        m_frames.back().dma = allocate_dma(frame_size);
        if (!m_frames.back().dma) {
            codec_log_error(m_logger,
                            "Failed to allocate all decoder buffers - "
                            "exhausted DMA memory?");
            /* Remove pair just pushed, dtor assumes that there is DMA memory
             to release */
            m_frame_buffers.pop_back();
            m_frames.pop_back();
            return false;
        }
        /* fb_template was filled with offsets, and physical memory addresses
         are really required by VPU */
        m_frame_buffers.back().bufY += m_frames.back().dma->phy_addr;
        m_frame_buffers.back().bufCb += m_frames.back().dma->phy_addr;
        m_frame_buffers.back().bufCr += m_frames.back().dma->phy_addr;
        m_frame_buffers.back().bufMvCol += m_frames.back().dma->phy_addr;
    }

    /* Finally, we have to let decoder know the buffers it can use */
    DecBufInfo buf_info;
    ::memset(&buf_info, 0, sizeof(buf_info));
    buf_info.avcSliceBufInfo.bufferBase = m_buffers.get_slice_buffer().phy_addr;
    buf_info.avcSliceBufInfo.bufferSize = m_buffers.get_slice_buffer().size;
    buf_info.vp8MbDataBufInfo.bufferBase = m_buffers.get_mb_prediction_buffer().phy_addr;
    buf_info.vp8MbDataBufInfo.bufferSize = m_buffers.get_mb_prediction_buffer().size;

    if (RETCODE_SUCCESS
        != vpu_DecRegisterFrameBuffer(m_handle, m_frame_buffers.data(), wanted_buffers,
                                      frame_buffer_template.strideY, &buf_info)) {
        codec_log_error(m_logger, "Failed to register frame buffers");
        return false;
    }

    return true;
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

bool VPUDecodingSession::feed_data(Buffer &part)
{
    if (part.size) {
        /* Chunk part is not empty, feed it */
        return m_vpu.feed_data(m_handle, m_buffers.get_bitstream_buffer(), part.data, part.size);
    } else {
        /* No data inside, nothing to do */
        return true;
    }
}

/* Note that it may seem that other feeding mechanism is possible, for example
 we could (at least in theory) deduce when decoded frame will be available,
 or if there are enough output frames to even try decoding. Problem with that
 approach is (and I tried) that (for example) number of frames that decoder
 can release to display varies (depends on reordering, but also stream state).
 So minimum number of frames it can release is m_number_of_display_frames but
 in some streams I saw it releasing ALL frame buffers. Consequently, there is no
 way to say "hey, do not attempt to decode because decoder won't have place to
 put this frame".

 On the other hand, to avoid stalling or having input piling up on the module
 input side (and I saw this too) we want to feed the decoder as fast as
 possible. So instead of some smart "stream understanding" feeding algorithm we
 stuff the decoder with everything we got, and just make sure nothing gets lost
 - this is why feed() function checks for space in decoder bitstream buffer.

 Now that policy means that decoder can have more data that it can put away
 to display (and so, apart from decoder's "natural" buffering due to H264 it
 may have some frames in the input bitstream. So we should expect to get more
 than one frame here, hence loop in this function */
bool VPUDecodingSession::wait_for_possible_decoded_frames(VPU::WaitResult &result)
{
    /* Loop here because in some previous iteration we may fed decoder
     successfully only to have it answer that not enough output frames
     are available. So there may be more than one frame to get out of decoder.
     Another reason is that in H264 more than one frame can become displayable
     when feeding one frame. For example when I-frame comes and we are
     reordering, all older frames could become displayable */
    do {
        int decode_frame_idx, display_frame_idx;
        struct timespec before, after;

        clock_gettime(CLOCK_MONOTONIC, &before);
        DecParam params;
        ::memset(&params, 0, sizeof(params));
        if (!m_vpu.wait_for_decoding(m_handle, params, result,
                                     decode_frame_idx, display_frame_idx)) {
            codec_log_error(m_logger, "Failed H264VPU::wait_for_decoding!");
            return false;
        }

        clock_gettime(CLOCK_MONOTONIC, &after);
        int64_t delta_s = (int64_t)after.tv_sec - (int64_t)before.tv_sec;
        int64_t delta_ns = (int64_t)after.tv_nsec - (int64_t)before.tv_nsec;
        int64_t total_delta_ns = delta_s * 1000 * 1000 * 1000 + delta_ns;
        int64_t total_delta_ms = total_delta_ns / 1000 / 1000;
        m_stats.update_decode_timing((uint64_t)total_delta_ms);

        /* Update feeding flag */
        m_wants_data = (VPU::WaitResult::NOT_ENOUGH_INPUT_DATA & result) ?
            true : false;

        /* Check for parameters changed event */
        if (VPU::WaitResult::PARAMETERS_CHANGED & result) {
            /* So this isn't really VPU error, just signal that we should
             reconfigure decoder. But in this code we try to react to all
             possible stream reconfigurations in advance, and so it signals
             logical error in our code, so treat is as any other error */
            codec_log_error(m_logger,
                            "Decoder asked for parameters change, which "
                            "shouldn't really happen!");
            return false;
        }
        /* If frame was decoded, establish proper timestamp for it. BTW there
         is difference between decoded frame and frame available for display
         because frame just decoded may be "in future" in case of bidirectional
         streams, and is shouldn't be displayed right now. But NALs that make
         this frame up are already consumed and this is our only chance to get
         timestamp right */
        if (VPU::WaitResult::DECODED_FRAME & result) {
            assert(decode_frame_idx < (int)m_frame_buffers.size());
            /* Get bitstream read index from VPU */
            size_t read_idx;
            if (!m_vpu.get_bitstream_buffer_read_index(m_handle, m_buffers.get_bitstream_buffer(),
                                                       read_idx)) {
                codec_log_error(m_logger, "Can't get bitstream buffer read index");
                return false;
            }
            /* Now use that index to retrieve proper timestamp for NALs just
             consumed. NOTE: this is the only place where we can safely call
             update_queue, as it will always take some information off the
             queue */
            m_frames[decode_frame_idx].timestamp
                = m_bitstream_monitoring.update_queue(m_logger, read_idx);
            m_frames[decode_frame_idx].timestamp_assigned = true;
        }
        /* If frame is available, for display put it into output queue */
        if (VPU::WaitResult::FRAME_AVAILABLE_FOR_DISPLAY & result) {
            assert(display_frame_idx < (int)m_frame_buffers.size());
            /* OK, can fill data for DecodedVPUFrame_t */
            m_output_frames.push_back(VPUOutputFrame());
            /* This we take from dma buffer of the frame */
            m_output_frames.back().dma = m_frames[display_frame_idx].dma;
            m_output_frames.back().size = (size_t)m_frames[display_frame_idx].dma->size;
            m_output_frames.back().timestamp = m_frames[display_frame_idx].timestamp;
            m_output_frames.back().geometry = m_frame_geometry;
            if (!m_frames[display_frame_idx].timestamp_assigned) {
                /* Just warn, because what we can do? */
                codec_log_warn(m_logger, "Couldn't find proper timestamp for frame");
            }
            /* Reset kept timestamp */
            m_frames[display_frame_idx].timestamp = 0;
            m_frames[display_frame_idx].timestamp_assigned = false;
            m_frames[display_frame_idx].reserved_for_display = true;
        }
        /* Loop until decoder reports one of those:
         - lack of input data
         - lack of output frames
         - end of stream
         All these occur naturally and mean succesful finish to this call */
    } while (!(result & VPU::WaitResult::NOT_ENOUGH_INPUT_DATA)
             && !(result & VPU::WaitResult::NOT_ENOUGH_OUTPUT_BUFFERS)
             && !(result & VPU::WaitResult::END_OF_SEQUENCE));
    return true;
}

void VPUDecodingSession::close_decoder()
{
    /* We log errors here but ignore them - this can be called with decoder
     in a mess state anyway, so what we can do? */
    if (m_handle) {
        if (RETCODE_SUCCESS != vpu_DecBitBufferFlush(m_handle)) {
            codec_log_error(m_logger, "Could not flush decoder bit buffer");
        }
        /* This informs decoder of end of input data */
        if (RETCODE_SUCCESS != vpu_DecUpdateBitstreamBuffer(m_handle, 0)) {
            codec_log_error(m_logger, "Could not send EOS to decoder");
        }
        vpu_DecClose(m_handle);
        m_handle = 0;

    }
    m_bitstream_monitoring.clear();
}

bool VPUDecodingSession::all_frames_returned()
{
    for (size_t i = 0; i < m_frames.size(); i++)
        if (m_frames[i].reserved_for_display) {
            return false;
        }
    return true;
}

void VPUDecodingSession::vpu_error_function(const char *msg, void *user_data)
{
    VPUDecodingSession *thiz = static_cast<VPUDecodingSession *>(user_data);
    codec_log_error(thiz->m_logger, "VPU error: %s", msg);
}
}
