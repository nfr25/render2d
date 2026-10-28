// SPDX-License-Identifier: AGPL-3.0-or-later
// A 2D point in double precision.
// Used for both world coordinates (metres) and image coordinates (pixels):
// the meaning comes from the context, not from the type.
// See notions §2 (two frames) and §8 (why double).
#pragma once

namespace r2d {

struct Point {
    double x = 0.0;
    double y = 0.0;
};

} // namespace r2d
