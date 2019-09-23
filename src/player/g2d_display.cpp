/*
 * Copyright (c) 2019  AIRTAME ApS
 * All Rights Reserved.
 *
 * See LICENSE.txt for further information.
 */

#include <errno.h>
#include <fcntl.h>
#include <linux/ipu.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include "g2d_display.hpp"

namespace airtame {
/* This is defined only on DG2 with its GC2000 GPU, so we use it to tell DG1
 and DG2 apart */
#ifdef IPU_PIX_FMT_GPU32_SB_SRT
#define DG2
#else
#define DG1
#endif

#ifdef DG1
/* Technically 2 is recommended, and it is enough when FPS doesn't get bigger
 than 40+. But if very fast g2d blitting is used (as here) and decoder is
 effective enough, one gets artifacts when FPS is 50 or more. 4 buffers cure
 the problem. Also see "i.MX Graphics User’s Guide", page 36, FB_MULTI_BUFFER
 environment variable description - seems like 4 is ideal */
#define NUMBER_OF_BUFFERS 4
#define ALIGN(v) (v)
#else
#define NUMBER_OF_BUFFERS 4
/* Again, technically this alignment would be necessary for "tiled" formats,
 which we don't use here, but for sake of safety */
#define ALIGN(v) align_to_64(v)
#endif

G2DDisplay::~G2DDisplay()
{
    if (-1 != m_framebuffer_fd) {
        close(m_framebuffer_fd);
    }
}

bool G2DDisplay::prepare_render(g2d_surface &destination)
{
    fb_var_screeninfo vinfo;
    if (!get_vinfo(vinfo)) {
        return false;
    }

    /* Compute desired parameters for framebuffer */
    size_t wanted_width = ALIGN(vinfo.xres);
    size_t wanted_height = ALIGN(vinfo.yres);

    size_t virtual_width = wanted_width;
    size_t virtual_height = wanted_height * NUMBER_OF_BUFFERS;

    if (vinfo.nonstd || (vinfo.xres_virtual != virtual_width)
        || (vinfo.yres_virtual != virtual_height)) {
        /* Not what we want. Likely first open or first call after resolution
         change. Force our wanted parameters */
        vinfo.xres_virtual = virtual_width;
        vinfo.yres_virtual = virtual_height;
        vinfo.yoffset = 0; /* Force this again to be at the begin */
        vinfo.nonstd = 0;

        /* Set new parameters */
        if (-1 == ioctl(m_framebuffer_fd, FBIOPUT_VSCREENINFO, &vinfo)) {
            fprintf(stderr, "Can't set variable framebuffer info: %s\n",
                    strerror(errno));
            return false;
        }

        /* Close and reopen framebuffer */
        if (-1 == close(m_framebuffer_fd)) {
            fprintf(stderr, "Cannot close framebuffer: %s\n",
                    strerror(errno));
            return false;
        }

        m_framebuffer_fd = -1;

        if (!get_vinfo(vinfo)) {
            return false;
        }

        /* Check if we got what we wanted */
        if (vinfo.nonstd || (vinfo.xres_virtual != virtual_width)
            || (vinfo.yres_virtual != virtual_height)) {
            fprintf(stderr, "Couldn't reset framebuffer to desired parameters\n");
            return false;
        }
    }

    /* OK, looks like we have proper framebuffer format, have to get fixed
     info */
    fb_fix_screeninfo finfo;
    if (-1 == ioctl(m_framebuffer_fd, FBIOGET_FSCREENINFO, &finfo)) {
        fprintf(stderr, "Can't get fixed framebuffer info: %s\n",
                strerror(errno));
        return false;
    }

    /* Fill in destination description */
    if (!framebuffer_format_to_g2d(vinfo, destination.format)) {
        fprintf(stderr, "Framebuffer format not supported by g2d\n");
        return false;
    }

    /* Plane 0 should contain next buffer address */
    size_t next_offset = get_next_offset(vinfo);
    destination.planes[0] = finfo.smem_start + (next_offset * finfo.line_length);

    /* Planes 1 and 2 are used only by blit sources, not destinations */
    destination.planes[1] = 0;
    destination.planes[2] = 0;

    /* Fill in full display rectangle */
    destination.left = 0;
    destination.top = 0;
    destination.right = vinfo.xres; // not sure here - maybe -1
    destination.bottom = vinfo.yres; // not sure here - maybe -1
    destination.width = vinfo.xres;
    destination.height = vinfo.yres;

    /* g2d wants stride in pixels, framebuffer one is in bytes! */
    destination.stride = finfo.line_length / (vinfo.bits_per_pixel / 8);

    /* Safe defaults for blending, clear and rotation */
    destination.blendfunc = G2D_ZERO;
    destination.global_alpha = 0;
    destination.clrcolor = 0;
    destination.rot = G2D_ROTATION_0;
    return true;
}

bool G2DDisplay::swap_buffers()
{
    fb_var_screeninfo vinfo;
    if (!get_vinfo(vinfo)) {
        return false;
    }

    vinfo.yoffset = get_next_offset(vinfo);
    /* Error here usually means "resolution change". We close framebuffer then
     and re-open it on next prepare_render() */
    if (-1 == ioctl(m_framebuffer_fd, FBIOPAN_DISPLAY, &vinfo)) {
        // TODO: maybe investigate which errno is set then, and return true
        // only on that errno, false otherwise?
        fprintf(stderr, "Could not pan display, likely a resolution change");
        /* Close framebuffer because for the resolution change to take effect
         we need to reopen it */
        close(m_framebuffer_fd);
        m_framebuffer_fd = -1;
        return true;
    }
    return true;
}

size_t G2DDisplay::get_number_of_buffers()
{
    return NUMBER_OF_BUFFERS;
}

bool G2DDisplay::get_vinfo(fb_var_screeninfo &vinfo)
{
    if (-1 == m_framebuffer_fd) {
        /* Either first call, or we closed after error */
        m_framebuffer_fd = open(m_framebuffer_path, O_RDWR, 0);
        if (-1 == m_framebuffer_fd) {
            fprintf(stderr, "Cannot open framebuffer \"%s\": %s\n",
                    m_framebuffer_path, strerror(errno));
            return false;
        }
    }

    /* Get variable framebuffer info */
    if (-1 == ioctl(m_framebuffer_fd, FBIOGET_VSCREENINFO, &vinfo)) {
        fprintf(stderr, "Can't get variable framebuffer info: %s\n",
                strerror(errno));
        return false;
    }

    return true;
}

size_t G2DDisplay::get_next_offset(const fb_var_screeninfo &vinfo)
{
    size_t wanted_height = ALIGN(vinfo.yres);
    size_t next_offset = vinfo.yoffset + wanted_height;
    if (next_offset >= vinfo.yres_virtual) {
        next_offset = 0;
    }
    return next_offset;
}

bool G2DDisplay::framebuffer_format_to_g2d(const fb_var_screeninfo &vinfo,
                                           g2d_format &format)
{
    /* Note that docs say there are 24-bit formats supported for G2D, but
     for example DG1 version of headers doesn't have them */
    if (16 == vinfo.bits_per_pixel) {
        // TODO: technically we should check if layout is really RGB565 but
        // it is :-)
        format = G2D_RGB565;
        return true;
    } else if (32 == vinfo.bits_per_pixel) {
        // TODO: G2D has several 32-bit formats, including RGB and BGR layouts,
        // with and without alpha channel, and alpha can be first or last byte,
        // so we should really check what our vinfo describes. But for now
        format = G2D_BGRA8888;
        return true;
    } else {
        fprintf(stderr, "%d bits per pixel not supported (should be 16 or 32)\n",
                vinfo.bits_per_pixel);
        return false;
    }
}

size_t G2DDisplay::align_to_64(size_t value)
{
    size_t r = value % 64;
    if (r) {
        value += 64 - r;
    }
    return value;
}
}
