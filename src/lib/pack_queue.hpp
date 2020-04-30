#pragma once

#include "codec_common.hpp"

#include <list>
#include <string>
#include <vpu_lib.h>

namespace airtame {

enum class PackPurpose {
    CONSUMPTION,
    FEEDING
};

/* This class is a bit like VideoBuffer from codec_common.hpp, and it even
 carries compatible free function. Idea is that the user feeds several video
 buffers into our decoder, not knowing (or _having_ to know) what is inside.
 Stream parsers break down said VideoBuffers into stream chunks and these
 chunks are attached to the frames, but necessary information is carried over
 the decoder to the output */
class VideoChunk {
public:
    const unsigned char *data = nullptr;
    size_t size = 0;
    VideoBuffer::FreeCallback free_callback = 0;
    std::string description;

    VideoChunk()
        : data(nullptr)
        , size(0)
        , free_callback(0)
    {
    }
    VideoChunk(const VideoChunk &c) = delete;
    VideoChunk(VideoChunk &&c) = default;
    ~VideoChunk()
    {
        /* Check for free callback, if it exists we should call it */
        if (free_callback) {
            free_callback();
        }
    }

    VideoChunk& operator=(const VideoChunk &) = delete;
    VideoChunk& operator=(VideoChunk &&) = default;
};

/* This class contains everything that is needed to decode single frame:
- bitstream chunks
- frame geometry
- user defined metadata
- decoding directives */

class Pack {
public:
    /* Bitstream chunks part */
    std::list<VideoChunk> m_chunks;

    /* Geometry part */
    FrameGeometry m_geometry;
    size_t m_maximum_number_of_reference_frames = 0;

    /* Metadata part */
    std::shared_ptr<FrameMetaData> meta;

    /* Decoding flags */
    CodecType m_codec_type;

    /* Assigned by stream parser */
    bool m_can_reopen_decoding = false;
    bool m_can_be_dropped = false; /* This can be also interpreted as "is a
                                    reference frame", because only non-reference
                                    frames can be dropped */
    bool m_is_complete = false; /* Frame now contains all the data, i.e. is not
                                 in the middle of reception/parsing */
    // h264-only flags, maybe add h264 to the names?
    bool m_needs_reordering = false; /* This can be decoded on a per-stream
                                      basis if you know (for example) h264
                                      profiles (and stream parser does) */
    bool m_needs_flushing = false;

    /* Written to by the decoder */
    bool m_decoded = false;
};

/* This class serves to restrict pack queue operations to few sane ones. In
 future we will probably want to have stuff like pluggable dropping policies
 here, and I guess then it will be possible to extend it with stuff like
 adding decode times on pop, or keeping info about how fast/slow decoding is.
 But right now it is pretty simple...

 "BEFORE DECODE" DROPPING ALGORITHM EXAMPLE:
 Let's suppose we have some frames in queue, and some of them are "too old"
 already, and we want to start dropping to catch up. This is how it could work:
 - First, drop all the frames that have m_can_be_dropped flag set. This is
 gentle and easy part. Note that some codecs (h264 non-reference B frames) will
 have it (for example about half of frames from YT videos have this flag set),
 others (vp8) not at all
 - Second, if we are still lagging behind, we can pick a frame with
 m_can_reopen_decoding and discard everything before it. This will create a
 "jump" in the playback. If there are several frames with m_can_reopen_decoding
 set, any one can be picked. All codecs will have those every now and then
 (for example h264 IDR frame, vp8 keyframe, ...) but just how often depends on
 the encoder. YT streams seem to have IDR frame every few seconds.

 "FASTER PLAYBACK" ALGORITHM:
 - Smooth possible by dropping all frames with m_can_be_dropped, so possible
 with h264, not possible with vp8
 - Otherwise gotta "jump" to m_can_reopen_decoding when those appear.

 "SEEK" ALGORITHM:
 - Imprecise seek can be done by jumping to just about any frame (if without
 stream resync feature, like vp8) or even byte offset (if has stream resync,
 like h264 or MPEGs), then getting to nearest m_can_reopen_decoding will be
 done automatically by decoder
 - Precise seek demands getting to certain location and finding nearest
 m_can_reopen_decoding frame _before_ it, then playing back (perhaps with both
 pre-decode and post-decode dropping) to precise frame in stream that has
 timestamp nearest to the one requested
 */

class PackQueue {
private:
    std::list<Pack> m_packs;
    size_t m_number_of_packs_popped = 0;
public:
    bool has_pack_for_consumption() const
    {
        if (m_packs.empty()) {
            return false;
        }
        /* Return true only if we have complete frame to feed and decode */
        return m_packs.front().m_is_complete;
    }

    bool has_pack_for_feeding() const
    {
        if (m_packs.empty()) {
            return false;
        }
        /* Return when there is something that wasn't fed yet, regardless
         of completness */
        return !m_packs.front().m_chunks.empty();
    }

    bool has_pack_for(PackPurpose purpose) const
    {
        if (PackPurpose::CONSUMPTION == purpose) {
            return has_pack_for_consumption();
        } else { // PackPurpose::FEEDING == type
            return has_pack_for_feeding();
        }
    }

    /* To be called by pack producer */
    void push_new_pack()
    {
        /* Automatically terminate previous pack, if any */
        if (!m_packs.empty()) {
            m_packs.back().m_is_complete = true;
        }
        m_packs.push_back(Pack());
    }

    /* This is called with every chunk pushed. Note that buffer may contain
     one or more chunks */
    void push_chunk(const unsigned char *data, size_t size, const char *description)
    {
        if (m_packs.empty()) {
            /* This may happen in "try to step" decoding mode, when first few
             chunks of a frame were fed into decoder and then caused error,
             or if decoder is skipping over frames that have
             m_can_reopen_decoding set to false */
            return;
        }
        m_packs.back().m_chunks.push_back(VideoChunk());
        VideoChunk &chunk = m_packs.back().m_chunks.back();
        chunk.data = data;
        chunk.size = size;
        chunk.description = description;
    }

    /* This is called when all the chunks from the buffer were pushed and buffer
     "free" information needs to be passed to last chunk, so that the buffer
     could be freed when last chunk is consumed.

     Note: this is specifically NOT passed to the push_chunk above, because
     parser may decide that some particular chunk is just a filler, and throw
     it away, in effect leaking the buffer memory. */
    void attach_free_callback(VideoBuffer::FreeCallback callback)
    {
        if ((m_packs.empty() || m_packs.back().m_chunks.empty())
            || (0 != m_packs.back().m_chunks.back().free_callback)) {
            /* This is not an error - it just means that stream parser has
             thrown away all the chunks from this buffer, so we can fire
             up free callback immediately */
            if (callback) {
                callback();
            }
        } else {
            /* Assign that free callback to the last chunk of the last
             video pack */
            m_packs.back().m_chunks.back().free_callback = callback;
        }
    }

    Pack &back()
    {
        assert(!m_packs.empty());
        return m_packs.back();
    }
    bool empty() const
    {
        return m_packs.empty();
    }

    /* To be called by pack consumer */
    const Pack &front() const
    {
        assert(!m_packs.empty());
        return m_packs.front();
    }

    void pop_chunk()
    {
        assert(!m_packs.empty());
        m_packs.front().m_chunks.pop_front();
    }

    void mark_front_as_decoded()
    {
        assert(!m_packs.empty());
        m_packs.front().m_decoded = true;
    }

    void pop_front()
    {
        assert(!m_packs.empty());
        m_packs.pop_front();
        ++m_number_of_packs_popped;
    }

    size_t get_number_of_packs_popped() const
    {
        return m_number_of_packs_popped;
    }
};
}
