// SPDX-License-Identifier: AGPL-3.0-or-later
// Checks the canvas on real pixels, read back from a Cairo image surface.
// Chapter 9 (half pixel) and chapter 11 (fill rules) of notions.html.

#include "backend/cairo/cairo_image.hpp"
#include "backend/cairo/cairo_target.hpp"
#include "cairo_image.hpp"
#include "check.hpp"

#include <cstring>
#include <filesystem>

using namespace r2d;

namespace {

using Image = TestImage;

void verticalLine(Canvas& cv, double x, double height) {
    cv.moveTo({x, 0.0});
    cv.lineTo({x, height});
    cv.stroke(StrokeStyle{.color = Color::fromHex(0x000000), .width = 1.0}, PathMode::Consume);
}

// §9: a 1-pixel line at x = 4.5 fills pixel 4 completely.
void testHalfPixelSharp() {
    Image img(10, 10);
    verticalLine(img.canvas, 4.5, 10.0);
    CHECK(img.red(3, 5) == 255);
    CHECK(img.red(4, 5) == 0);
    CHECK(img.red(5, 5) == 255);
}

// §9: the same line at x = 4 covers half of pixels 3 and 4 -> two greys.
void testHalfPixelBlurred() {
    Image img(10, 10);
    verticalLine(img.canvas, 4.0, 10.0);
    CHECK(img.red(2, 5) == 255);
    CHECK_NEAR(img.red(3, 5), 128, 3);
    CHECK_NEAR(img.red(4, 5), 128, 3);
    CHECK(img.red(5, 5) == 255);
}

// §11: outer ring and hole turning the same way.
void squareWithHole(Canvas& cv) {
    cv.moveTo({2, 2});  cv.lineTo({18, 2});  cv.lineTo({18, 18}); cv.lineTo({2, 18});  cv.closePath();
    cv.moveTo({7, 7});  cv.lineTo({13, 7});  cv.lineTo({13, 13}); cv.lineTo({7, 13});  cv.closePath();
}

void testFillRules() {
    {
        Image img(20, 20);
        squareWithHole(img.canvas);
        img.canvas.fill(FillStyle{.color = Color::fromHex(0x000000), .rule = FillRule::EvenOdd},
                        PathMode::Consume);
        CHECK(img.red(4, 10) == 0);     // built part: painted
        CHECK(img.red(10, 10) == 255);  // courtyard: empty, even with same ring order
    }
    {
        Image img(20, 20);
        squareWithHole(img.canvas);
        img.canvas.fill(FillStyle{.color = Color::fromHex(0x000000), .rule = FillRule::NonZero},
                        PathMode::Consume);
        CHECK(img.red(4, 10) == 0);
        CHECK(img.red(10, 10) == 0);    // non-zero: +1 +1 = 2, the hole is lost
    }
}

// Keep mode: the path survives the fill and can be stroked.
void testKeepPath() {
    Image img(20, 20);
    img.canvas.moveTo({2, 2}); img.canvas.lineTo({18, 2}); img.canvas.lineTo({18, 18});
    img.canvas.lineTo({2, 18}); img.canvas.closePath();
    img.canvas.fillAndStroke(FillStyle{.color = Color::fromHex(0x808080)},
                             StrokeStyle{.color = Color::fromHex(0x000000), .width = 2.0});
    CHECK(img.red(10, 10) == 0x80); // inside: fill colour
    CHECK(img.red(10, 2) == 0);     // on the edge: stroke colour
}

void testFormatFromPath() {
    CHECK(formatFromPath("map.png") == OutputFormat::Png);
    CHECK(formatFromPath("MAP.PNG") == OutputFormat::Png);
    CHECK(formatFromPath("out/carte.svg") == OutputFormat::Svg);
    CHECK(formatFromPath("a.b.pdf") == OutputFormat::Pdf);
    CHECK(!formatFromPath("photo.jpg").has_value());
    CHECK(!formatFromPath("noextension").has_value());
}

// §48: orange (255, 128, 0) at 50 % is 0x80804000, stored as B, G, R, A.
void testImageBytes() {
    std::string error;
    auto img = CairoImage::create(ImageSize{4, 4}, error);
    CHECK(img != nullptr);
    if (!img) return;
    CHECK(img->stride() >= 4 * 4);
    img->canvas().clear(Color::fromHex(0xFF8000, 128.0 / 255.0));
    const unsigned char* p = img->pixels() + 2 * img->stride() + 1 * 4;   // pixel (1, 2)
    CHECK(p[0] == 0x00);   // B
    CHECK(p[1] == 0x40);   // G: 128 x 128 / 255 = 64.25 -> 64
    CHECK(p[2] == 0x80);   // R: 255 x 128 / 255 = 128
    CHECK(p[3] == 0x80);   // A
}

// §48: the same drawing in memory and in a PNG file gives the same pixels.
void drawScene(Canvas& cv) {
    cv.clear(Color::fromHex(0xFFFFFF));
    cv.moveTo({3.3, 2.0});
    cv.lineTo({28.0, 9.5});
    cv.lineTo({12.0, 27.7});
    cv.closePath();
    cv.fillAndStroke(FillStyle{.color = Color::fromHex(0x2B6C8F)},
                     StrokeStyle{.color = Color::fromHex(0xB5541C), .width = 1.7});
}

void testImageSameAsPng() {
    const std::string path = (std::filesystem::temp_directory_path() / "r2d_test_image.png").string();
    std::string error;
    auto target = CairoTarget::create(path, ImageSize{32, 32}, error);
    auto img    = CairoImage::create(ImageSize{32, 32}, error);
    CHECK(target != nullptr && img != nullptr);
    if (!target || !img) return;
    drawScene(target->canvas());
    drawScene(img->canvas());
    CHECK(target->finish(error));

    cairo_surface_t* png = cairo_image_surface_create_from_png(path.c_str());
    CHECK(cairo_surface_status(png) == CAIRO_STATUS_SUCCESS);
    const unsigned char* a = cairo_image_surface_get_data(png);
    const int strideA      = cairo_image_surface_get_stride(png);
    bool same = a != nullptr;
    for (int y = 0; same && y < 32; ++y)
        same = std::memcmp(a + y * strideA, img->pixels() + y * img->stride(), 32 * 4) == 0;
    CHECK(same);
    cairo_surface_destroy(png);
}

} // namespace

int main() {
    testHalfPixelSharp();
    testHalfPixelBlurred();
    testFillRules();
    testKeepPath();
    testFormatFromPath();
    testImageBytes();
    testImageSameAsPng();
    return check::summary();
}
