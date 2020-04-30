#include <chrono>

#include <vpu_lib.h>

#include "pack_queue.hpp"
#include "vpu_decoder.hpp"
#include "vpu_decoding_session.hpp"

namespace airtame {

VPUOutputFrame VPUDecoder::step(PackQueue &queue)
{
    return step_implementation(queue, PackPurpose::CONSUMPTION);
}

VPUOutputFrame VPUDecoder::try_to_step(PackQueue &queue)
{
    return step_implementation(queue, PackPurpose::FEEDING);
}

bool VPUDecoder::has_frame_for_decoding() const
{
    if (m_session) {
        /* Check if current session has frames for decoding */
        return m_session->has_frame_for_decoding();
    } else {
        /* We will have to create new session and it will have free frames */
        return true;
    }
}

void VPUDecoder::return_output_frame(long physical_address)
{
    if (m_session) {
        m_session->return_output_frame(physical_address);
    }
}

VPUOutputFrame VPUDecoder::flush_step()
{
    VPUOutputFrame frame;
    if (!m_session) {
        /* Nothing to do */
        return frame;
    }

    if (!m_session->feed_end_of_stream()) {
        close();
        return frame;
    }

    if (!has_frame_for_decoding()) {
        /* Can't tell whether we can get a frame or not, because it is not
         safe to call decoder without frame for decoding, so assume we still
         need to call */
        return frame;
    }

    std::shared_ptr<FrameMetaData> fake_meta; /* This is never used */
    VPUDecodeStatus status = m_session->decode_video(fake_meta, frame);
    if (VPUDecodeStatus::ERROR & status) {
        /* Some kind of error, end flushing */
        close();
        return frame;
    }

    /* Flushing should never cause decode */
    assert(!(VPUDecodeStatus::OUTPUT_DECODED & status));

    /* We end up when nothing more is given for display */
    if (VPUDecodeStatus::NOTHING == status) {
        /* OK, no more frames given, flushing complete */
        close();
        return frame;
    }

    /* Should have received frame here */
    assert(VPUDecodeStatus::FRAME_GIVEN_FOR_DISPLAY & status);

    /* Retry, because we got frame */
    return frame;
}

void VPUDecoder::close()
{
    m_session.reset();
}

// TODO: this function assumes video decoding now. But it would actually
// make sense to add JPEG frame packs, and so this function could:
// 1) Efficiently decode a series of JPEGs, reusing same bitstream buffer and
// frames (provided geometry stays the same)
// 2) Efficiently decode MJPEG streams
VPUOutputFrame VPUDecoder::step_implementation(PackQueue &queue, PackPurpose purpose)
{
    VPUOutputFrame output;

    /* See if there is anything we can do for this type */
    if (!queue.has_pack_for(purpose)) {
        return output; /* No input data, nothing can be done */
    }

    /* We need to have at least one free frame to do anything useful */
    if (!has_frame_for_decoding()) {
        return output; /* No output buffer, can't call decoder */
    }

    /* Sometimes crucial parameters (like resolution) change from frame to frame
     and then session has to be closed and then open again */
    if (check_for_reopening(queue.front())) {
        /* Gotta reopen the session */
        m_session.reset();
    }

    /* Make sure we have decoding session */
    // TODO: session can probably be opened with any data (not metadata)
    // so incomplete frame packs can (and should) open decoding. VP8 frames
    // need to be complete, but they always come complete or not at all
    if (!m_session) {
        /* This is super cheap, so we do a loop, so the user won't have to */
        while (queue.has_pack_for(PackPurpose::CONSUMPTION)
               && !queue.front().m_can_reopen_decoding) {
            /* Not all video frames can reopen decoding. For example, in h264
             only IDR frames can, in VP8 only keyframes, and so on... */
            queue.pop_front();
        }

        if (!queue.has_pack_for(purpose)) {
            /* Nothing remained after popping off packs that would allow for
             reopening */
            return output; /* No more input, nothing can be done */
        }

        /* Try to open new session */
        m_session.reset(VPUDecodingSession::open_for_video(
            m_logger, m_stats, m_buffers, m_frames, queue.front().m_codec_type,
            queue.front().m_geometry, queue.front().m_maximum_number_of_reference_frames,
            m_display_frames, queue.front().m_needs_reordering
        ));

        if (!m_session) {
            /* Opening failed for some reason, get rid of this frame to move
             forward. Note that here we DO return control to the user, because
             opening is costly - opening, subsequent allocation of DMA frames
             and a first decode can easily take 100ms or more */
            queue.pop_front();
            return output;
        }
    }

    /* This is important - when we are decoding complete frames (this function
     is called with PackPurpose::CONSUMPTION we don't expect incomplete frame
     data at all */
    bool allow_for_incomplete_data = PackPurpose::FEEDING == purpose;

    /* OK, so this is "steady state" operation. When session is opened and is
     compatible with incoming frame pack, and we have free frame for decoding */
    if (!feed_and_decode(queue, output, allow_for_incomplete_data)) {
        /* If we are here, then error occured. Cannot proceed with current decoding
         session, so need to close and reopen in the future. */
        m_session.reset();
    }

    if (output.has_data()) {
        ++m_frames_given;
    }

    return output;
}

bool VPUDecoder::check_for_reopening(const Pack &pack) const
{
    /* No session, so we gotta open */
    if (!m_session) {
        return true;
    }

    /* IMPORTANT!
     This function is written so as to reopen only when strictly needed.
     Reopening is costly, as it can involve DMA memory handling, decoder reinit
     and so on. One important thing is that reopening seems to help with certain
     decode artifacts - for example, with VP8 stream when "garbled" images were
     created for some keyframes, reopening cleared those. Code looked like that:

     if ((CodecType::VP8 == pack.m_codec_type) && pack.m_can_reopen_decoding) {
         return true;
     }

     BUT later I found out that emitting IVF sequence headers just for keyframes
     that actually change resolution also helps, and it is much more efficient.
     So right now code is not here, but keep that in mind in case of more
     problems.
     */

    /* Compatibility check - see if stream parameters aren't changed on the
     frame. If they have, we need to close this stream and reopen */
    if (pack.m_codec_type != m_session->get_codec_type()) {
        codec_log_info(m_logger, "Codec type change, need to reopen");
        return true;
    }

    size_t required_frames = pack.m_maximum_number_of_reference_frames
        + m_display_frames;
    if (required_frames != m_session->get_number_of_frame_buffers()) {
        codec_log_info(m_logger, "Buffering requirement change, need to reopen");
        return true;
    }

    if (pack.m_needs_reordering != m_session->get_reordering()) {
        codec_log_info(m_logger, "Reordering requirement change, need to reopen");
        return true;
    }

    if (pack.m_geometry != m_session->get_frame_geometry()) {
        codec_log_info(m_logger, "Frame geometry change, need to reopen");
        return true;
    }

    return false;
}

bool VPUDecoder::feed_and_decode(PackQueue &queue, VPUOutputFrame &output,
                                 bool allow_for_incomplete_data)
{
    /* No sense to call this otherwise */
    assert(!queue.empty());
    assert(has_frame_for_decoding());

    /* Feed frame if it wasn't already fed (unfortunately we can't guarantee
     decode after every feed, because decoder may decide to return previously
     buffered frame back, and we might not have place for decode after that) */
    if (!queue.front().m_chunks.empty() && !feed_frame(queue)) {
        /* On error pop offending frame, otherwise we end up running into same
         issue again */
        queue.pop_front();
        return false;
    }

    /* Frame might be decoded already - frames with the flush flag set stay
     on queue even after decode */
    if (!queue.front().m_decoded) {
        /* OK, have complete frame fed in bitstream buffer, can decode */
        auto before = std::chrono::steady_clock::now();
        VPUDecodeStatus status = m_session->decode_video(queue.front().meta, output);
        auto duration = std::chrono::steady_clock::now() - before;

        if (VPUDecodeStatus::ERROR & status) {
            /* Decoding error, throw offending frame away or we will end up
             running into same problem again */
            queue.pop_front();
            return false;
        }
        size_t duration_msec =
            std::chrono::duration_cast<std::chrono::milliseconds>(duration).count();

        /* Let's see if decode was actually performed */
        if (VPUDecodeStatus::OUTPUT_DECODED & status) {
            /* Frame was decoded, can proceed */
            ++m_stats.number_of_decode_operations;
            m_stats.update_decode_timing(duration_msec);
            queue.mark_front_as_decoded();
            if (!queue.front().m_needs_flushing) {
                /* No longer need this frame */
                queue.pop_front();
                return true;
            }
        } else if (VPUDecodeStatus::FRAME_GIVEN_FOR_DISPLAY & status) {
            /* We got one of buffered frames instead, this is fine but do not
             pop frame yet, data remained in bitstream buffer */
            return true;
        } else if (VPUDecodeStatus::NOT_ENOUGH_INPUT_DATA & status) {
            ++m_stats.number_of_rolled_back_decodes;
            /* This is "legal" only if allow_for_incomplete_data is true */
            if (allow_for_incomplete_data) {
                return true;
            } else {
                codec_log_error(m_logger, "Got NOT_ENOUGH_DATA but complete "
                                          "frame was expected");
                return false;
            }
        } else {
            assert(VPUDecodeStatus::ERROR & status);
            /* This means decode error. Pop off offending frame, or we may end
             up going here again...and again and again */
            queue.pop_front();
            return false;
        }
    }

    /* If we got here, this frame needs flushing, which is special case at the
     end of sequence, we might get some frames that were buffered in the decoder
     (reference frames in the future). This really only matters when file/stream
     ends, because in normal playback decoder is smart enough to spit out all
     buffered frames on seeing next (for example in h264 - IDR frame) */
    assert(queue.front().m_needs_flushing);
    if (!has_frame_for_decoding()) {
        /* No use to flush, it won't work */
        return true;
    }

    output = flush_step();
    if (output.has_data()) {
        /* Got yet another output frame, have to give it back */
        return true;
    } else {
        /* Had input frame but produced no output, this is it */
        queue.pop_front();
        return true;
    }
}

bool VPUDecoder::feed_frame(PackQueue &queue)
{
    assert(!queue.empty());
    const Pack &pack = queue.front();
    size_t total_size = 0;
    for (auto &c : pack.m_chunks) {
        total_size += c.size;
    }

    size_t total_fed = 0;
    /* Note that we use this while loop instead of for syntax like above to
     avoid invalidating the iterators and crashing */
    while (!pack.m_chunks.empty()) {
        size_t size_fed;
//        codec_log_info(m_logger, "PUSHING %s (%zu)",
//                       pack.m_chunks.front().description.c_str(),
//                       pack.m_chunks.front().size);
        if (!m_session->feed(pack.m_chunks.front().data, pack.m_chunks.front().size, size_fed)) {
            return false;
        }
        if (size_fed != pack.m_chunks.front().size) {
            codec_log_error(m_logger, "End of bitstream space while feeding, "
                                      "%zu of total %zu fed",
                            total_fed + size_fed, total_size);
            return false;
        }
        total_fed += pack.m_chunks.front().size;
        queue.pop_chunk();
    }
//    codec_log_info(m_logger, "FED %zu bytes", total_fed);
    return true;
}
}
