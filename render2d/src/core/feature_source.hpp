// SPDX-License-Identifier: AGPL-3.0-or-later
// Anything that can hand out features one at a time: a GDAL layer, or an
// in-memory list in the tests. The renderer depends on this, not on GDAL.
// See notions §17, §25, §30 and §50.
#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <vector>

#include "core/attributes.hpp"
#include "core/envelope.hpp"
#include "core/geometry.hpp"

namespace r2d {

// What happened while reading: nothing is silently dropped.
struct ReadStats {
    std::size_t read        = 0;   // features returned by the source
    std::size_t drawn       = 0;   // converted and handed to the visitor
    std::size_t empty       = 0;   // no geometry, or an empty one
    std::size_t unsupported = 0;   // a type we do not draw (yet)

    ReadStats& operator+=(const ReadStats& o) {
        read += o.read; drawn += o.drawn; empty += o.empty; unsupported += o.unsupported;
        return *this;
    }
};

// The feature's id (FID): unique in its layer, given by GDAL (the rank in a
// FlatGeobuf or a shapefile, the primary key in PostGIS). notions §50.
using FeatureId = std::int64_t;

using FeatureVisitor = std::function<void(const Geometry&, const Attributes&, FeatureId)>;

class FeatureSource {
public:
    virtual ~FeatureSource() = default;

    // The layer's fields, in column order. Attributes are indexed like this.
    virtual const std::vector<FieldDef>& fields() const = 0;

    // Only these columns are read; the others stay NULL. Empty = no attribute
    // at all, the default: a style without filter reads geometries only. §30.
    virtual void setNeededFields(const std::vector<int>& columns) = 0;

    // Only features touching this rectangle are returned (source CRS).
    virtual void setSpatialFilter(const Envelope& e) = 0;

    // One full read, feature by feature. Can be called again (one call per
    // rendering pass). The Geometry and Attributes are reused for the next feature.
    virtual ReadStats forEach(const FeatureVisitor& visit) = 0;
};

} // namespace r2d
