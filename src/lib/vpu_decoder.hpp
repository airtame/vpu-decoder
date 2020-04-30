#pragma once

#include "codec_common.hpp"
#include "codec_logger.hpp"
#include "vpu_decoder_buffers.hpp"
#include "vpu_frame_buffers.hpp"
#include "vpu_decoding_session.hpp"
#include "pack_queue.hpp"

namespace airtame {

/* The purpose of this class is to use low level VPUDecodingSession to implement
 fully featured decoder. Hight level session/frame pack management is here */
class VPUDecoder {
private:
    CodecLogger &m_logger;
    size_t m_display_frames;

    VPUDecoderBuffers m_buffers;
    VPUFrameBuffers m_frames;

    DecodingStats m_stats;
    std::shared_ptr<VPUDecodingSession> m_session;

    size_t m_frames_given = 0;
public:
    VPUDecoder(CodecLogger &logger, size_t display_frames)
        : m_logger(logger)
        , m_display_frames(display_frames)
        , m_frames(logger)
    {
    }

    /* It is safe to call this anyway, but for the step to actually do something
     this->has_frame_for_decoding() and pack_queue.has_packs_for_consumption()
     must both return true. So it should be called like this:
     while (decoder.has_frame_for_decoding() && queue.has_packs_for_consumption()) {
        VPUOutputFrame frame = decoder.step(queue);
        if (frame.has_data()) {
            // do something with the frame, then return it back once it is free
        }
     }

     Now, basically there are two possible outcomes from calling this function:
     decoded frame is given for display or not (duh!). Also this function
     guarantees that if has_frame_for_decoding() and has_packs_for_consumption()
     both are true, SOMETHING is bound to happen, that is:
     - Either frame is given for display
     - One frame pack is taken off the queue
     - (both can happen as well)

     More precisely, reasons for not giving the frame for display can be:
     - No frame pack suitable for decoding in the queue (for example no complete
     one, or, when decoder is closed, no frame pack allowing for reopening of
     the decoder, and so on)
     - No frame available for decoding (it is not safe to call VPU decoder
     without free frame - we learned that the hard way)
     - (REORDERED streams only) - frame was fed, decoded and buffered inside
     of decoder - nothing gets given for display

     Now, when the frame is given out for display, what happens internally may
     be either (streams without reordering, such as VP8 or h264 with reordering
     enabled):
     - Frame pack was taken off the queue, fed, decoded and given out for
     display

     OR (when reordering is enabled)
     - Frame pack was taken off the queue, fed, decoded and given out for
     display (this happens with B-frames for example)
     - Frame pack was taken off the queue, fed, decoded and buffered, OTHER
     frame (previously buffered one) is given out for display
     - Frame pack wasn't taken off the queue (because it carries flush flag),
     it was fed, decoded and frame is given out for display
     - Frame pack with flush flag wasn't fed and decoded (because it already
     was) but decoder is being called to return all previously buffered and
     not given for display frames.
     */
    VPUOutputFrame step(PackQueue &queue);

    /* This function is like above, except it will also try to decode frame
     packs not marked as complete. You should AVOID USING it if you can,
     because it isn't efficient usage of VPU decoder. So:
     - When decoding something that isn't encoded in real time (such as YT
     video, stream saved to file, and so on) one should use step() exclusively
     - When decoding live stream, one should supplant h264 parser m_is_complete
     information by information derived from protocol itself. For example, in
     Miracast one can force frames to be single slice only. Custom casting
     protocols could have "last NAL" flag carried as a metadata and so on. Then
     also step() can be safely used
     - If nothing is known about the stream and it comes in real time
     try_to_step() can be used, but it will have huge performance penalty in
     case of multislice streams.

     Also, just like for step() decoder.has_frame_for_decoding() must be true,
     but unlike step queue.has_frame_for_consumption() is enough, frame doesn't
     have to be complete */
    VPUOutputFrame try_to_step(PackQueue &queue);

    /* If that function returns true, decode is possible. If it doesn't, then
     frame must be returned first */
    bool has_frame_for_decoding() const;

    /* Frame with this physical_address finished displaying and can be reused
     by the decoder */
    void return_output_frame(long physical_address);

    /* Some uses expect decoder to have flush function. Problem with VPU decoder
     is that sometimes it needs to get display frames back even if flushing. And
     of course this is not what the flushing is expected to do, so this function
     is more like "flush up to one frame".

     WARNING: just like step()/try_to_step() it requires has_frame_for_decoding()
     to be true to actually do any useful work. So flushing only ends when
     has_frame_for_decoding() is true and flush_step returns empty frame */
    VPUOutputFrame flush_step();

    /* This should be called to finish current decoding, for example just before
     rewind operation. Right now flush is implemented internally, as a part of
     decode(), so no need for it */
    void close();

    bool is_closed() const
    {
        return !m_session.get();
    }

    const DecodingStats &get_stats() const
    {
        return m_stats;
    }

    size_t get_number_of_frames_given() const
    {
        return m_frames_given;
    }

private:
    VPUOutputFrame step_implementation(PackQueue &queue, PackPurpose purpose);
    bool check_for_reopening(const Pack &pack) const;
    bool feed_and_decode(PackQueue &queue, VPUOutputFrame &output,
                         bool allow_for_incomplete_data);
    bool feed_frame(PackQueue &queue);
};
}
