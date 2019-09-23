/*
 * Copyright (c) 2018  AIRTAME ApS
 * All Rights Reserved.
 *
 * See LICENSE.txt for further information.
 */

#pragma once
#include <functional>

#include <stddef.h>
#include <stdint.h>

#include "timestamp.hpp"

namespace airtame {

class VideoBuffer {
public:
    Timestamp timestamp = 0;
    const unsigned char *data = nullptr;
    size_t size = 0;
    using FreeCallback = std::function<void(void)>;
    FreeCallback free_callback = 0;
};

class FrameGeometry {
public:
    /* Macroblock size (16) aligned dimensions */
    size_t m_padded_width, m_padded_height;
    /* True dimensions of image <= than padded dimensions */
    size_t m_true_width, m_true_height;
    /* Crop offset */
    size_t m_crop_left, m_crop_top;
    /* Ctors */
    FrameGeometry()
        : m_padded_width(0)
        , m_padded_height(0)
        , m_true_width(0)
        , m_true_height(0)
        , m_crop_left(0)
        , m_crop_top(0)
    {

    }
    // TODO: several types of ctors are not best way of writing self-explanatory
    // code. Maybe leave "zero all" and "set all" ctors and make "init from
    // true values" a method?
    /* Ctor for making frame geometry out of true dimensions (just pad them) */
    FrameGeometry(size_t true_width, size_t true_height)
        : m_true_width(true_width)
        , m_true_height(true_height)
        , m_crop_left(0)
        , m_crop_top(0)
    {
        m_padded_width = true_width & ~0xf;
        m_padded_height = true_height & ~0xf;
        if (m_padded_width < true_width) {
            m_padded_width += 16;
        }
        if (m_padded_height < true_height) {
            m_padded_height += 16;
        }
    }
    /* Full init ctor, when all values are set, for example after H264 SPS was
     parsed */
    FrameGeometry(size_t padded_width, size_t padded_height, size_t true_width,
                  size_t true_height, size_t crop_left, size_t crop_top)
        : m_padded_width(padded_width)
        , m_padded_height(padded_height)
        , m_true_width(true_width)
        , m_true_height(true_height)
        , m_crop_left(crop_left)
        , m_crop_top(crop_top)
    {
    }

    bool operator != (const FrameGeometry &other)
    {
        if (m_padded_width != other.m_padded_width) {
            return true;
        }
        if (m_padded_height != other.m_padded_height) {
            return true;
        }
        if (m_true_width != other.m_true_width) {
            return true;
        }
        if (m_true_height != other.m_true_height) {
            return true;
        }
        if (m_crop_left != other.m_crop_left) {
            return true;
        }
        if (m_crop_top != other.m_crop_top) {
            return true;
        }
        return false;
    }
};

class DecodingStats {
public:
    /* Summed up time of all decoding operations (msec) */
    Timestamp total_decoding_time = 0;
    /* Number of decode operations performed. Note that it not necessarily
     means number of frames decoded, as especially with H264 there can be
     0..n frames per decode operation */
    uint64_t number_of_decode_operations = 0;
    /* Longest decode operation (msec) */
    Timestamp max_decode_duration = 0;
    /* Biggest DMA allocation size */
    size_t max_dma_allocation_size = 0;

    void update_decode_timing(Timestamp last_duration)
    {
        ++number_of_decode_operations;
        total_decoding_time += last_duration;
        if (max_decode_duration < last_duration) {
            max_decode_duration = last_duration;
        }
    }

    void update_dma_allocation_size(size_t current_size)
    {
        if (max_dma_allocation_size < current_size) {
            max_dma_allocation_size = current_size;
        }
    }
};
}
