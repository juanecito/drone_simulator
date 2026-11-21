// SPDX-FileCopyrightText: 2026 Juan Maria Gomez Lopez <juan-maria.gomez-lopez@tutamail.com>
// SPDX-License-Identifier: MIT
//
// LAS / LAZ fixtures written in-test with LASzip (SPECIFICATION §8: terrain — LAS/LAZ): points
// sampled from a known surface, with ground, vegetation, noise and withheld points.

#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include <laszip/laszip_api.h>

namespace ds::test {

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wconversion"  // LASzip's point fields are bit-fields.

// A point to write.
struct LidarPoint {
    double x = 0.0;
    double y = 0.0;
    double z = 0.0;
    int classification = 2;
    bool withheld = false;
};

// How to write a file.
struct LidarFileOptions {
    int versionMinor = 2;       // 2 ⇒ LAS 1.2 (point format 1), 4 ⇒ LAS 1.4 (point format 6).
    bool compressed = false;    // LAZ.
    int epsg = 25831;           // GeoTIFF ProjectedCSTypeGeoKey; 0 ⇒ none.
    std::string wkt;            // WKT record (2112) instead of the keys, if set.
    bool verticalFeet = false;  // VerticalUnitsGeoKey = foot (heights are then written in feet).
};

// Known surface (metres): a slope with gentle waves, around 100 m.
inline double lidarSurface(double x, double y) {
    return 100.0 + 0.05 * (x - 400000.0) + 0.02 * (y - 4600000.0) + 3.0 * std::sin(x / 40.0);
}

// Points every @p spacing metres over [x0, x0 + w] × [y0, y0 + h] (deterministic jitter):
// ground on the surface, and every third one a vegetation point 8 m above it. Points inside
// the squares of @p gaps (x, y, side) are left out.
inline std::vector<LidarPoint> lidarPoints(double x0, double y0, double w, double h, double spacing,
                                           const std::vector<std::array<double, 3>>& gaps = {}) {
    std::vector<LidarPoint> out;
    int k = 0;
    for (double y = y0; y <= y0 + h; y += spacing) {
        for (double x = x0; x <= x0 + w; x += spacing) {
            ++k;
            const double jx = x + spacing * 0.3 * std::sin(k * 1.7);
            const double jy = y + spacing * 0.3 * std::cos(k * 2.3);
            bool skip = false;
            for (const auto& g : gaps) {
                skip = skip || (jx >= g[0] && jx < g[0] + g[2] && jy >= g[1] && jy < g[1] + g[2]);
            }
            if (skip || jx < x0 || jy < y0 || jx > x0 + w || jy > y0 + h) {
                continue;
            }
            out.push_back({jx, jy, lidarSurface(jx, jy), 2, false});
            if (k % 3 == 0) {
                out.push_back({jx, jy, lidarSurface(jx, jy) + 8.0, 5, false});
            }
        }
    }
    return out;
}

// Writes @p points; false (with @p error) on failure.
inline bool writeLidar(const std::filesystem::path& file, const std::vector<LidarPoint>& points,
                       const LidarFileOptions& o = {}, std::string* error = nullptr) {
    laszip_POINTER p = nullptr;
    if (laszip_create(&p) != 0) {
        return false;
    }
    const auto fail = [&] {
        if (error != nullptr) {
            laszip_CHAR* text = nullptr;
            laszip_get_error(p, &text);
            *error = text != nullptr ? text : "?";
        }
        laszip_destroy(p);
        return false;
    };
    laszip_header_struct* h = nullptr;
    laszip_get_header_pointer(p, &h);
    const bool v14 = o.versionMinor >= 4;
    h->version_major = 1;
    h->version_minor = static_cast<laszip_U8>(v14 ? 4 : 2);
    h->header_size = static_cast<laszip_U16>(v14 ? 375 : 227);
    h->offset_to_point_data = h->header_size;
    h->point_data_format = static_cast<laszip_U8>(v14 ? 6 : 1);
    h->point_data_record_length = static_cast<laszip_U16>(v14 ? 30 : 28);
    h->x_scale_factor = h->y_scale_factor = h->z_scale_factor = 0.001;
    h->x_offset = 400000.0;
    h->y_offset = 4600000.0;
    h->z_offset = 0.0;
    const double zScale = o.verticalFeet ? 1.0 / 0.3048 : 1.0;
    if (!o.wkt.empty()) {
        if (laszip_add_vlr(p, "LASF_Projection", 2112, static_cast<laszip_U16>(o.wkt.size() + 1),
                           "WKT", reinterpret_cast<const laszip_U8*>(o.wkt.c_str())) != 0) {
            return fail();
        }
    } else if (o.epsg != 0) {
        std::vector<std::uint16_t> keys{1, 1, 0, 0};
        const auto key = [&](std::uint16_t id, std::uint16_t value) {
            keys.insert(keys.end(), {id, 0, 1, value});
            ++keys[3];
        };
        key(1024, 1);  // GTModelTypeGeoKey: projected.
        key(3072, static_cast<std::uint16_t>(o.epsg));
        if (o.verticalFeet) {
            key(4099, 9002);
        }
        if (laszip_add_vlr(p, "LASF_Projection", 34735, static_cast<laszip_U16>(keys.size() * 2),
                           "GeoKeys", reinterpret_cast<const laszip_U8*>(keys.data())) != 0) {
            return fail();
        }
    }
    // Counts and bounds before writing (the header is written first).
    const auto n = static_cast<laszip_U32>(points.size());
    if (v14) {
        h->number_of_point_records = 0;  // Legacy counts stay 0 for point formats ≥ 6.
        h->extended_number_of_point_records = n;
        h->extended_number_of_points_by_return[0] = n;
    } else {
        h->number_of_point_records = n;
        h->number_of_points_by_return[0] = n;
    }
    if (!points.empty()) {
        h->min_x = h->max_x = points[0].x;
        h->min_y = h->max_y = points[0].y;
        h->min_z = h->max_z = points[0].z * zScale;
        for (const LidarPoint& q : points) {
            h->min_x = std::min(h->min_x, q.x);
            h->max_x = std::max(h->max_x, q.x);
            h->min_y = std::min(h->min_y, q.y);
            h->max_y = std::max(h->max_y, q.y);
            h->min_z = std::min(h->min_z, q.z * zScale);
            h->max_z = std::max(h->max_z, q.z * zScale);
        }
    }
    if (o.compressed && v14) {
        laszip_request_native_extension(p, 1);
    }
    if (laszip_open_writer(p, file.c_str(), o.compressed ? 1 : 0) != 0) {
        return fail();
    }
    laszip_point_struct* pt = nullptr;
    laszip_get_point_pointer(p, &pt);
    for (const LidarPoint& q : points) {
        pt->X = static_cast<laszip_I32>(std::lround((q.x - h->x_offset) / h->x_scale_factor));
        pt->Y = static_cast<laszip_I32>(std::lround((q.y - h->y_offset) / h->y_scale_factor));
        pt->Z = static_cast<laszip_I32>(std::lround(q.z * zScale / h->z_scale_factor));
        pt->return_number = 1;
        pt->number_of_returns = 1;
        if (v14) {
            pt->extended_point_type = 1;
            pt->extended_return_number = 1;
            pt->extended_number_of_returns = 1;
            pt->extended_classification = static_cast<laszip_U8>(q.classification);
            pt->classification = static_cast<laszip_U8>(std::min(q.classification, 31));
            pt->extended_classification_flags = static_cast<laszip_U8>(q.withheld ? 0x4 : 0);
        } else {
            pt->classification = static_cast<laszip_U8>(q.classification);
        }
        pt->withheld_flag = q.withheld ? 1 : 0;
        if (laszip_write_point(p) != 0) {
            return fail();
        }
    }
    if (laszip_close_writer(p) != 0) {
        return fail();
    }
    laszip_destroy(p);
    return true;
}

#pragma GCC diagnostic pop

}  // namespace ds::test
