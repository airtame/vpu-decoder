/*
 * Copyright (c) 2018-2020  AIRTAME ApS
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
#include "vpu_decoder_buffers.hpp"
#include "vpu_frame_buffers.hpp"
#include "vpu_output_frame.hpp"

namespace airtame {

/* I read that for bitmasky things to work with class enum one has to define
 apropriate operators...talk about overkill, when it just worked in standard C
 */

enum class VPUDecodeStatus {
    NOTHING = 0,
    NO_FREE_OUTPUT_BUFFER = 1,
    OUTPUT_DECODED = 2,
    FRAME_GIVEN_FOR_DISPLAY = 4,
    NOT_ENOUGH_INPUT_DATA = 8,
    DECODE_TIMEOUT = 16,
    ERROR = 1024
};

inline void operator |= (VPUDecodeStatus &a, VPUDecodeStatus b)
{
    a = static_cast<VPUDecodeStatus>(static_cast<int>(a) | static_cast<int>(b));
}

inline constexpr bool operator & (const VPUDecodeStatus a, const VPUDecodeStatus b)
{
    return (static_cast<int>(a) & static_cast<int>(b)) ? true : false;
}

inline constexpr VPUDecodeStatus operator | (const VPUDecodeStatus a, const VPUDecodeStatus b)
{
    return static_cast<VPUDecodeStatus>(static_cast<int>(a) | static_cast<int>(b));
}

/* Purpose of this class is to encapsulate state and handling of VPU decoder to
 make it easier to use. And that's it. Unlike previous version of the code, it
 doesn't offer extra features like decoding state machine or bitstream buffer
 monitoring */
class VPUDecodingSession {
protected:
    CodecLogger &m_logger;
    DecodingStats &m_stats;

    /* These manage allocation (and other handling) of all DMA memory needed
     for decoding. */
    VPUDecoderBuffers &m_buffers;
    VPUFrameBuffers &m_frames;

    /* These are session invariants that we get from upper level - they get
     assigned when the session is open, and the only way to change these is to
     create new session */
    CodecType m_codec_type = CodecType::NONE;
    FrameGeometry m_frame_geometry;
    size_t m_number_of_reference_frame_buffers;
    size_t m_number_of_display_frame_buffers;
    bool m_reordering = false;

    /* Handle of the open decoder */
    DecHandle m_handle = nullptr;

    bool m_initial_info_retrieved = false;

    /* Disallow constructing session objects by the user */
    VPUDecodingSession(CodecLogger &logger, DecodingStats &stats, VPUDecoderBuffers &buffers,
                       VPUFrameBuffers &frames, CodecType codec_type, const FrameGeometry &frame_geometry,
                       size_t number_of_reference_frame_buffers, size_t number_of_display_frame_buffers,
                       bool reordering);

public:
    ~VPUDecodingSession();
    /* The only way to make decoding session objects is to call this function.
     nullptr will be returned on error */
    static VPUDecodingSession *open_for_video(CodecLogger &logger, DecodingStats &stats,
                                              VPUDecoderBuffers &buffers, VPUFrameBuffers &frames,
                                              CodecType codec_type, const FrameGeometry &frame_geometry,
                                              size_t number_of_reference_frame_buffers,
                                              size_t number_of_display_frame_buffers,
                                              bool reordering);
    /* For JPEG one doesn't create permanent decoding session, as there is no
     "state" to carry from one decode operation to the next (same applies to
     MJPEG streams)

     BUT - and this actually could be a TODO, one could add JPEG frame packs,
     and then JPEG would be just a pack of one, and MJPEG or several subsequent
     JPEG decodes could be just a series of JPEG frame packs, re-using the
     memory, allowing for automatic reaction to resolution changes and so on.
     Would require changes in VPUDecoder as well */
    static bool decode_jpeg(CodecLogger &logger, VPUDMAPointer bitstream,
                            VPUDMAPointer frame, const FrameGeometry &frame_geometry,
                            bool interleave);
    /* These are public and static because one needs them to prepare DMA memory
     for JPEG bitstream and frame buffer */
    static FrameBuffer prepare_nv12_frame_buffer_template(const FrameGeometry &frame_geometry);
    // TODO: this could be in more generic context, and could be used from
    // all "buffer" classes and so common code...
    static VPUDMAPointer allocate_dma(size_t size);

    /* Video decode operations, return false on error, true otherwise */
    /* This function adds data to bitstream input buffer (input of the decoder
     chip */
    bool feed(const unsigned char *data, size_t size, size_t &size_fed);
    /* This function feeds special "end of stream" marker, which is needed to
     retrieve remaining buffered frames out of the decoder (see decode()
     description below) */
    bool feed_end_of_stream();
    /* Utility function for monitoring bitstream buffer state */
    bool get_bitstream_buffer_free_space_available(size_t &size);

    /* This should be called before starting decoding, to make sure that there
     is at least one free frame for decode. */
    bool has_frame_for_decoding() const;

    /* This should be called by display part, to notify that the frame given
     away is no longer needed and decoder can reuse it */
    void return_output_frame(unsigned long physical_address);

    /* Is decoding still in progress? Returns true is so, false if it ended
     (or wasn't started in the first place). Safe to call at any time */
    bool is_busy();

    /* They say that good gasoline engine needs just two things to run, mixture
     and the spark. And when set up properly, VPU decoder is same: Given proper
     mixture of protocol chunks making up an encoded frame, and free output
     buffer to decode to, it will always do something. BUT, and this important,
     what actually _gets_ produced depends on reordering status:
     - When reordering is disabled, one encoded frame is given, and that very
     frame will be decoded to one of free buffers and given away for display.
     So there is always one decode, one display and it always involves given
     encoded frame.
     - When reordering is enabled, one encoded frame is given, and one of the
     following may happen:
        - Frame is decoded and given immediately for display (just like above),
        optionally it may also be buffered
        - Frame is decoded and _nothing_ is given for display at all (so decoded
        frame gets buffered for the future)
        - Frame is _not_ decoded and some previous frame is given for display
        - There is also special case of not having any more input to decode,
        but with pending buffered frames. Then special "end of stream" info
        has to be put into bitstream buffer (via feed_end_of_stream() call), and
        decoder will spit out remaining frames (previously buffered ones).
     - And finally, for extra safety (again, we are dealing with VPU VP8 decoder
     bug here) when decode() is called without free frame, NO_FREE_FRAME will
     be ORed into status, and no action will be performed.

     Arguments are - metadata to be assigned to decoded frame (if any), decoded
     flag which will be true if actual decode taken place, given_for_display
     which will be true if any frame gets returned for display. Frame(s) given
     for display will be added to output_frames list. */
    VPUDecodeStatus decode_video(const std::shared_ptr<FrameMetaData> &meta,
                                 VPUOutputFrame &output_frame);

    /* TODO: async decoding interface.
     Note that implementation of decoding is actually split into two private
     functions below: start_decoding() and wait_for_decode(). These could be
     exposed (either directly, or wrapped into some kind of interfaces) to allow
     for "async decode" functionality, starting VPU decode first, and allowing
     CPU do other stuff, then coming for results of the decode operation. There
     is also is_busy() for checking if decode is still in progress. */

    /* Accessors. Higher level code calls these and compares parameters required
     for decoding of subsequent frames. If any of these is not the same, session
     needs to be closed and reopened. */
    CodecType get_codec_type() const
    {
        return m_codec_type;
    }

    const FrameGeometry &get_frame_geometry() const
    {
        return m_frame_geometry;
    }

    size_t get_number_of_frame_buffers() const
    {
        return m_frames.get_number_of_reference_frame_buffers()
            + m_frames.get_number_of_display_frame_buffers();
    }

    bool get_reordering() const
    {
        return m_reordering;
    }

protected:
    /* Like described above, these could be made public in order to allow for
     async decoding. Both functions used for video only */
    bool start_video_decoding();
    VPUDecodeStatus wait_for_video_decode(const std::shared_ptr<FrameMetaData> &meta,
                                          VPUOutputFrame &output_frame);
    /* Other utilities */
    bool allocate_frames();
    bool get_initial_info(DecInitialInfo &initial_info);

    /* Static utilities - used before instance of DecodingSession is created, or
     also used when no instance is created (JPEG decoding) */
    static VPUDecodeStatus wait_for_decode(CodecLogger &logger, DecHandle handle,
                                           int &decoded_frame_buffer_index,
                                           int &display_frame_buffer_index);
    static DecHandle open_decoder(VPUDecoderBuffers &buffers, CodStd bitstream_format,
                                  const FrameGeometry &frame_geometry, bool reordering);
};
} // namespace airtame

