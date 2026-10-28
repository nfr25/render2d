// SPDX-License-Identifier: AGPL-3.0-or-later
#include "core/view.hpp"

#include <algorithm>
#include <cmath>

namespace r2d {

std::optional<View> makeView(const Envelope& requested, ImageSize size) {
    if (!requested.isValid() || size.width <= 0 || size.height <= 0) {
        return std::nullopt;
    }

    View v;
    v.requested = requested;
    v.size      = size;

    // 1. Metres per pixel in each direction, keep the larger one so the whole
    //    requested area fits and pixels stay square. notions §4.
    const double resX = requested.width()  / size.width;
    const double resY = requested.height() / size.height;
    v.resolution = std::max(resX, resY);

    // 2. Shown envelope: same center, image size times resolution.
    const Point  c     = requested.center();
    const double halfW = size.width  * v.resolution / 2.0;
    const double halfH = size.height * v.resolution / 2.0;
    v.shown = {c.x - halfW, c.y - halfH, c.x + halfW, c.y + halfH};

    // "Widened" means: the image shows visibly more than asked, at least half a
    // pixel more in one direction. A rounded height, or a bbox computed by a
    // browser, differs by a tiny fraction of a pixel: that is not widening.
    const double halfPixel = 0.5 * v.resolution;
    v.widened = v.shown.width()  - requested.width()  > halfPixel ||
                v.shown.height() - requested.height() > halfPixel;

    // 3. World -> image. notions §3 and §5, with s = 1 / res:
    //      x =  s*X - s*Xmin
    //      y = -s*Y + s*Ymax      (Y flip: image rows go down)
    const double s = 1.0 / v.resolution;
    v.worldToImage = {s, 0.0, 0.0, -s, -s * v.shown.xmin, s * v.shown.ymax};

    // 4. Image -> world. Always invertible here (s > 0), so no check needed.
    v.imageToWorld = *v.worldToImage.inverse();
    return v;
}

} // namespace r2d
