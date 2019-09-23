/*
 * Copyright (c) 2013-2019  AIRTAME ApS
 * All Rights Reserved.
 *
 * See LICENSE.txt for further information.
 */

#pragma once
#include <cstdlib>
#include <cstdint>

class H264Bitstream {
private:
    const unsigned char *m_data{ nullptr };
    size_t m_data_size{ 0 };
    size_t m_bits_available{ 0 };
    uint32_t m_bits{ 0 };

public:
    struct Result {
        Result()
        {
        }
        Result(int32_t val, bool err)
        {
            value = val;
            error = err;
        }

        int32_t value{ 0 };
        bool error{ true };
    };

    H264Bitstream(const unsigned char *data, size_t data_size)
    {
        reset(data, data_size);
    }

    void reset(const unsigned char *data, size_t data_size)
    {
        m_data = data;
        m_data_size = data_size;
        m_bits_available = 0;
        m_bits = 0;
    }

    /* Functions below used to have ignore_* counterparts. But I don't think
     we can just ignore anything - it can have end of stream error in it. So
     just use read* from now on, because there is no point in having ignore
     functions that return same value as read functions, just ignore the
     values in code */

    /* Reads an unsigned integer of up to 24 bits from the bitstream */
    Result read_un_bits(size_t n);

    /* Reads an unsigned integer of variable length (Exp-Golomb coded).
     In H264 stream that will be limited to 24 bits */
    Result read_uev_bits();

    /* Same, but signed Exp-Golomb coded */
    Result read_sev_bits();
};
