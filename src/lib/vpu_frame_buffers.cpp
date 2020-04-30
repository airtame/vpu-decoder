#include <list>

#include "vpu_frame_buffers.hpp"
#include "vpu_decoding_session.hpp"

namespace airtame {
bool VPUFrameBuffers::reserve(size_t frame_buffer_size,
                              size_t number_of_reference_frame_buffers,
                              size_t number_of_display_frame_buffers,
                              FrameBuffer *(&decoder_buffers))
{
    /* This we do always */
    m_number_of_reference_frame_buffers = number_of_reference_frame_buffers;
    m_number_of_display_frame_buffers = number_of_display_frame_buffers;
    m_decoder_buffers.clear();

    /* See if we can re-use current memory */
    if (frame_buffer_size > m_frame_buffer_size) {
        codec_log_info(m_logger, "Requested frame size %.2fMB is > current "
                                 "%.2fMB, will have to remove and allocate again",
                       (double)frame_buffer_size / (1024 * 1024),
                       (double)m_frame_buffer_size / (1024 * 1024));
        /* Nothing we can do, just remove everything */
        m_frames.clear();
        /* And accept given size */
        m_frame_buffer_size = frame_buffer_size;
    } /* Else retain all the frames we have, and use them instead */

    /* This is how many buffers we want to have */
    size_t needed = m_number_of_reference_frame_buffers
        + number_of_display_frame_buffers;

    /* Let's try to recycle old frames */
    std::list<VPUDMAPointer> allocations;
    size_t available = 0;
    for (auto old : m_frames) {
        /* When old frame is not given for display, we can recycle it. If it
         was, we can't use it because it may still be in display process, and
         there is no way to tell the decoder "do not use this frame"...unless
         perhaps appending these at the end would make decoder use those at
         least?? But dangerous bet */
        if (!old.given_for_display) {
            ++available;
            allocations.push_back(old.dma);
        }
    }

    m_frames.clear();
    /* It is OK to, for example, recycle just 5 frame out of 7, or some such.
     This is because remaining 2 frames are in display, and we don't want to
     wait for it. We've been there, and it wasn't pretty. In fact, it was
     damn complicated, very error prone, and produced playback hiccups */
    codec_log_info(m_logger, "Managed to recycle %zu frames, have to allocate %zu",
                   available, needed - available);

    // TODO: measure time here?
    while (available < needed) {
        VPUDMAPointer dma = VPUDecodingSession::allocate_dma(m_frame_buffer_size);
        if (dma) {
            allocations.push_back(dma);
            ++available;
        } else {
            codec_log_error(m_logger,
                            "Failed to allocate all decoder buffers - "
                            "exhausted DMA memory?");
            return false;
        }
    }

    /* Finally, set up new vectors */
    m_decoder_buffers.reserve(available);
    m_frames.reserve(available);
    for (auto dma : allocations) {
        m_decoder_buffers.push_back(FrameBuffer());
        /* Only set base addresses here, caller will add up proper offsets,
         we don't know them yet */
        m_decoder_buffers.back().bufY = dma->phy_addr;
        m_decoder_buffers.back().bufCb = dma->phy_addr;
        m_decoder_buffers.back().bufCr = dma->phy_addr;
        m_decoder_buffers.back().bufMvCol = dma->phy_addr;
        m_frames.push_back(VPUFrameMemoryAndMetadata());
        m_frames.back().given_for_display = false;
        m_frames.back().clear_display_flag = false;
        m_frames.back().dma = dma;
    }
    decoder_buffers = m_decoder_buffers.data();
    return true;
}

void VPUFrameBuffers::mark_frame_as_returned(unsigned long physical_address)
{
    /* Now we have to identify which frame it was using the physical address */
    for (size_t idx = 0; idx < m_frames.size(); idx++) {
        if (m_frames[idx].dma->phy_addr == physical_address) {
            /* Frame found */
            m_frames[idx].given_for_display = false;
            m_frames[idx].clear_display_flag = true;
            /* DO NOT clear flag here - we may have decoder running, and then
             operation would be silently ignored, "leaking" frame in effect.
             This way allows us to experiment with asynchronous decoding */
        }
    }

    /* We might be given a frame that is unknown. This is because DMA buffers
     are now reference counted so the client might hold on to them even after
     decoding session is removed/recreated. In some cases, result may be that
     new instance of decoding session gets frame that belonged to previous
     session.

     So do nothing if an invalid frame is given. */
}

bool VPUFrameBuffers::return_frames_now(DecHandle decoder)
{
    for (size_t idx = 0; idx < m_frames.size(); idx++) {
        if (m_frames[idx].clear_display_flag) {
            if (RETCODE_SUCCESS != vpu_DecClrDispFlag(decoder, idx)) {
                /* Not much we can do here, and it means that decoder will run out
                 of memory soon, so... */
                codec_log_fatal(m_logger, "Could not return displayed frame back to decoder");
                return false;
            }
            m_frames[idx].clear_display_flag = false;
        }
    }
    return true;
}

bool VPUFrameBuffers::has_frame_for_decoding() const
{
    /* IMPORTANT: I used to think that when already decoded frame (like
     reference frame which was "in future" when decoded) is given away, then no
     decode occured and so no frame was taken and so number of free frames
     should stay the same. THIS IS NOT TRUE - I guess that when decoder gives
     away for display what used to be a reference frame it immediately clams one
     of free frames into its place. And so proper way of checking "if there is
     a frame for decoding" is basically checking if we have given away less
     frames than display reserve */
    size_t number_of_frame_buffers_given_out_for_display = 0;
    for (auto f : m_frames) {
        if (f.given_for_display) {
            ++number_of_frame_buffers_given_out_for_display;
        }
    }
    return number_of_frame_buffers_given_out_for_display
        < m_number_of_display_frame_buffers;
}

void VPUFrameBuffers::frame_decoded(size_t index,
                                    const std::shared_ptr<FrameMetaData> &meta)
{
    /* Have decoded frame, assign metadata. Note that decoding frame is not
     the same as giving it away for display - we may be operating codec
     that uses reordering and so buffers frames inside. We just assign
     metadata to the frame and increase number of buffered decoded frames */
    assert(index < m_frames.size());
    assert(!m_frames[index].meta);
    assert(!m_frames[index].given_for_display);
    m_frames[index].meta = meta;
}

void VPUFrameBuffers::frame_to_be_given_for_display(size_t index,
                                                    VPUDMAPointer &dma_return,
                                                    std::shared_ptr<FrameMetaData> &meta_return)
{
    assert(index < m_frames.size());
    assert(m_frames[index].meta);
    /* Give back DMA memory and frame metadata */
    dma_return = m_frames[index].dma;
    meta_return = m_frames[index].meta;
    /* Mark frame as given out for display, clear metadata */
    m_frames[index].given_for_display = true;
    m_frames[index].meta.reset();
}
}
