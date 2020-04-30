/*
 * Copyright (c) 2019  AIRTAME ApS
 * All Rights Reserved.
 *
 * See LICENSE.txt for further information.
 */

#include <list>
#include <string>

#include <math.h>
#include <stdio.h>
#include <string.h>
#include <sys/time.h>
#include <unistd.h>

extern "C" {
#include <vpu_io.h>
#include <vpu_lib.h>
}

#include "g2d_display.hpp"
#include "ivf.h"
#include "jpeg_parser.hpp"
#include "stream.hpp"
#include "h264_nal.hpp"
#include "h264_stream_handler.hpp"
#include "jpeg_stream_handler.hpp"
#include "vp8_stream_handler.hpp"

double get_timestamp()
{
    timeval current;
    gettimeofday(&current, nullptr);
    return (double)current.tv_sec + ((double)current.tv_usec / 1000000.0);
}

airtame::StreamHandler *produce_handler(airtame::Stream &stream)
{
    const unsigned char *read_pointer = stream.get_read_pointer();
    /* Try to detect stream type */
    /* VP8 IVF container has magic numbers at the beginning, so check for it
     first (28 is last header byte we access) */
    if ((stream.get_size_left() >= 28)
        && !::memcmp(read_pointer, IVF_MAGIC_NUMBER, 4)) {
        fprintf(stderr, "\tIVF magic number detected\n");
        if (!::memcmp(read_pointer + 8, IVF_VP8_FOURCC, 4)) {
            fprintf(stderr, "\tVP8 content detected\n");
            return new airtame::VP8StreamHandler(stream);
        } else {
            fprintf(stderr, "\tFOURCC code in IVF stream is not VP8");
            return nullptr;
        }
    }

    /* JPEG JFIF also has magic number, so try that then */
    const char * jfif = "JFIF";
    if (stream.get_size_left() > 11) {
        /* What is usually called "JPEG file" starts with SOI marker (so 0xff
         and SOI bytes), then has APP0 marker (so 0xff and APP0), then two bytes
         of APP0 marker size which we ignore here, and bytes 6-11 contain ASCII
         string "JFIF", including terminating zero */
        if ((0xff == read_pointer[0]) && (MarkerType::SOI == (MarkerType)read_pointer[1])
            && (0xff == read_pointer[2]) && (MarkerType::APP0 == (MarkerType)read_pointer[3])
            && !::memcmp(read_pointer + 6, jfif, 5)) {
            return new airtame::JPEGStreamHandler(stream, true);
        }
    }

    /* OK, finally try scanning for h264 start code */
    if (at_h264_next_start_code(read_pointer, read_pointer + stream.get_size_left())) {
        return new airtame::H264StreamHandler(stream);
    }

    /* Couldn't recognize stream type */
    return nullptr;
}

/* Here we are using interpretation of g2d_surface as described in
 "i.MX Graphics User’s Guide" page 9, namely that surface itself spreads from
 0 to width and from 0 to height (horizontally/vertically), and the area of
 interest (where image should "land") is between left to right and bottom to top
 */
void compute_scaling(size_t image_width, size_t image_height,
                     g2d_surface &output)
{
    double output_width = output.right - output.left;
    double output_height = output.bottom - output.top;
    double scale_factor_width = output_width / image_width;
    double scale_factor_height = output_height / image_height;
    double frame_capacity_aspect_ratio = output_width / output_height;
    double image_aspect_ratio = (double)image_width / image_height;
    double scaling_factor = (frame_capacity_aspect_ratio > image_aspect_ratio)
        ? scale_factor_height : scale_factor_width;

    /* Round to the nearest integer when computing final resolution */
    size_t final_width = (size_t)(scaling_factor * image_width + 0.5);
    size_t final_height = (size_t)(scaling_factor * image_height + 0.5);

    /* Now fill in */
    output.left += (output_width - final_width) / 2;
    output.top += (output_height - final_height) / 2;
    output.right = output.left + final_width;
    output.bottom = output.top + final_height;
}

bool start_display(void *g2d, airtame::G2DDisplay &display,
                   std::list<airtame::StreamHandler *> &handlers)
{
    g2d_surface surface;
    if (!display.prepare_render(surface)) {
        return false;
    }

    /* Limit the number of clears because they affect the performance */
    static size_t clear_counter = display.get_number_of_buffers();
    if (clear_counter) {
        --clear_counter;
        if (g2d_clear(g2d, &surface)) {
            fprintf(stderr, "G2D clear failed");
            return false;
        }
    }

    /* Enable dithering for 16 bit format. This is done per-frame,
     because we allow for resolution/video mode change in flight and so
     surface.format may change as well */
    if (G2D_RGB565 == surface.format) {
        g2d_enable(g2d, G2D_DITHER);
    }

    /* Lay out all handlers on a matrix. This is very simple approach, but
     1) More or less all our sources have aspect ratio resembling the one
     on screen, so you want to divide width and height by same factor anyway
     2) This is more for test/demo than for real production stuff */

    size_t side = ::ceil(::sqrt(handlers.size()));
    size_t n = 0;

    for (auto h : handlers) {
        /* Compute matrix coordinates of this handler first */
        size_t j = n / side;
        size_t i = n - (j * side);
        ++n;

        /* Source frame description (NV12 data from VPU decoder) */
        g2d_surface src;
        src.format = h->is_interleaved() ? G2D_NV12 : G2D_I420;
        src.planes[0] = h->get_last_frame().dma->phy_addr;
        src.planes[1] = h->get_last_frame().dma->phy_addr
            + (h->get_last_frame().geometry.m_padded_width
               * h->get_last_frame().geometry.m_padded_height);
        src.planes[2] = (h->is_interleaved() ? 0 : src.planes[1])
            + (h->get_last_frame().geometry.m_padded_width
               * h->get_last_frame().geometry.m_padded_height / 4);
        src.left = h->get_last_frame().geometry.m_crop_left;
        src.top = h->get_last_frame().geometry.m_crop_top;
        src.right = h->get_last_frame().geometry.m_crop_left
            + h->get_last_frame().geometry.m_true_width;
        src.bottom = h->get_last_frame().geometry.m_crop_top
            + h->get_last_frame().geometry.m_true_height;
        /* stride same as width, mb size aligned */
        src.stride = h->get_last_frame().geometry.m_padded_width;
        src.width = h->get_last_frame().geometry.m_padded_width;
        src.height = h->get_last_frame().geometry.m_padded_height;
        src.blendfunc = G2D_ONE;
        src.global_alpha = 255;
        src.clrcolor = 0;
        src.rot = G2D_ROTATION_0;

        /* Now compute matrix cell coords we want to land in */
        g2d_surface cell = surface;
        cell.left = i * cell.width / side;
        cell.right = (i + 1) * cell.width / side;
        cell.top = j * cell.height / side;
        cell.bottom = (j + 1) * cell.height / side;

        /* Scale our frame to sit within the cell, but with proper aspect ratio */
        compute_scaling(h->get_last_frame().geometry.m_true_width,
                        h->get_last_frame().geometry.m_true_height,
                        cell);

        /* blit frame data into framebuffer. This is async operation - blitter
         will continue working in the background, while we'll do other stuff */
        if (g2d_blit(g2d, &src, &cell)) {
            fprintf(stderr, "G2D blit failure\n");
            return false;
        }
    }

    return true;
}

bool end_display(void *g2d, airtame::G2DDisplay &display)
{
    if (g2d_finish(g2d)) {
        fprintf(stderr, "G2D finish failed\n");
        return false;
    }

    if (!display.swap_buffers()) {
        return false;;
    }

    return true;
}

int main(int argc, char *argv[])
{
    if (argc < 3) {
        fprintf(stderr, "Usage:\n%s /dev/fd? file0[@offset] [file1[@offset]]...\n",
                argv[0]);
        return -1;
    }

    // TODO: ctrl-c handler!

    /* Gotta init VPU, or decode init will fail */
    if (RETCODE_SUCCESS != vpu_Init(nullptr)) {
        fprintf(stderr, "Could not initialize the VPU\n");
        return -1;
    }

    std::list<airtame::StreamHandler *> handlers;

    /* Iterate over provided files, trying to create handlers for them */
    for (int i = 2; i < argc; i++) {

        /* Notation name@offset is accepted */
        std::string name = argv[i];
        size_t offset = 0;
        size_t oo = name.find('@');

        if (std::string::npos != oo) {
            offset = ::atoi(argv[i] + oo + 1);
            name = name.substr(0, oo);
        }

        airtame::Stream stream;
        fprintf(stderr, "Trying to open %s\n", name.c_str());
        if (!stream.open(name.c_str())) {
            continue;
        }

        airtame::StreamHandler *handler = produce_handler(stream);
        if (handler) {
            handler->offset(offset);
            /* Success, stream recognized */
            if (handler->init()) {
                handlers.push_back(handler);
            } else {
                fprintf(stderr, "Couldn't init the decoder\n");
                delete handler;
            }
        } else {
            fprintf(stderr, "Couldn't recognize stream type of %s. Most likely "
                            "neither raw h264, vp8 ivf or jpeg jfif\n",
                    argv[i]);
        }
    }

    if (handlers.empty()) {
        fprintf(stderr, "Couldn't open any stream, exiting\n");
        return -1;
    }

    /* Now need to init G2D */
    void *g2d;
    if (g2d_open(&g2d)) {
        fprintf(stderr, "Failed to init G2D\n");
        return -1;
    }

    if (g2d_make_current(g2d, G2D_HARDWARE_2D)) {
        fprintf(stderr, "Failed to set HW type for G2D\n");
        return -1;
    }

    /* Display loop */
    airtame::G2DDisplay display(argv[1]);
    double start = get_timestamp();
    double decode_sum = 0, decode_partial_sum = 0;
    double display_sum = 0, display_partial_sum = 0;
    size_t frames = 0;
    size_t start_frames = 0;
    bool new_frame = true;
    bool do_display = false;

    while (new_frame) {
        /* Start display process for already decoded frames (if any). Display
         process itself is asynchronous, and will continue after return from
         start_display() call */
        double display_start = get_timestamp();
        if (do_display) {
            start_display(g2d, display, handlers);
        }

        /* Start decode for next set of frames */
        double decode_start = get_timestamp();

        /* Try to step all handlers, and see if at least one new frame is
         produced */
        new_frame = false;
        for (auto h : handlers) {
            if (h->step()) {
                new_frame = true;
            }
        }

        if (new_frame) {
            double decode_end = get_timestamp();
            decode_sum += decode_end - decode_start;
            decode_partial_sum += decode_end - decode_start;
            ++frames;
        }

        /* Phase III: wait for the blit operation to finish */
        if (do_display) {
            end_display(g2d, display);
        } else {
            /* Only first iteration skips display */
            do_display = true;
        }

        /* Now we can get rid of displayed buffers. Interestingly so, this is
         not costless - it can take 1ms+ */
        for (auto h : handlers) {
            h->swap();
        }

        double display_end = get_timestamp();
        display_sum += display_end - display_start;
        display_partial_sum += display_end - display_start;

        /* FPS counter */
        double now = get_timestamp();
        if (((int)start != (int)now) || !new_frame) {
            double fps = (double)(frames - start_frames) / display_partial_sum;
            double avg_decode = decode_partial_sum / (frames - start_frames);
            double avg_display = display_partial_sum / (frames - start_frames);
            fprintf(stderr, "FPS=%.2f (%.2fms), average decode %.2fms\n", fps,
                    avg_display * 1000, avg_decode * 1000);
            start = now;
            start_frames = frames;
            decode_partial_sum = 0.0;
            display_partial_sum = 0.0;
        }
    }

    /* Final stats */
    fprintf(stderr, "Decoded %zu frames, average FPS=%.2f (%.2fms), average "
                    "decode %.2fms\n", frames, (double)frames / display_sum,
            1000 * display_sum / frames, 1000 * decode_sum / frames);

    /* Get rid of open stream handlers */
    while (!handlers.empty()) {
        delete handlers.front();
        handlers.pop_front();
    }

    // TODO: should deinit g2d and vpu now

    return 0;
}
