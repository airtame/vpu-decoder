/*
 * Copyright (c) 2018-2019  AIRTAME ApS
 * All Rights Reserved.
 *
 * See LICENSE.txt for further information.
 */

#include <assert.h>
#include "h264_bitstream.hpp"
#include "h264_stream_parser.hpp"

/* H264Parser handles proper NAL-feeding for the decoder. This is because for
 H264 you can't really have "dumb" feeding of H264 data into decoder. At least,
 that HW decoder we are using.

 This is because in H264 streams NALs compose "video sequences". Video sequence
 consists of picture slices, of which first should be IDR picture slice. IDR
 stands for "Instant Decoder Refresh", meaning that at this point decoder state
 can be properly (re)initialized using just this one picture.

 But the problem is that IDR (and all other picture slices in H264) reference
 something called parameter sets. There are two kinds of these, sequence
 parameter set (SPS) and picture parameter set (PPS). They are kind of "context"
 messages that carry parameters common to whole video sequence (SPS) or one or
 more pictures (PPS). Now H264 standard allows those to be send anywhere in the
 stream, or indeed even out-of-band, provided that the proper SPS/PPS will be
 fed into decoder just before frame that refers to it.

 So for example stream continues, decoder sends out frames and between other
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

/* Big problem with NAL parsing is this:
 - We know for sure that hw decoder works better if given separate, whole NALs
 one at a time (for example QSV stream gets decoded to single pixel (0,0) color
 all over the screen if NALs are not separated)
 - We also want as little delay as possible. Especially, we do not want to wait
 for the next input frame with feeding the data from the current one. But if we
 allowed for NALs spanning multiple frames we couldn't be sure whether current
 NAL ends with end of the frame, or extends in next one. So we would have to
 wait for the next frame, therefore inducing one (pipeline) frame delay.
 - Not to mention that handling fragmented NALs in decoder introduces great deal
 of complexity to already tricky code

 We control streaming done by ourselves, so in our stream multiple NALs may
 arrive "glued together" (like from QSV encoder), but newer "cut to pieces".
 Looks like current AirPlay/Tatvik also does this.

 So solution is this:
 - Assume that frames start with start code, and contain whole NALs (so NAL
 terminates on start code of next NAL or end of buffer)
 - Monitor if that assumption is true, and complain otherwise
 - For applications that don't naturally provide data as described above (like
 reading from file in imx_playback_video, or for example new version of AirPlay),
 introduce something BEFORE input of decoder module, that will make sure NALs
 are not in multiple buffers.

 This way, we can have most efficient and low-latency (also relatively simple)
 code in HW module for our own streaming, and complexity can be added where/if
 necessary.

 IMPORTANT:

 This parser does error handling on two levels. First thing is checking for
 start codes in process_buffer(). Second thing is concept of being "synchronized"
 with the stream (or not). This comes from H264 (and earlier MPEG standards)
 terminology of start codes "allowing for synchronization with the stream".

 Basic idea is that decoder can start reading incoming bit stream at any given
 place (OK, here we are in ordinary computer system and not serial link and so
 assume that bitstream is byte-aligned, but nothing more than that). Now, there
 are only one place (and that is IDR slice) where decoding can start. So parser
 starts with m_synchronized flag set to false. And in this mode:
 - PPS/SPS NALs are kept for future reference
 - On every IDR NAL we synchronize with the stream
 - Every other NAL is immediately thrown away.

 Of course, when m_synchronized is set, all NALs are fed into the decoder save
 from app-specific NALS. In this way, incoming stream can be insane (for example
 just B-frames) and we won't even try to feed that to decoder, we just consume
 NALs right away without doing nothing, in effect fast-forwarding the stream,
 looking for IDR image slice on which we can try to resynchronize.

 Procedure described above is good enough to allow for starting bitstream
 decoding at random offset - parser will simply keep skipping NALs until it
 accumulates enough SPS/PPSes and encounter IDR NAL it can decode.

 Note that this has nothing to do with decoder error handling - from decoder
 POV stream is valid if IDR NAL appears and both it, and all subsequent image
 slice NALs refer to some valid and known SPS/PPS pairs. Stream can still
 make no sense to decoder, which has its own error handling procedures */

/* Uncomment this to log messages about NALs being fed to the decoder */
//#define NAL_LOGGING

size_t H264StreamParser::process_buffer(Timestamp timestamp, const unsigned char *data,
                                        size_t length)
{
    const unsigned char *limit = data + length;
    const unsigned char *current_nal = at_h264_next_start_code(data, limit);
    if (!current_nal) {
        /* No start code found, WTF is this? */
        codec_log_warn(m_logger,
                       "H264 parser given entire buffer without NAL "
                       "start code in it");
        return go_out_of_sync() ? length : 0;
    }

    size_t bytes_consumed = current_nal - data;
    if ((bytes_consumed > 1) || ((bytes_consumed == 1) && (data[0]))) {
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
        if (handle_nal(timestamp, current_nal, current_nal_size)) {
            /* Decoder consumed this NAL, can move on */
            bytes_consumed += current_nal_size;
            current_nal = next_nal;
        } else {
            /* Decoder can't take this NAL right now, stop feeding */
            break;
        }
    }

    return bytes_consumed;
}

bool H264StreamParser::reset()
{
    if (m_decoder.reset()) {
        m_synchronized = false;
        m_activated_pps = m_activated_sps = -1;
        for (size_t i = 0; i < H264_NUMBER_OF_SPS_ALLOWED; i++) {
            m_sequence_parameter_sets[i].reset();
        }
        for (size_t i = 0; i < H264_NUMBER_OF_PPS_ALLOWED; i++) {
            m_picture_parameter_sets[i].reset();
        }
        return true;
    } else {
        return false;
    }
}

bool H264StreamParser::handle_nal(Timestamp timestamp, const unsigned char *nal, size_t size)
{
    /* Fourth byte of NAL (last byte of start code), five low bits are
     nal_unit_type */
    NalType type = NalType(nal[3] & 0x1f);
    /* Now route NAL to apropriate handler. NAL handlers return boolean
     that indicates whether NAL was consumed or not. If it wasn't consumed
     it will be re-fed in the future */
    switch (type) {
        case NalType::UNSPECIFIED_ZERO: {
            return handle_unspecified_nal(nal, size);
        }

        /* Not subdivided slice */
        case NalType::IDR_SLICE: {
            return handle_idr_slice_nal(timestamp, nal, size);
        }

        /* Not subdivided slice */
        case NalType::NON_IDR_SLICE: {
            return handle_non_idr_slice_nal(timestamp, nal, size);
        }

        /* This whole Partition A/B/C bussines is so encoder could subdivide
         slices into up to three NALs. */
        case NalType::PARTITION_A_SLICE: {
            /* Partition A is just a first part of subdivided slice, but just
             like ordinary slice it starts with slice_header, so we can use
             same routine */
            return handle_non_idr_slice_nal(timestamp, nal, size);
        }

        case NalType::PARTITION_B_SLICE: {
            return handle_bc_partition_nal(nal, size, NalType::PARTITION_B_SLICE);
        }

        case NalType::PARTITION_C_SLICE: {
            return handle_bc_partition_nal(nal, size, NalType::PARTITION_C_SLICE);
        }

        case NalType::SUPPLEMENTAL_ENHANCED_INFORMATION: {
            return handle_sei_nal(nal, size);
        }

        case NalType::SEQUENCE_PARAMETER_SET: {
            return handle_sps_nal(nal, size);
        }

        case NalType::PICTURE_PARAMETER_SET: {
            return handle_pps_nal(nal, size);
        }

        case NalType::ACCESS_UNIT_DELIMITER: {
/* Just throw it away, they are not needed for decoding, and for example streams
 generated by Microsoft H264 software encoders tend to have a lot of these */
#ifdef NAL_LOGGING
            codec_log_debug(m_logger, "Access Unit Delimiter NAL ignored");
#endif
            return true;
        }

        case NalType::END_OF_SEQUENCE: {
            return handle_end_of_sequence_nal(nal, size);
        }

        case NalType::END_OF_STREAM: {
            return handle_end_of_stream_nal(nal, size);
        }

        case NalType::FILLER: {
            /* Just throw it away, this is just to pad network packets to
             constant size or some such */
#ifdef NAL_LOGGING
            codec_log_debug(m_logger, "Filler NAL ignored");
#endif
            return true;
        }

        default: {
            if ((NalType::FIRST_RESERVED_TYPE <= type) && (type <= NalType::LAST_RESERVED_TYPE)) {
                return handle_reserved_nal(nal, size);
            } else if ((NalType::FIRST_UNSPECIFIED_TYPE <= type)
                       && (type <= NalType::LAST_UNSPECIFIED_TYPE)) {
                return handle_unspecified_nal(nal, size);
            } else {
                /* This should never happen, but to avoid infinite loop in
                 production environment, like "Twitch problem" of March 2019 */
                codec_log_error(m_logger, "Unexpected NAL type %d, eating it up "
                                          "to avoid infinite loop", type);
                assert(0); /* We want to notice in debug environments */
                return true;
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
bool H264StreamParser::handle_sps_nal(const unsigned char *nal, size_t size)
{
    // TODO: move to parsing SPS here, and giving cached parsed version to the
    // decoders
    /* We could just parse whole SPS here, but it doesn't make much sense
     because most of the time it would be same o', so just read id out of
     it (it is in fourth byte of payload), and then compare with existing
     SPS with that id */
    H264Bitstream bs(nal + 7, size - 7);
    H264Bitstream::Result bits;
    bits = bs.read_uev_bits();
    if (bits.error) {
        codec_log_error(m_logger,
                        "Bitstream parse error while reading sps_id out "
                        "of SPS");
        return go_out_of_sync();
    }

    int sps_id = bits.value;
    if ((sps_id < 0) || (sps_id >= H264_NUMBER_OF_SPS_ALLOWED)) {
        codec_log_error(m_logger, "SPS parse error: sps_id out of range");
        return go_out_of_sync();
    }

    /* OK, update SPS at sps_id */
    if (m_sequence_parameter_sets[sps_id].update(nal, size, -1)) {
        /* Check currently active SPS */
        if ((-1 != m_activated_sps) && (m_activated_sps == sps_id)) {
            /* Will have to reactivate SPS, as currently active one
                got replaced with different content. Technically this should
                only happend on video sequence boundary, but it doesn't hurt
                to handle it this way */
            m_activated_sps = m_activated_pps = -1;
#ifdef NAL_LOGGING
            codec_log_debug(m_logger,
                            "New SPS with id %d received, need "
                            " reactivation of SPS/PPS pair",
                            sps_id);
#endif
        } else { /* Just keep that SPS, nothing changes */
#ifdef NAL_LOGGING
            codec_log_debug(m_logger,
                            "New SPS with id %d received and kept for "
                            "future use",
                            sps_id);
#endif
        }
    }
    return true;
}

/* See comments about SPS above and also, H264 standard: 7.4.2.2 on PPS:
 "pic_parameter_set_id identifies the picture parameter set that is referred to
 in the slice header. The value of pic_parameter_set_id shall be in the range of
 0 to 255, inclusive.
 seq_parameter_set_id refers to the active sequence parameter set. The value of
 seq_parameter_set_id shall be in the range of 0 to 31, inclusive.

 IMPORTANT: same as with SPS, we save PPS regardless of sync status */

// TODO: create proper PPS parsing code analogous to SPS one
bool H264StreamParser::handle_pps_nal(const unsigned char *nal, size_t size)
{
    PpsNalInfo pps;
    if (!at_h264_get_pps_info(nal, size, pps)) {
        codec_log_error(m_logger, "Failed to parse PPS");
        return go_out_of_sync();
    }

    uint32_t sps_id = pps.seq_parameter_set_id;
    uint32_t pps_id = pps.pic_parameter_set_id;

    if (!m_sequence_parameter_sets[sps_id].get_size()) {
        codec_log_error(m_logger, "PPS parse error: sps_id refers to unknown SPS");
        return go_out_of_sync();
    }

    /* OK, update PPS at pps_id */
    if (m_picture_parameter_sets[pps_id].update(nal, size, sps_id)) {
        /* PPS changed, see if it was active one */
        if (m_activated_pps == pps_id) {
            /* Active PPS changed, next reference will require re-activation */
            if (sps_id != m_activated_sps) { /* That PPS causes also activation of
                                                different SPS */
#ifdef NAL_LOGGING
                codec_log_debug(m_logger,
                                "New PPS with id %d and sps_id %d received "
                                "This replaces currently active SPS/PPS "
                                "and will cause reactivation of SPS/PPS",
                                pps_id, sps_id);
#endif
                m_activated_sps = -1;
            } else { /* Just PPS changes */
#ifdef NAL_LOGGING
                codec_log_debug(m_logger,
                                "New PPS with id %d and sps_id %d received. "
                                "This replaces currently active PPS and will "
                                "need to be reactivated in its place",
                                pps_id, sps_id);
#endif
            }
            m_activated_pps = -1;
        } else {
#ifdef NAL_LOGGING
            codec_log_debug(m_logger,
                            "New PPS with id %d and sps_id %d received "
                            "and kept for future use",
                            pps_id, sps_id);
#endif
        }
    }
    return true;
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

 Also, this frame is the only NAL which allows us to sync with stream, as, by
 definition "After the decoding of an IDR picture all following coded pictures
 in decoding order can be decoded without inter prediction from any picture
 decoded prior to the IDR picture. The first picture of each coded video
 sequence is an IDR picture" */
bool H264StreamParser::handle_idr_slice_nal(Timestamp timestamp, const unsigned char *nal,
                                            size_t size)
{
    NalType picture_type;
    int slice_type, sps_id, pps_id;
    if (!parse_slice_header(nal, size, picture_type, slice_type, sps_id, pps_id)) {
        return go_out_of_sync();
    }

    /* Validity of SPS gets checked when reading PPS, so doesn't have to check
     now, and particular PPS needed is checked in parse_slice_header */
    const NALParameterSet &sps = m_sequence_parameter_sets[sps_id];
    const NALParameterSet &pps = m_picture_parameter_sets[pps_id];

    /* Now feed all three NALs to the decoder */
    // TODO: this only on FIRST SLICE OF IDR PICTURE, NOT ALL OF THEM!
    if (m_decoder.feed_picture_slice_with_parameter_sets(
            timestamp, sps.get_data(), sps.get_size(), pps.get_data(), pps.get_size(), nal, size,
            true, m_activated_sps != sps_id, m_activated_pps != pps_id)) {
        /* SPS/PPS/IDR picture slice consumed, sequence opened succesfully */
        m_activated_sps = sps_id;
        m_activated_pps = pps_id;
#ifdef NAL_LOGGING
        codec_log_debug(m_logger, "IDR picture slice_type %s size %zu ts " PRIi64 " fed into decoder",
                        at_h264_slice_type_description(slice_type), size, timestamp);
#endif
        if (!m_synchronized) {
            codec_log_info(m_logger, "Parser resynchronized with input stream");
            m_synchronized = true;
        }
        return true;
    } else {
        /* IDR picture slice not consumed yet, will have to re-feed */
        return false;
    }
}

/* Non-IDR images cannot change SPS and so won't require decoder reopening. They
 can change PPS (which contains things like quantizer params) though */
bool H264StreamParser::handle_non_idr_slice_nal(Timestamp timestamp, const unsigned char *nal,
                                                size_t size)
{
    NalType picture_type;
    int slice_type, sps_id, pps_id;

    /* This function will check PPS being referred for validity */
    if (!parse_slice_header(nal, size, picture_type, slice_type, sps_id, pps_id)) {
        return go_out_of_sync();
    }

    const char *picture_description
        = NalType::PARTITION_A_SLICE == picture_type ? "Partition A" : "Non-IDR";

    if (!m_synchronized) {
#ifdef NAL_LOGGING
        codec_log_debug(m_logger, "%s picture slice thrown away because out of sync",
                        picture_description);
#endif
        return true; /* Consume this NAL, we have to skip all the way to
                      IDR slice to resync */
    }

    /* Only IDR slices are allowed to change activated SPS */
    if (m_activated_sps != sps_id) {
        codec_log_error(m_logger,
                        "%s picture slice refers to SPS id %d but SPS %d "
                        "is activated!",
                        picture_description, sps_id, m_activated_sps);
        return go_out_of_sync();
    }

    const NALParameterSet &pps = m_picture_parameter_sets[pps_id];

    /* Now feed two NALs to the decoder */
    if (m_decoder.feed_picture_slice_with_parameter_sets(timestamp, nullptr, 0, pps.get_data(),
                                                         pps.get_size(), nal, size,
                                                         false, /* NOT IDR NAL! */
                                                         false, /* Don't activate SPS */
                                                         m_activated_pps != pps_id)) {
        /* PPS/Non-IDR picture slice consumed, sequence opened succesfully */
        m_activated_pps = pps_id;
#ifdef NAL_LOGGING
        codec_log_debug(m_logger, "%s picture slice_type %s size %zu ts " PRIi64 " fed into decoder",
                        picture_description, at_h264_slice_type_description(slice_type), size,
                        timestamp);
#endif
        return true;
    } else {
        /* Slice not consumed yet, will have to re-feed */
        return false;
    }
}

/* This is just partition slice B or C, which continue from previous partition A
 slice, which contained header and was already accepted by decoder, because we
 got here, so nothing special to do */
bool H264StreamParser::handle_bc_partition_nal(const unsigned char *nal, size_t size,
                                               NalType partition_type)
{
    if (!m_synchronized) {
#ifdef NAL_LOGGING
        codec_log_debug(m_logger, "Partition %s picture slice size %zu discarted",
                        (NalType::PARTITION_B_SLICE == partition_type) ? "B" : "C", size);
#endif
        return true; /* Consume this NAL */
    }
    if (m_decoder.feed_other(nal, size)) {
#ifdef NAL_LOGGING
        codec_log_debug(m_logger, "Partition %s picture slice size %zu fed into decoder",
                        (NalType::PARTITION_B_SLICE == partition_type) ? "B" : "C", size);
#endif
        return true;
    } else {
        /* Slice not consumed yet, will have to re-feed */
        return false;
    }
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
 (like some kind of markers). But in spirit of keeping compliance with old
 code (which fed everything), will feed SEIs, too.
 */
bool H264StreamParser::handle_sei_nal(const unsigned char *nal, size_t size)
{
    if (!m_synchronized) {
#ifdef NAL_LOGGING
        codec_log_debug(m_logger, "SEI NAL size %zu discarted", size);
#endif
        return true; /* Consume this NAL */
    }
    if (m_decoder.feed_other(nal, size)) {
#ifdef NAL_LOGGING
        codec_log_debug(m_logger, "SEI NAL size %zu fed into decoder", size);
#endif
        return true;
    } else {
        return false;
    }
}

/* H264 standard: on the SEI, AUD, filler, ends of sequence, and end of stream:
 "No normative decoding process is specified for NAL units with nal_unit_type
 equal to 6, 9, 10, 11, and 12". Now, AUD and filler got ignored above. SEI
 we know to contain extensions that our decoder _may_ interpret even if there
 is no "normative decoding process". Ends of sequence and ends of stream are/can
 be used by our own code, so we give NALs in these classes to the decoder
 without extra handling.
*/

bool H264StreamParser::handle_end_of_stream_nal(const unsigned char *nal, size_t size)
{
    if (!m_synchronized) {
#ifdef NAL_LOGGING
        codec_log_debug(m_logger, "End of stream NAL discarted");
#endif
        return true; /* Consume this NAL */
    }
    /* Technically, we could switch synchronized flag off here, but in fact we
     do carry SPS activation over separate sequences, which prevents us from
     reopening the decoder if not necessary, so we de-sync only on errors and
     keep synchronized on end of sequence. And BTW looks like nobody cares to
     send end of sequence apart from our own imx_playback_video and tests. */
    if (m_decoder.feed_end_of_stream(nal, size)) {
#ifdef NAL_LOGGING
        codec_log_debug(m_logger, "End of stream NAL fed into decoder");
#endif
        return true;
    } else {
        return false;
    }
}

bool H264StreamParser::handle_end_of_sequence_nal(const unsigned char *nal, size_t size)
{
    if (!m_synchronized) {
#ifdef NAL_LOGGING
        codec_log_debug(m_logger, "End of sequence NAL size %zu discarted", size);
#endif
        return true;
    }
    if (m_decoder.feed_other(nal, size)) {
#ifdef NAL_LOGGING
        codec_log_debug(m_logger, "End of sequence NAL size %zu fed into decoder", size);
#endif
        return true;
    } else {
        return false;
    }
}

/* On the reserved range:
 "Decoders shall ignore (remove from the bitstream and discard) the contents of
 all NAL units that use reserved values of nal_unit_type.
 NOTE – This requirement allows future definition of compatible extensions to
 this Recommendation | International Standard."

 BUT this is first versions of H264 standard, which was then changed (f.e. they
 added Fidelity Range Extensions that AirPlay uses and HW decoder handles) and
 so we are not sure how/if our HW decoder handles these, so it is better to leave
 this decision to it.
 */
bool H264StreamParser::handle_reserved_nal(const unsigned char *nal, size_t size)
{
    if (!m_synchronized) {
#ifdef NAL_LOGGING
        codec_log_debug(m_logger, "Reserved NAL (unit_type %u) size %zu discarted", nal[3] & 0x1f,
                      size);
#endif
        return true;
    }
    if (m_decoder.feed_other(nal, size)) {
#ifdef NAL_LOGGING
        codec_log_debug(m_logger, "Reserved NAL (unit_type %u) size %zu fed into decoder",
                        nal[3] & 0x1f, size);
#endif
        return true;
    } else {
        return false;
    }
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
bool H264StreamParser::handle_unspecified_nal(const unsigned char *nal, size_t size)
{
    /* Discard */
    (void)size;
    (void)nal;
#ifdef NAL_LOGGING
    codec_log_debug(m_logger, "Unspecified NAL (unit_type %u) size %zu discarted", nal[3] & 0x1f,
                    size);
#endif
    return true;
}

bool H264StreamParser::go_out_of_sync()
{
    if (m_synchronized) {
        /* Avoid repeating the message */
        codec_log_error(m_logger,
                        "H264 parser going out of sync till next IDR "
                        "image slice");
        m_synchronized = false;
    }
    return m_decoder.reset(); /* We may need to re-enter here many times */
}

/* WARNING: unlike previous functions this fuction does not return consumed
 flag but parser success/failure */
bool H264StreamParser::parse_slice_header(const unsigned char *slice_nal, size_t size,
                                          NalType &picture_type, int &slice_type,
                                          int &sps_id, int &pps_id)
{
    SliceHeaderInfo slice_header_info;
    if (!at_h264_get_initial_slice_header_info(slice_nal, size, slice_header_info)) {
        codec_log_error(m_logger, "Initial slice header parsing failed!");
        return false;
    }

    slice_type = slice_header_info.slice_type;

    pps_id = slice_header_info.pic_parameter_set_id;
    const NALParameterSet &pps = m_picture_parameter_sets[pps_id];
    if (!pps.get_size()) {
        codec_log_error(m_logger, "Slice header wants to activate unknown PPS");
        return false;
    }

    /* SPS itself was checked when PPS referred here by slice header was loaded,
     so no need for extra checks */
    sps_id = pps.get_referred_index();
    const NALParameterSet &sps = m_sequence_parameter_sets[sps_id];

    // TODO: these should be cached to avoid re-parsing on every slice */
    SpsNalInfo sps_info;
    if (!at_h264_get_sps_info(m_sequence_parameter_sets[sps_id].get_data(),
        m_sequence_parameter_sets[sps_id].get_size(), sps_info)) {
        codec_log_error(m_logger, "SPS parser failure");
        return false;
    }

    PpsNalInfo pps_info;
    if (!at_h264_get_pps_info(m_picture_parameter_sets[pps_id].get_data(),
        m_picture_parameter_sets[pps_id].get_size(), pps_info)) {
        codec_log_error(m_logger, "PPS parser failure");
        return false;
    }

    /* Now we can do full slice header parsing */
    if (!at_h264_get_full_slice_header_info(slice_nal, size, sps_info, pps_info,
                                            slice_header_info)) {
        codec_log_error(m_logger, "Full slice header parsing failed!");
        return false;
    }

    char buf[16];
    if (NalType::IDR_SLICE == slice_header_info.nal_unit_type) {
        codec_log_info(m_logger, "IDR %spicture slice_type=%s idr_pic_id=%u, size = %zu",
                       slice_header_info.ref_nal_idc ? "reference " : "",
                       at_h264_slice_type_description(slice_header_info.slice_type),
                       slice_header_info.idr_pic_id, size);
    } else {
        codec_log_info(m_logger, "%spicture slice type=%s size=%zu",
                       slice_header_info.ref_nal_idc ? "reference " : "",
                       at_h264_slice_type_description(slice_header_info.slice_type),
                       size);
    }

    // Questions we want answered:
    // 1) First slice of IDR picture. To check for it:
    // - If it is IDR picture slice, and previous NAL wasn't
    // - If it is IDR picture slice, and previous NAL was this as well but had
    // different idr_pic_id. So we need "previous NAL" info

    /* First thing we have to do is to make sure we maintain monotonic idr
     picture id. Stream doesn't have to supply one, as standard says
     (7.4.3 "Slice header semantics"):
     "idr_pic_id identifies an IDR picture. The values of idr_pic_id in all
     the slices of an IDR picture shall remain unchanged. When two consecutive
     access units in decoding order are both IDR access units, the value of
     idr_pic_id in the slices of the first such IDR access unit shall differ
     from the idr_pic_id in the second such IDR access unit. The value of
     idr_pic_id shall be in the range of 0 to 65535, inclusive" */
    // not sure here, we need previous reference picture/previous idr picture?
//    bool first_idr_slice = false;
//    if (idr_unit_type) {
//        if ((previous_nal != IDR_NAL) || (idr_pic_id != previous_idr_pic_id)) {
//            first_idr_slice = true;
//            ++monotonic_idr_picture_id;
//            previous_idr_pic_id = idr_pic_id;
//        }
//    }

    /* Picture order count decoding. There are three variants, depending on
     pic_order_cnt_type equal to 0, 1 or 2. And for Mode 0 we need "previous
     reference picture in decoding order" while for Mode 1/2 we need "previous
     picture in decoding order" */

//    if (0 == sps_info.pic_order_cnt_type) {
//        if (idr_unit_type) {
//            /* "If the current picture is an IDR picture, prevPicOrderCntMsb is
//             set equal to 0 and prevPicOrderCntLsb is set equal to 0. */
//            prevPicOrderCntMsb = prevPicOrderCntLsb = 0;
//        }
//    }

    return true;
}
}
