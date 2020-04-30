/*
 * Copyright (c) 2018-2019  AIRTAME ApS
 * All Rights Reserved.
 *
 * See LICENSE.txt for further information.
 */

#pragma once

#include <string.h>
extern "C" {
#include <vpu_io.h>
#include <vpu_lib.h>
}

namespace airtame {

/* Values here "inherited" from libimxvpuapi, and tuned down a bit */
/* WARNING: bitstream buffer size should be 4-byte aligned and a multiple of 1024.
 Not sure of really good value here, but we can probably go totally on the safe
 side, and use one full decoded frame size. No use of video compression making
 things bigger, after all. */
#define VPU_DEC_H264_MAIN_BITSTREAM_BUFFER_SIZE (1920 * 1088 * 3 / 2)

/* VPU documentation 3.2.3.2 seems to recommend to use half a size of one
 YUV frame. In our case 1920*1088*1.5/2 so exactly like integer computation below */
#define VPU_MAX_SLICE_BUFFER_SIZE (1920 * 1088 * 15 / 20)

/* MA: libimxvpuapi used half a MB here and this probably just lazy, because
 there max 32 SPS and 256 PPS allowed, and typically they are very small.
 Plus, we save SPS/PPS in H264Parser anyway, and feed them JIT. So we'll
 probably hardly see more than 1SPS and perhaps a few PPSes per video sequence.
 And finally, I have yet to see H264 stream that uses more than one SPS at a
 time (for example AirPlay seems to replace previous SPS rather than send both).
 WARNING: address should be 4-aligned and size a multiple of 1024 */
#define VPU_PS_SAVE_BUFFER_SIZE (1024 * 128)

#define VPU_DEC_VP8_MB_PRED_BUFFER_SIZE (68 * (1920 * 1088 / 256))

// TODO: cleanup "growing" of bitstream buffer, maybe add panicking in the
// decoding session instead

class VPUDecoderBuffers {
private:
    /* Size of bitstream buffer to create during next allocation */
    size_t m_wanted_bitstream_buffer_size;

    /* Bitstream buffer, commont to all decoders (H264, VP8, ...) */
    vpu_mem_desc m_bitstream_buffer;

    /* Additional buffers H264 decoding needs */
    vpu_mem_desc m_ps_save_buffer, m_slice_buffer;

    /* Same, but for the VP8 decoding */
    vpu_mem_desc m_mb_prediction_buffer;

public:
    VPUDecoderBuffers()
        : m_wanted_bitstream_buffer_size(VPU_DEC_H264_MAIN_BITSTREAM_BUFFER_SIZE)
    {
        /* Zero all the buffer structures */
        ::memset(&m_bitstream_buffer, 0, sizeof(m_bitstream_buffer));
        ::memset(&m_ps_save_buffer, 0, sizeof(m_ps_save_buffer));
        ::memset(&m_slice_buffer, 0, sizeof(m_slice_buffer));
        ::memset(&m_mb_prediction_buffer, 0, sizeof(m_mb_prediction_buffer));
    }

    ~VPUDecoderBuffers()
    {
        get_rid_of_buffer(m_bitstream_buffer);
        get_rid_of_buffer(m_slice_buffer);
        get_rid_of_buffer(m_ps_save_buffer);
        get_rid_of_buffer(m_mb_prediction_buffer);
    }

    // TODO: make sure that it is safe to call init_for_h264 and then init_for_vp8
    // (release old buffers first)

    bool init_for_h264()
    {
        /* Make sure we release any VP8 buffers */
        get_rid_of_buffer(m_mb_prediction_buffer);
        /* Make sure m_wanted_bitstream_buffer_size is padded - it cannot be
         just any size */
        m_wanted_bitstream_buffer_size = pad_buffer_size(m_wanted_bitstream_buffer_size);
        /* Bitstream we need to allocate and map, because process needs to
         feed video data there. */
        get_and_map_buffer(m_bitstream_buffer, m_wanted_bitstream_buffer_size);

        /* Remaining buffers just have to be allocated, VPU will use them,
         CPU won't */
        m_ps_save_buffer.size = VPU_PS_SAVE_BUFFER_SIZE;
        if (RETCODE_FAILURE == IOGetPhyMem(&m_ps_save_buffer)) {
            /* No ps save buffer, no h264 decoding */
            return false;
        }

        m_slice_buffer.size = VPU_MAX_SLICE_BUFFER_SIZE;
        if (RETCODE_FAILURE == IOGetPhyMem(&m_slice_buffer)) {
            /* Again, cannot decode without ps save buffer */
            return false;
        }
        return true;
    }

    bool init_for_vp8()
    {
        /* Make sure we release any h264 buffers */
        get_rid_of_buffer(m_ps_save_buffer);
        get_rid_of_buffer(m_slice_buffer);
        /* Make sure m_wanted_bitstream_buffer_size is padded - it cannot be
         just any size */
        m_wanted_bitstream_buffer_size = pad_buffer_size(m_wanted_bitstream_buffer_size);
        /* Bitstream we need to allocate and map, because process needs to
         feed video data there. */
        get_and_map_buffer(m_bitstream_buffer, m_wanted_bitstream_buffer_size);

        /* Remaining buffers just have to be allocated, VPU will use them,
         CPU won't */
        m_mb_prediction_buffer.size = VPU_DEC_VP8_MB_PRED_BUFFER_SIZE;
        if (RETCODE_FAILURE == IOGetPhyMem(&m_mb_prediction_buffer)) {
            return false;
        }
        return true;
    }

    void update_wanted_bitstream_buffer_size(size_t chunk_size)
    {
        /* Traditional "allocate twice as much" approach */
        size_t desired_size = 2 * chunk_size;
        if ((chunk_size > (size_t)m_bitstream_buffer.size)
            && (m_wanted_bitstream_buffer_size < desired_size)) {
            m_wanted_bitstream_buffer_size = desired_size;
        }
    }

    bool should_grow_bitstream_buffer()
    {
        return m_wanted_bitstream_buffer_size != (size_t)m_bitstream_buffer.size;
    }

    bool grow_bitstream_buffer()
    {
        /* Make sure bitstream buffer size is padded */
        m_wanted_bitstream_buffer_size = pad_buffer_size(m_wanted_bitstream_buffer_size);
        if (get_rid_of_buffer(m_bitstream_buffer) &&
            get_and_map_buffer(m_bitstream_buffer, m_wanted_bitstream_buffer_size)) {
            return true;
        } else {
            return false;
        }
    }

    /* Getters */
    const vpu_mem_desc &get_bitstream_buffer() const
    {
        return m_bitstream_buffer;
    }

    const vpu_mem_desc &get_ps_save_buffer() const
    {
        return m_ps_save_buffer;
    }

    const vpu_mem_desc &get_slice_buffer() const
    {
        return m_slice_buffer;
    }

    const vpu_mem_desc &get_mb_prediction_buffer() const
    {
        return m_mb_prediction_buffer;
    }

private:
    bool get_and_map_buffer(vpu_mem_desc &buffer, size_t size)
    {
        buffer.size = size;
        if (RETCODE_FAILURE == IOGetPhyMem(&buffer)) {
            /* No bitstream buffer, no decoding */
            return false;
        }
        if (RETCODE_FAILURE == IOGetVirtMem(&buffer)) {
            /* Can't map bitstream, so can't send data to decoder */
            return false;
        }
        return true;
    }
    bool get_rid_of_buffer(vpu_mem_desc &buffer)
    {
        /* Unmap old buffer */
        if (buffer.virt_uaddr) {
            if (RETCODE_FAILURE == IOFreeVirtMem(&buffer)) {
                /* Just in case... */
                IOFreePhyMem(&buffer);
                return false;
            }
        }
        /* Free old buffer */
        if (buffer.phy_addr) {
            if (RETCODE_FAILURE == IOFreePhyMem(&buffer)) {
                return false;
            }
        }
        return true;
    }
    /* So I couldn't any authoritative source as what bitstream buffer size
     quant should be, but it works with 4K and it happens to be page size
     which is what mmap() can give us anyway... */
    size_t pad_buffer_size(size_t size)
    {
        size &= ~4095;
        return (size + 4096);

    }
};
}
