/*
 * Copyright (c) 2018-2020  AIRTAME ApS
 * All Rights Reserved.
 *
 * See LICENSE.txt for further information.
 */

#include <assert.h>
#include "h264_stream_parser.hpp"

/* H264Parser handles proper NAL-feeding for the decoder. This is because for
 H264 you can't really have "dumb" feeding of H264 data into decoder. At least,
 that HW decoder we are using.

 This is because in H264 streams NALs compose "video sequences". Video sequence
 consists of several pictures, of which first should be IDR picture. IDR stands
 for "Instant Decoder Refresh", meaning that at this point decoder state can be
 properly (re)initialized using just this one picture.

 Pictures are not single entities, but are subdivided into slices. Each picture
 may be composed of one or more slices. And in fact this subdivision is our
 first serious problem - in order to feed decoder properly, and to properly
 assign metadata such as timestamp or rotation to the decoded frames we need to
 know which picture slices make up which frame. In this version of the code it
 was finally solved by parsing up slice headers.

 Unfortunately the only way to know that we have received all the slices that
 make up current picture is to wait for first slice that starts next picture
 (or sequence end, or stream end). As far as I know, there is no way for H264
 to signal "this is last slice". All that is carried is frame identification, so
 one waits for "begin of a next frame" to understand that previous frame has
 ended.

 Second problem is that picture slices in H264 reference something called
 parameter sets. There are two kinds of these, sequence parameter set (SPS) and
 picture parameter set (PPS). They are kind of "context" messages that carry
 parameters common to whole video sequence (SPS) or one or more pictures (PPS).
 Now H264 standard allows those to be send anywhere in the stream, or indeed
 even out-of-band, provided that the proper SPS/PPS will be fed into decoder
 just before frame that refers to it.

 So for example stream goes on, decoder sends out frames and between other
 NALs fed it will get new SPS, which is not active just yet. "Old" sequence
 continues. Then after awhile IDR picture slice comes and refers that SPS, so
 current video sequence has to end.

 Decoder flushes the state, sends out all buffered frames and then it should
 re-initialize. Problem is that SPS frame got consumed with the "old sequence"
 and now new sequence doesn't have it anymore.

 This problem is worked around in this parser here just as recommended by H264
 standard - parameter sets are taken away from stream and kept in parser
 arrays (m_sequence_parameter_sets/m_picture_parameter_sets). Incoming slice
 NALs are analyzed and apropriate parameter sets are issued just in time. */

namespace airtame {

// TODO: this is for "Annex B", we could also have "AVCC"

// TODO: we could easily handle fragmented buffers now. It could help on
// the user side too - right now we kinda have two parsers - very simple
// one. To help with NALs broken by the buffer boundaries we could save
// three "carry" bytes from the end of the buffer to next one. Could have
// special field for that in Chunk/Pack
void H264StreamParser::process_buffer(const VideoBuffer &buffer)
{
    const unsigned char *limit = buffer.data + buffer.size;
    const unsigned char *current_nal = at_h264_next_start_code(buffer.data, limit);
    if (!current_nal) {
        /* No start code found, WTF is this? */
        codec_log_warn(m_logger,
                       "H264 parser given entire buffer without NAL "
                       "start code in it");
        return;
    }

    size_t bytes_consumed = current_nal - buffer.data;
    if ((bytes_consumed > 1) || ((bytes_consumed == 1) && (buffer.data[0]))) {
        /* Found start code, but it isn't very first thing in buffer, so
         issue a warning. It is OK for start code to be preceeded by one
         zero, though */
        codec_log_warn(m_logger,
                       "H264 NAL start code not the first thing in "
                       "given buffer, skipping %zu bytes",
                       bytes_consumed);
    }

    while (current_nal) {
        /* Find next start code - search start at current_nal + 4 because NAL
         begins with start code that is four bytes long and we want to skip it */
        const unsigned char *next_nal = at_h264_next_start_code(current_nal + 4, limit);
        /* Depending on whether it was found or not, current NAL limit is */
        const unsigned char *current_nal_limit = next_nal ? next_nal : limit;
        size_t current_nal_size = (size_t)(current_nal_limit - current_nal);
        /* Pass NAL to top level handler */
        handle_nal(buffer.meta, current_nal, current_nal_size);
        /* Decoder consumed this NAL, can move on */
        bytes_consumed += current_nal_size;
        current_nal = next_nal;
    }

    /* IMPORTANT - attach free callback from the buffer to the chunks parsed
     into video stream or we will "leak" this buffer */
    m_frames.attach_free_callback(buffer.free_callback);
}

void H264StreamParser::handle_nal(const std::shared_ptr<FrameMetaData> &meta,
                                  const unsigned char *nal, size_t size)
{
    /* Fourth byte of NAL (last byte of start code), five low bits are
     nal_unit_type */
    NalType type = NalType(nal[3] & 0x1f);
    /* Now route NAL to apropriate handler. NAL handlers return boolean
     that indicates whether NAL was consumed or not. If it wasn't consumed
     it will be re-fed in the future */
    switch (type) {
        case NalType::UNSPECIFIED_ZERO: {
            handle_unspecified_nal(nal, size);
            return;
        }

        case NalType::NON_IDR_SLICE: {
            handle_slice_nal(meta, nal, size, NalType::NON_IDR_SLICE);
            return;
        }

        case NalType::PARTITION_A_SLICE: {
            handle_slice_nal(meta, nal, size, NalType::PARTITION_A_SLICE);
            return;
        }

        case NalType::IDR_SLICE: {
            handle_slice_nal(meta, nal, size, NalType::IDR_SLICE);
            return;
        }

        case NalType::PARTITION_B_SLICE: {
            handle_bc_partition_nal(nal, size, NalType::PARTITION_B_SLICE);
            return;
        }

        case NalType::PARTITION_C_SLICE: {
            handle_bc_partition_nal(nal, size, NalType::PARTITION_C_SLICE);
            return;
        }

        case NalType::SUPPLEMENTAL_ENHANCED_INFORMATION: {
            handle_sei_nal(nal, size);
            return;
        }

        case NalType::SEQUENCE_PARAMETER_SET: {
            handle_sps_nal(nal, size);
            return;
        }

        case NalType::PICTURE_PARAMETER_SET: {
            handle_pps_nal(nal, size);
            return;
        }

        case NalType::ACCESS_UNIT_DELIMITER: {
            /* Just throw it away, they are not needed for decoding, and for
             example streams generated by Microsoft H264 software encoders tend
             to have a lot of these */
            return;
        }

        case NalType::END_OF_SEQUENCE: {
            handle_end_of_sequence_nal();
            return;
        }

        case NalType::END_OF_STREAM: {
            handle_end_of_stream_nal();
            return;
        }

        case NalType::FILLER: {
            /* Just throw it away, this is just to pad network packets to
             constant size or some such */
            return;
        }

        default: {
            if ((NalType::FIRST_RESERVED_TYPE <= type) && (type <= NalType::LAST_RESERVED_TYPE)) {
                handle_reserved_nal(nal, size);
                return;
            } else if ((NalType::FIRST_UNSPECIFIED_TYPE <= type)
                       && (type <= NalType::LAST_UNSPECIFIED_TYPE)) {
                handle_unspecified_nal(nal, size);
                return;
            } else {
                /* This should never happen, but to avoid infinite loop in
                 production environment, like "Twitch problem" of March 2019 */
                codec_log_error(m_logger, "Unexpected NAL type %d, eating it up "
                                          "to avoid infinite loop", type);
                assert(0); /* We want to notice in debug environments */
                return;
            }
        }
    }
}

/* All cited text from ITU-T H.264 standard (2003 version) */

/* H264 standard: on the parameter sets (SPS and PPS):
 "NOTE – When a NAL unit having nal_unit_type equal to 7 or 8 is present in an
 access unit, it may or may not be referred to in the coded pictures of the
 access unit in which it is present, and may be referred to in coded pictures
 of subsequent access units."

 Also (7.4.1.2.1):

 "NOTE – The sequence and picture parameter set mechanism decouples
 the transmission of infrequently changing information from the transmission of
 coded macroblock data. Sequence and picture parameter sets may, in some
 applications, be conveyed "out-of-band" using a reliable transport mechanism."

 Meaning that proper SPS/PPS handling involves keeping previous ones because
 at least in theory they can be recalled by some slice even after end of
 access unit.

 Also (7.4.2.1) on SPS:
 "NOTE – When feasible, encoders should use distinct values of
 seq_parameter_set_id when the values of other sequence parameter set syntax
 elements differ rather than changing the values of the syntax elements
 associated with a specific value of seq_parameter_set_id."

 My comment is this: "when feasible" is next to useless to us, especially
 so that streaming solutions probably retransmit SPS/PPS every once in a while,
 for example to allow connection of new clients in the middle of the stream

 IMPORTANT: SPS are saved regardless of synchronized status, because on getting
 IDR slice we need to have at least one SPS/PPS to even think about syncing
 back */
void H264StreamParser::handle_sps_nal(const unsigned char *nal, size_t size)
{
    /* Parse SPS */
    SpsNalInfo sps;
    if (!at_h264_get_sps_info(nal, size, sps)) {
        codec_log_error(m_logger, "Failed to parse SPS");
        return;
    }
    int sps_id = sps.seq_parameter_set_id;

    /* OK, update SPS at sps_id */
    if (m_sequence_parameter_sets[sps_id].update(nal, size, -1, sps)) {
        /* Check currently active SPS */
        int pps_id = m_current_picture_slice_header.pic_parameter_set_id;
        const NALParameterSet<PpsNalInfo> &pps = m_picture_parameter_sets[pps_id];
        if (pps.get_referred_index() == sps_id) {
            /* Will have to reactivate SPS, as currently active one got replaced
             with different content. Technically this should only happend on
             video sequence boundary, but it doesn't hurt to handle it this way */
            m_current_picture_slice_header.pic_parameter_set_id = -1;
        } /* Just keep that SPS, nothing changes */
    }
}

/* See comments about SPS above and also, H264 standard: 7.4.2.2 on PPS:
 "pic_parameter_set_id identifies the picture parameter set that is referred to
 in the slice header. The value of pic_parameter_set_id shall be in the range of
 0 to 255, inclusive.
 seq_parameter_set_id refers to the active sequence parameter set. The value of
 seq_parameter_set_id shall be in the range of 0 to 31, inclusive.

 IMPORTANT: same as with SPS, we save PPS regardless of sync status */
void H264StreamParser::handle_pps_nal(const unsigned char *nal, size_t size)
{
    /* Parse PPS */
    PpsNalInfo pps;
    if (!at_h264_get_pps_info(nal, size, pps)) {
        codec_log_error(m_logger, "Failed to parse PPS");
        return;
    }
    int sps_id = pps.seq_parameter_set_id;

    if (!m_sequence_parameter_sets[sps_id].get_size()) {
        codec_log_error(m_logger, "PPS parse error: sps_id refers to unknown SPS");
        return;
    }
    int pps_id = pps.pic_parameter_set_id;

    /* OK, update PPS at pps_id */
    if (m_picture_parameter_sets[pps_id].update(nal, size, sps_id, pps)) {
        /* PPS changed, see if it was active one */
        if (m_current_picture_slice_header.pic_parameter_set_id == pps_id) {
            /* Active PPS changed, next reference will require re-activation */
            m_current_picture_slice_header.pic_parameter_set_id = -1;
        }
    }
}

/* "An activated sequence parameter set RBSP shall remain active for the entire
 coded video sequence.
 NOTE – Because an IDR access unit begins a new coded video sequence and an
 activated sequence parameter set RBSP must remain active for the entire coded
 video sequence, a sequence parameter set RBSP can only be activated by a
 buffering period SEI message when the buffering period SEI message is part of
 an IDR access unit."

 So SPS "activation" is possible in IDR slice or in buffering period SEI that
 immediately precedes it, but anyway both have to refer to same SPS. So we can
 well ignore buffering period SEI as a source of SPS activation, and just use
 IDR frame slice for that. Also, see comment before handle_sei_nal()

 Also, this is the only NAL which allows us to sync with stream, as, by
 definition "After the decoding of an IDR picture all following coded pictures
 in decoding order can be decoded without inter prediction from any picture
 decoded prior to the IDR picture. The first picture of each coded video
 sequence is an IDR picture" */
void H264StreamParser::handle_slice_nal(const std::shared_ptr<FrameMetaData> &meta,
                                        const unsigned char *nal, size_t size,
                                        NalType slice_type)
{
    /* One caveat here - if we are going to be fed by "random access" h264
     stream, we could possibly mistaken _not first_ slice of IDR picture as
     first one (should we jump right in the middle of multislice IDR picture).

     BUT it should still be safe because then no SPS/PPS preceding IDR slices
     will get received and processed, and parse_slice_header will fail with
     lack of SPS/PPS error */
    SliceHeaderInfo slice_header_info;
    if (!parse_slice_header(nal, size, slice_header_info)) {
        return;
    }

    /* Validity of SPS gets checked when reading PPS, so doesn't have to check
     now, and particular PPS needed is checked in parse_slice_header */
    int pps_id = slice_header_info.pic_parameter_set_id;
    const NALParameterSet<PpsNalInfo> &pps = m_picture_parameter_sets[pps_id];
    int sps_id = pps.get_referred_index();
    const NALParameterSet<SpsNalInfo> &sps = m_sequence_parameter_sets[sps_id];
    const char *description;
    if (at_h264_are_different_pictures(m_current_picture_slice_header,
                                       slice_header_info)) {
        /* FIRST SLICE of new frame, gotta switch frames */
        m_current_picture_slice_header = slice_header_info;
        /* Start a new frame */
        m_frames.push_new_pack();
        m_frames.back().m_codec_type = CodecType::H264;
        m_frames.back().m_geometry
            = FrameGeometry(sps.get_info().padded_frame_width, sps.get_info().padded_frame_height,
                            sps.get_info().true_frame_width, sps.get_info().true_frame_height,
                            sps.get_info().true_crop_offset_left, sps.get_info().true_crop_offset_top);
        // TODO: write down description about how it is enough to have as many,
        // and how our decoder wants +2
        m_frames.back().m_maximum_number_of_reference_frames = sps.get_info().num_ref_frames + 2;
        m_frames.back().m_can_be_dropped = slice_header_info.ref_nal_idc ? false : true;
        m_frames.back().m_is_complete = false;
        m_frames.back().meta = meta;
        // TODO: use profile from SPS to set this properly! Right now setting
        // reordering for all H264 streams which is far from optimal. Have override
        // to disable reordering for realtime streaming as well!
        m_frames.back().m_needs_reordering = m_force_disable_reordering ? false : true;
        m_frames.back().m_needs_flushing = false;
        if (NalType::IDR_SLICE == slice_type) {
            /* IDR slice can reopen the decoder */
            m_frames.back().m_can_reopen_decoding = true;
            /* Note that we ALWAYS add SPS and PPS to FIRST IDR slice. This is
             because we are not sure when decoder will decide to reopen itself,
             we are just generating stream of frame packs. So always equip IDR
             frame with both parameter sets */
            m_frames.push_chunk(sps.get_data(), sps.get_size(), "SPS");
            m_frames.push_chunk(pps.get_data(), pps.get_size(), "PPS");
            description = "First IDR slice";
        } else {
            m_frames.back().m_can_reopen_decoding = false;
            /* NON-IDR slices can only switch PPSes, see if it does so. Even if
             it does, standard says that this PPS has to refer currently active
             SPS so feed PPS only */
            if (m_current_picture_slice_header.pic_parameter_set_id
                    != slice_header_info.pic_parameter_set_id) {
                m_frames.push_chunk(pps.get_data(), pps.get_size(), "PPS");
            }
            description = "First slice";
        }
    } else {
        /* Not first slice of a frame */
        description = (NalType::IDR_SLICE == slice_type) ? "IDR slice" : "slice";
        /* IMPORTANT: if frame is spread across multiple input video buffers,
         they can have multiple metadata. This code will naturally use metadata
         of the buffer that contained very first slice, but if if one wanted to
         process multiple metadata properly, something like this could be used:

         if (meta && (m_frames.back().meta != meta)) {
            // for example append "new" metadata to "old"
         }

         But tread carefully here. Metadata of first slice (such as timestamp)
         probably makes most sense anyway.
         */
    }
    m_frames.push_chunk(nal, size, description);
}

/* This is just partition slice B or C, which continue from previous partition A
 slice, which contained header and was already accepted by decoder, because we
 got here, so nothing special to do */
void H264StreamParser::handle_bc_partition_nal(const unsigned char *nal, size_t size,
                                               NalType partition_type)
{
    /* Append NAL to frame */
    m_frames.push_chunk(nal, size,
                        (NalType::PARTITION_B_SLICE == partition_type)
                            ? "Partition B" : "Partition C");
}

/* Standard page 50:
  "NOTE – Because an IDR access unit begins a new coded video sequence and an
 activated sequence parameter set RBSP must remain active for the entire coded
 video sequence, a sequence parameter set RBSP can only be activated by a
 buffering period SEI message when the buffering period SEI message is part of
 an IDR access unit". So technically buffering period SEI can activate SPS
 (activation in here should be understood as "first use of that SPS"
 But:
 "Supplemental Enhancement Information (SEI) contains information that is not
 necessary to decode the samples of coded pictures from VCL NAL units." and also
 we know that SPS activation is allowed only once per sequence, and that
 sequence begins with IDR header slice NAL which (if preceded by buffering
 period SEI activating certain SPS) has to refer to same SPS.

 So it looks like SEIs could be thrown away. Especially so that I was not able
 to find the stream which contained buffering period SEI and most of the stream
 seem to be without SEIs at all or contains SEI types irrelevant for our work
 (like some kind of markers). */
void H264StreamParser::handle_sei_nal(const unsigned char *nal, size_t size)
{
    /* So in the latest version of stream parsing the problem is this:
     we recognize end of frame n only on first slice header of frame n + 1.
     And also we recognize start of very first frame on very first slice.
     But SEI and similar NALs are coming before first slice of the frame they
     refer to. So when first NAL appears and it is SEI, we don't have frame on
     queue yet. And when - after building a frame SEI that applies to the
     subsequent frame shows up, we'd add it to the current frame.

     This is not a huge problem - we could as well have a "cache" for special
     elements like SEIs or reserved NALs, and add those to the cache, and put
     that cache in front of frame every time begin of frame is detected. But
     we can also throw these away */
    (void)nal;
    (void)size;
}

/* H264 standard: on the SEI, AUD, filler, ends of sequence, and end of stream:
 "No normative decoding process is specified for NAL units with nal_unit_type
 equal to 6, 9, 10, 11, and 12". Now, AUD and filler got ignored above. SEI
 we know to contain extensions that our decoder _may_ interpret even if there
 is no "normative decoding process". We add flushing flag so we know we need
 to get buffered frames from reordered sequence.
*/
void H264StreamParser::handle_end_of_stream_nal()
{
    if (m_frames.empty()) {
        codec_log_warn(m_logger, "End of stream NAL received but there is no "
                                 "sequence to terminate");
    } else {
        m_frames.back().m_needs_flushing = true;
    }
}

void H264StreamParser::handle_end_of_sequence_nal()
{
    if (m_frames.empty()) {
        codec_log_warn(m_logger, "End of sequence NAL received but there is no "
                                 "sequence to terminate");
    } else {
        m_frames.back().m_needs_flushing = true;
    }
}

/* On the reserved range:
 "Decoders shall ignore (remove from the bitstream and discard) the contents of
 all NAL units that use reserved values of nal_unit_type.
 NOTE – This requirement allows future definition of compatible extensions to
 this Recommendation | International Standard."

 BUT this is first versions of H264 standard, which was then changed (f.e. they
 added Fidelity Range Extensions that AirPlay uses and HW decoder handles)
 */
void H264StreamParser::handle_reserved_nal(const unsigned char *nal, size_t size)
{
    /* See comment in handle_sei_nal() */
    (void)nal;
    (void)size;
}

/* H264 standard on the unspecified range:
 "NAL units that use nal_unit_type equal to 0 or in the range of 24..31,
 inclusive, shall not affect the decoding process specified in this
 Recommendation | International Standard.
 NOTE – NAL unit types 0 and 24..31 may be used as determined by the application.
 No decoding process for these values of nal_unit_type is specified in this
 Recommendation | International Standard."

 HW decoder ignores these (I checked by sending additional NALs from x264 module
 to the old decoder module and it worked fine, so we don't bother to feed them.

 If we were one day to add extensions to the stream, it would be good idea to
 assign them some NAL type unspecified in standard, and take them from here to
 their proper destination
 */
void H264StreamParser::handle_unspecified_nal(const unsigned char *nal, size_t size)
{
    /* Discard */
    (void)size;
    (void)nal;
}

/* This function returnes parser success/failure status */
bool H264StreamParser::parse_slice_header(const unsigned char *slice_nal, size_t size,
                                          SliceHeaderInfo &slice_header_info)
{
    ::memset(&slice_header_info, 0, sizeof(slice_header_info));
    if (!at_h264_get_initial_slice_header_info(slice_nal, size, slice_header_info)) {
        codec_log_error(m_logger, "Initial slice header parsing failed!");
        return false;
    }

    int pps_id = slice_header_info.pic_parameter_set_id;
    const NALParameterSet<PpsNalInfo> &pps = m_picture_parameter_sets[pps_id];
    if (!pps.get_size()) {
        codec_log_error(m_logger, "Slice header wants to activate unknown PPS");
        return false;
    }

    /* SPS itself was checked when PPS referred here by slice header was loaded,
     so no need for extra checks */
    int sps_id = pps.get_referred_index();

    /* Now we can do full slice header parsing */
    if (!at_h264_get_full_slice_header_info(slice_nal, size,
                                            m_sequence_parameter_sets[sps_id].get_info(),
                                            m_picture_parameter_sets[pps_id].get_info(),
                                            slice_header_info)) {
        codec_log_error(m_logger, "Full slice header parsing failed!");
        return false;
    }
    return true;
}
}
