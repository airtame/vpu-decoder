#include "vpu_bitstream_buffer_monitoring.hpp"

namespace airtame {
/* So this is my best effort of assigning proper timestamps back to decoded
 frames. Problem is, decoder eats stuff (H264 NALs or VP8 frames or...) that
 have assigned timestamps only in the pipeline (not in the decoder bitstream
 input buffer), and spits out frames, which can be (and often are) made of many
 previously written units (for example H264 frame can be made out of several
 slices/partitions). And for H264 it is hard to say without serious stream
 parsing which particular NAL even starts a frame. And there are dropped frames
 and buffered frames, and frame reordering...

 So we do this: each chunk of data copied into decoder input circular buffer is
 also added to m_chunks FIFO, with begin/end indices of that chunk in decoder
 bitstream buffer. And when we get notified about decoded (not displayed! just
 decoded, it may still sit inside decoder for some time if it is refence frame
 "in future") frame index, we scan s_nals queue popping off all chunks that
 (judging from hw decode read_idx) were consumed during decoding of that very
 frame. Of course we only take timestamps from these chunks that have them
 (H264 SPS and PPS as well as slice partition NALs do not have timestamps
 assigned cause they for sure cannot start a frame).

 This, at least in theory lets us assign timestamps taken from very these chunks
 that make up frame just decoded. BTW this is one of the things original
 libimxvpuapi code wasn't doing, in effect assuming input buffer length zero
 just after decoding the frame...which might not be true */

void VPUBitstreamBufferMonitoring::clear()
{
    m_chunks.clear();
}

void VPUBitstreamBufferMonitoring::push_chunk(size_t begin, size_t end, Timestamp timestamp,
                                           bool is_frame)
{
    m_chunks.push_back(Chunk());
    m_chunks.back().begin = begin;
    m_chunks.back().end = end;
    m_chunks.back().timestamp = timestamp;
    m_chunks.back().is_frame = is_frame;
}

// TODO: redo code so that it won't assume taking frames off
Timestamp VPUBitstreamBufferMonitoring::update_queue(CodecLogger &logger, size_t new_read_idx)
{
    if (m_last_read_idx_set && (m_last_read_idx == new_read_idx)) {
        codec_log_error(logger, "Decoder read index not moving!");
    } else {
        m_last_read_idx_set = true;
    }
    m_last_read_idx = new_read_idx;
    bool have_timestamp = false;
    Timestamp timestamp = 0;
    /* Assumptions:
     0) Chunk begin is first byte, and end is first byte after
     the nal finished, that is: (begin + size) % circular_buffer_length
     1) read_idx DOES END within some chunk put in the circular buffer, that is
     read_index will lie somewhere in (begin, end> range for certain chunk, but
     keep in mind this is circular buffer we're dealing with
     2) Chunks are consumed in order, so if we see chunk that doesn't contain
     new_read_idx in it, we assume it was consumed.
     3) new_read_idx will never move more than buffer_size - 1 bytes, which makes
     sense because we cannot put more data there anyway
     4) This function won't be called if some chunks weren't taken off the queue,
     that is it gets called only when H264VPU::WaitResult::DECODED_FRAME is
     returned from H264VPU::wait_for_decoding, so we know for sure we have to
     take off at least one */
    bool stop = false;
    while (!m_chunks.empty() && !stop) {
        /* Process chunk timestamp. IMPORTANT: we take only first one. This is
         because frame might be composed of several chunks (like when H264
         frame has more than one slice NAL) and/or frame could have been
         dropped by decoder and we are moving over more than one chunk. */
        if (!have_timestamp && m_chunks.front().is_frame) {
            /* We take ts only out of slice NALs, because these make frame
             up and we want frame timestamp */
            timestamp = m_chunks.front().timestamp;
            have_timestamp = true;
        }
        /* Now be careful, read_ptr sometimes stops not just after nal (at the
         end), but some bytes short of it. I am guessing these NALs have fillers
         at the end or start_code reading state machine needs this sometimes or
         whatever. Anyway, we stop if within range, so it should work regardless.
         */
        stop = is_read_index_inside_chunk(m_chunks.front(), new_read_idx);
        /* Pop off NAL that we assume is consumed */
        m_chunks.pop_front();
    }
    if (!have_timestamp) {
        codec_log_warn(logger, "No timestamped NAL found for decoded frame");
    }
    return timestamp;
}

bool VPUBitstreamBufferMonitoring::is_read_index_inside_chunk(const Chunk &chunk, size_t read_idx)
{
    /* This is circular buffer we're dealing with and so chunk may be wrapped
     around */
    if (chunk.begin < chunk.end) {
        /* This is nice, not wrapped around chunk. So the "inside" condition
         for that chunk is just read_idx in (begin, end> range */
        if ((chunk.begin < read_idx) && (read_idx <= chunk.end)) {
            return true;
        }
    } else {
        /* Two ranges to check, actually */
        if ((chunk.begin < read_idx) /* Past begin */
            || (read_idx <= chunk.end)) { /* Before end */
            return true;
        }
    }
    return false;
}
}
