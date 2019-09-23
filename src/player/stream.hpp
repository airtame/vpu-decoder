/*
 * Copyright (c) 2019  AIRTAME ApS
 * All Rights Reserved.
 *
 * See LICENSE.txt for further information.
 */

#pragma once

namespace airtame {

/* File mapping to process space. Because I am too lazy to deal with actual
 codec elements overlapping the I/O buffers :P */
class Stream {
private:
    int m_fd = -1;
    const unsigned char *m_buffer = nullptr;
    const unsigned char *m_read_pointer = nullptr;
    size_t m_total_size = 0;
    size_t m_size_left = 0;

public:
    Stream()
    {
    }

    /* This simply takes over ownership of stream that was owned by source
     (if any). Very cheap */
    Stream(Stream &source)
        : m_fd(source.m_fd)
        , m_buffer(source.m_buffer)
        , m_read_pointer(source.m_read_pointer)
        , m_total_size(source.m_total_size)
        , m_size_left(source.m_size_left)
    {
        /* Now we can't have two instances pointing out to the same open file
         mapping, because first dtor call fucks up the other as well, so make
         original instance point to nothing */
        source.m_fd = -1;
        source.m_buffer = source.m_read_pointer = nullptr;
        source.m_total_size = source.m_size_left = 0;
    }
    ~Stream();
    bool open(const char *path);
    void flush_bytes(size_t n)
    {
        if (n < m_size_left) {
            m_size_left -= n;
            m_read_pointer += n;
        } else {
            m_size_left = 0;
            m_read_pointer = nullptr;
        }
    }
    const unsigned char *get_read_pointer() const
    {
        return m_read_pointer;
    }
    size_t get_size_left() const
    {
        return m_size_left;
    }
};
} // namespace airtame
