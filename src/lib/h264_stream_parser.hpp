/*
 * Copyright (c) 2018-2020 AIRTAME ApS
 * All Rights Reserved.
 *
 * See LICENSE.txt for further information.
 */

#pragma once

#include <string.h>

#include "codec_logger.hpp"
#include "h264_nal.hpp"
#include "pack_queue.hpp"

namespace airtame {

class H264StreamParser {
private:
    /* This is simple class we use for keeping and updating H264 parameter sets,
     (SPS and PPS) */
    template <typename InfoType>
    class NALParameterSet {
    protected:
        unsigned char *m_data = nullptr;
        size_t m_size = 0;
        int m_referred_index = -1; /* PPSes have to "refer" to proper SPS, here
                                    we keep this index. Will be set to -1 for
                                    SPSes */
        InfoType m_info;
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
        bool update(const unsigned char *nal, size_t s, int referred_index,
                    const InfoType &info)
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
            m_info = info;
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

        const InfoType &get_info() const
        {
            return m_info;
        }
    };

    CodecLogger &m_logger;

    /* Frame list */
    PackQueue &m_frames;

    /* Force disable reordering flag */
    bool m_force_disable_reordering;

    /* SPS and PPS tables. H264 standard allows transmitting a number of these
     and activating them on per-slice basis, so proper handling on our side
     requires keeping them and sending to decoder when slice activates them */
    NALParameterSet<SpsNalInfo> m_sequence_parameter_sets[H264_NUMBER_OF_SPS_ALLOWED];
    NALParameterSet<PpsNalInfo> m_picture_parameter_sets[H264_NUMBER_OF_PPS_ALLOWED];

    /* Last seen slice header info */
    SliceHeaderInfo m_current_picture_slice_header;

public:
    H264StreamParser(CodecLogger &logger, PackQueue &frames,
                     bool force_disable_reordering)
        : m_logger(logger)
        , m_frames(frames)
        , m_force_disable_reordering(force_disable_reordering)
    {
    }

    /* Call this with data buffer, which should contain whole NALs (so no
     division on buffer boundary, etc). Parser will handle filler or thrash at
     begin of the buffer (as long as start code is not emulated by random
     garbage). Status is returned, if true then NAL(s) were found and added to
     frame queue */
    void process_buffer(const VideoBuffer &buffer);

    void set_force_disable_reordering(bool force_disable_reordering)
    {
        m_force_disable_reordering = force_disable_reordering;
    }

private:
    void handle_nal(const std::shared_ptr<FrameMetaData> &meta, const unsigned char *nal, size_t size);
    void handle_sps_nal(const unsigned char *nal, size_t size);
    void handle_pps_nal(const unsigned char *nal, size_t size);
    void handle_slice_nal(const std::shared_ptr<FrameMetaData> &meta, const unsigned char *nal,
                          size_t size, NalType slice_type);
    void handle_bc_partition_nal(const unsigned char *nal, size_t size,
                                 NalType slice_type);
    void handle_end_of_stream_nal();
    void handle_sei_nal(const unsigned char *nal, size_t size);
    void handle_end_of_sequence_nal();
    void handle_reserved_nal(const unsigned char *nal, size_t size);
    void handle_unspecified_nal(const unsigned char *nal, size_t size);
    /* Utilities */
    void push_chunk(const unsigned char *nal, size_t size, const char *description);
    /* Return false on error */
    bool parse_slice_header(const unsigned char *slice_nal, size_t size,
                            SliceHeaderInfo &slice_header_info);
};
}
