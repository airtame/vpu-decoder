/*
 * Copyright (c) 2019  AIRTAME ApS
 * All Rights Reserved.
 *
 * See LICENSE.txt for further information.
 */

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include "stream.hpp"

namespace airtame {
Stream::~Stream()
{
    if (m_buffer) {
        ::munmap((void *)m_buffer, m_total_size);
    }
    if (-1 != m_fd) {
        ::close(m_fd);
    }
}

bool Stream::open(const char *path)
{
    /* Open file */
    m_fd = ::open(path, O_RDONLY);
    if (-1 == m_fd) {
        fprintf(stderr, "Cannot open %s: %s\n", path, strerror(errno));
        return false;
    }

    /* Get the size of the file */
    struct stat s;
    if (-1 == ::fstat(m_fd, &s)) {
        fprintf(stderr, "Cannot stat %s: %s\n", path, strerror(errno));
        return false;
    }
    m_total_size = m_size_left = s.st_size;

    /* mmap whole file */
    m_buffer = (const unsigned char *)::mmap(0, s.st_size, PROT_READ, MAP_PRIVATE, m_fd, 0);
    if (MAP_FAILED == m_buffer) {
        fprintf(stderr, "Cannot mmap view of %s: %s\n", path, strerror(errno));
        return false;
    }
    m_read_pointer = m_buffer;
    return true;
}
}
