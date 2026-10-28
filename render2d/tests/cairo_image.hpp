// SPDX-License-Identifier: AGPL-3.0-or-later
// A small white in-memory image with a canvas on it, for pixel tests.
#pragma once

#include <cairo.h>

#include <cstdint>
#include <cstring>

#include "backend/cairo/cairo_canvas.hpp"

struct TestImage {
    cairo_surface_t*  surface;
    cairo_t*          cr;
    r2d::CairoCanvas  canvas;

    TestImage(int w, int h)
        : surface(cairo_image_surface_create(CAIRO_FORMAT_ARGB32, w, h)),
          cr(cairo_create(surface)),
          canvas(cr) {
        canvas.clear(r2d::Color::fromHex(0xFFFFFF));
    }
    ~TestImage() { cairo_destroy(cr); cairo_surface_destroy(surface); }
    TestImage(const TestImage&) = delete;
    TestImage& operator=(const TestImage&) = delete;

    // Channels of pixel (x, y), 0..255. Opaque image, so no premultiply issue.
    int red(int x, int y)   { return channel(x, y, 16); }
    int green(int x, int y) { return channel(x, y, 8); }
    int blue(int x, int y)  { return channel(x, y, 0); }

    int channel(int x, int y, int shift) {
        cairo_surface_flush(surface);
        const unsigned char* data = cairo_image_surface_get_data(surface);
        const int stride = cairo_image_surface_get_stride(surface);
        std::uint32_t argb;
        std::memcpy(&argb, data + y * stride + x * 4, 4);
        return static_cast<int>((argb >> shift) & 0xFF);
    }
};
