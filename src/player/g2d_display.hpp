/*
 * Copyright (c) 2019  AIRTAME ApS
 * All Rights Reserved.
 *
 * See LICENSE.txt for further information.
 */

#pragma once

#include <stddef.h>
#include <g2d.h>
#include <linux/fb.h>

namespace airtame {
class G2DDisplay {
private:
    const char *m_framebuffer_path;
    int m_framebuffer_fd = -1;
public:
    G2DDisplay(const char *framebuffer_path)
        : m_framebuffer_path(framebuffer_path)
    {
    }
    ~G2DDisplay();
    bool prepare_render(g2d_surface &destination);
    bool swap_buffers();
    size_t get_number_of_buffers();
private:
    bool get_vinfo(fb_var_screeninfo &vinfo);
    static size_t get_next_offset(const fb_var_screeninfo &vinfo);
    static bool framebuffer_format_to_g2d(const fb_var_screeninfo &vinfo,
                                          g2d_format &format);
    static size_t align_to_64(size_t value);
};
}
