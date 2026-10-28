// SPDX-License-Identifier: AGPL-3.0-or-later
// An axis-aligned rectangle in world coordinates (a "bbox").
// See notions §3.
#pragma once

#include "core/point.hpp"

namespace r2d {

struct Envelope {
    double xmin = 0.0;
    double ymin = 0.0;
    double xmax = 0.0;
    double ymax = 0.0;

    double width()  const { return xmax - xmin; }
    double height() const { return ymax - ymin; }
    Point  center() const { return {(xmin + xmax) / 2.0, (ymin + ymax) / 2.0}; }

    // True when the rectangle has a strictly positive area.
    // Written with ">" so that NaN values also count as invalid.
    bool isValid() const { return width() > 0.0 && height() > 0.0; }

    // The same rectangle, `d` larger on every side (the filter margin, §18).
    Envelope expandedBy(double d) const { return {xmin - d, ymin - d, xmax + d, ymax + d}; }

    // Smallest rectangle containing both (the total extent of several layers).
    Envelope merged(const Envelope& o) const {
        return {xmin < o.xmin ? xmin : o.xmin, ymin < o.ymin ? ymin : o.ymin,
                xmax > o.xmax ? xmax : o.xmax, ymax > o.ymax ? ymax : o.ymax};
    }
};

} // namespace r2d
