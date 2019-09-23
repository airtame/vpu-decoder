/*
 * Copyright (c) 2018  AIRTAME ApS
 * All Rights Reserved.
 *
 * See LICENSE.txt for further information.
 */

#include <list>
#include <stddef.h>
#include <stdint.h>

#include "codec_logger.hpp"
#include "timestamp.hpp"

namespace airtame {
    class VPUBitstreamBufferMonitoring {
    private:
        /* To keep strict timestamping of frames we keep track of all data chunks
         (H264 NALs, VP8 frames, whatever) fed into bitstream input queue of
         the decoder, so we know which bytes were consumed for any given output
         frame */
        struct Chunk {
            size_t begin, end;
            Timestamp timestamp;
            bool is_frame;
        };
        typedef std::list<Chunk> ChunkList;
        ChunkList m_chunks;
        bool m_last_read_idx_set = false;
        size_t m_last_read_idx;
    public:
        void clear();
        void push_chunk(size_t begin, size_t end, Timestamp timestamp, bool is_frame);
        Timestamp update_queue(CodecLogger &logger, size_t new_read_idx);
    private:
        bool is_read_index_inside_chunk(const Chunk &chunk, size_t read_idx);
    };
}
