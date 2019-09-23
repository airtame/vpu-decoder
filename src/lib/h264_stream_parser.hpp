/*
 * Copyright (c) 2018-2019  AIRTAME ApS
 * All Rights Reserved.
 *
 * See LICENSE.txt for further information.
 */

#pragma once

#include <string.h>

#include "codec_logger.hpp"

/* Important: we include vpu_h264_decoder.hpp, but use H264Decoder type - this
 is to allow for alternative decoder implementations to be included on other
 platforms in the future. Each of platform-specific include files should do
 something like that:
 typedef PlatformSpecificH264Decoder H264Decoder;
 so this file can just use H264Decoder everywhere */
#include "vpu_h264_decoder.hpp"

namespace airtame {

class H264StreamParser {
private:
    /* This is simple class we use for keeping and updating H264 parameter sets,
     (SPS and PPS) */
    class NALParameterSet {
    protected:
        unsigned char *m_data = nullptr;
        size_t m_size = 0;
        int m_referred_index = -1; /* PPSes have to "refer" to proper SPS, here
                                    we keep this index. Will be set to -1 for
                                    SPSes */
    public:
        ~NALParameterSet()
        {
            delete[] m_data;
        }

        void reset()
        {
            delete[] m_data;
            m_data = nullptr;
            m_size = 0;
            m_referred_index = -1;
        }

        /* Given new NAL with same index, see if it is really different and
         replace. Return bool indicating if something has changed */
        bool update(const unsigned char *nal, size_t s, int referred_index)
        {
            if ((m_size == s) && !::memcmp(m_data, nal, s)) {
                /* Not updated, NALs are equal */
                return false;
            }
            /* Otherwise update */
            if (m_data) {
                delete[] m_data;
            }
            m_data = new uint8_t[s];
            ::memcpy(m_data, nal, s);
            m_size = s;
            m_referred_index = referred_index;
            return true;
        }

        const unsigned char *get_data() const
        {
            return m_data;
        }

        size_t get_size() const
        {
            return m_size;
        }

        int get_referred_index() const
        {
            return m_referred_index;
        }
    };

    CodecLogger &m_logger;
    /* Decoder we'll feed with complete, separated NALs in proper order
     (that is, SPS/PPS exactly when they are needed, only once, one after
     another) */
    H264Decoder &m_decoder;
    /* If this flag is true, then NALs are passed to decoder. Otherwise, we
     are just skipping frames, looking for IDR picture slice on which we'll
     attempt resyncing */
    bool m_synchronized = false;
    /* SPS and PPS tables. H264 standard allows transmitting a number of these
     and activating them on per-slice basis, so proper handling on our side
     requires keeping them and sending to decoder when slice activates them */
    NALParameterSet m_sequence_parameter_sets[H264_NUMBER_OF_SPS_ALLOWED];
    NALParameterSet m_picture_parameter_sets[H264_NUMBER_OF_PPS_ALLOWED];
    /* Currently activated parameter sets, -1 means none */
    int m_activated_sps = -1;
    int m_activated_pps = -1;

    /* IDR picture numbering */
    int monotonic_idr_picture_id = 0;
    int previous_idr_pic_id = -1;
    /* Reference picture properties */

    /* Picture order count decoding */
    int prevPicOrderCntMsb = 0;
    int prevPicOrderCntLsb = 0;
    int topFieldOrderCount;
    int bottomFieldOrderCount;

public:
    H264StreamParser(CodecLogger &logger, H264Decoder &decoder)
        : m_logger(logger)
        , m_decoder(decoder)
    {
    }
    /* Call this with data buffer, which should contain whole NALs (so no
     division on buffer boundary, etc). Parser will handle filler or thrash at
     begin of the buffer (as long as start code is not emulated by random
     garbage). Number of bytes consumed is returned, and:
        - If it is zero, then call that function with same parameters again
        - If it is bigger, but smaller than length of the buffer, some data
        was consumed but not all, call this function again but passing
        updated data and length
        - If it equals length, then buffer can be discarted */
    size_t process_buffer(Timestamp timestamp, const unsigned char *data,
                          size_t length);
    bool reset();
private:
    /* NAL handlers. They all return boolean indicating whether given NAL
     was consumed. */
    bool handle_nal(Timestamp timestamp, const unsigned char *nal, size_t size);
    bool handle_sps_nal(const unsigned char *nal, size_t size);
    bool handle_pps_nal(const unsigned char *nal, size_t size);
    // TODO: probably can merge those in the future as slice header parsing now
    // answers all the questions that were previously answered by arguments
    bool handle_idr_slice_nal(Timestamp timestamp, const unsigned char *nal,
                              size_t size);
    bool handle_non_idr_slice_nal(Timestamp timestamp, const unsigned char *nal,
                                  size_t size);
    bool handle_bc_partition_nal(const unsigned char *nal, size_t size,
                                 NalType slice_type);
    bool handle_end_of_stream_nal(const unsigned char *nal, size_t size);
    bool handle_sei_nal(const unsigned char *nal, size_t size);
    bool handle_end_of_sequence_nal(const unsigned char *nal, size_t size);
    bool handle_reserved_nal(const unsigned char *nal, size_t size);
    bool handle_unspecified_nal(const unsigned char *nal, size_t size);
    /* Utilities */
    bool go_out_of_sync();
    bool parse_slice_header(const unsigned char *slice_nal, size_t size,
                            NalType &picture_type, int &slice_type, int &sps_id,
                            int &pps_id);
};

}
