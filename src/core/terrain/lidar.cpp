// SPDX-FileCopyrightText: 2026 Juan Maria Gomez Lopez <juan-maria.gomez-lopez@tutamail.com>
// SPDX-License-Identifier: MIT

#include "ds/terrain/lidar.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <format>
#include <limits>
#include <memory>
#include <system_error>
#include <vector>

#include "ds/common/log.hpp"
#include "ds/common/sha256.hpp"

#include "gdal_support.hpp"
#include <cpl_conv.h>
#include <cpl_string.h>
#include <gdal.h>
#include <laszip/laszip_api.h>
#include <ogr_srs_api.h>
#include <unistd.h>

namespace ds::terrain {
namespace {

namespace fs = std::filesystem;

constexpr float kNaN = std::numeric_limits<float>::quiet_NaN();
constexpr double kNoData = -9999.0;
constexpr double kFootM = 0.3048;
constexpr double kUsFootM = 1200.0 / 3937.0;
constexpr std::size_t kMaxRimPoints = 3000;

TerrainError error(TerrainErrorCode code, std::string message) {
    return TerrainError{code, std::move(message)};
}

// --- LASzip reader ----------------------------------------------------------------------------

class Reader {
public:
    Reader() = default;
    Reader(const Reader&) = delete;
    Reader& operator=(const Reader&) = delete;
    Reader(Reader&&) = delete;
    Reader& operator=(Reader&&) = delete;
    ~Reader() {
        if (p_ != nullptr) {
            if (open_) {
                laszip_close_reader(p_);
            }
            laszip_destroy(p_);
        }
    }

    TerrainResult<void> open(const fs::path& file) {
        std::error_code ec;
        if (!fs::is_regular_file(file, ec)) {
            return std::unexpected(
                error(TerrainErrorCode::FileNotFound, std::format("{}: not found", file.string())));
        }
        if (laszip_create(&p_) != 0) {
            return std::unexpected(error(TerrainErrorCode::CorruptData, "LASzip: out of memory"));
        }
        // LAS 1.4 point types written in compatibility mode read back as extended points.
        laszip_request_compatibility_mode(p_, 1);
        laszip_BOOL compressed = 0;
        if (laszip_open_reader(p_, file.c_str(), &compressed) != 0) {
            return std::unexpected(error(TerrainErrorCode::CorruptData,
                                         std::format("{}: not a readable LAS / LAZ file ({})",
                                                     file.filename().string(), lastError())));
        }
        open_ = true;
        compressed_ = compressed != 0;
        laszip_get_header_pointer(p_, &header_);
        laszip_get_point_pointer(p_, &point_);
        // The total is in the header (laszip_get_point_count counts the points read so far);
        // LAS 1.4 point formats keep the legacy 32-bit count at 0.
        count_ = header_->extended_number_of_point_records != 0
                     ? header_->extended_number_of_point_records
                     : header_->number_of_point_records;
        return {};
    }

    [[nodiscard]] std::string lastError() const {
        laszip_CHAR* text = nullptr;
        if (p_ != nullptr && laszip_get_error(p_, &text) == 0 && text != nullptr) {
            return text;
        }
        return "unknown error";
    }
    [[nodiscard]] const laszip_header_struct& header() const { return *header_; }
    [[nodiscard]] const laszip_point_struct& point() const { return *point_; }
    [[nodiscard]] std::uint64_t count() const noexcept { return count_; }
    [[nodiscard]] bool compressed() const noexcept { return compressed_; }
    bool next() { return laszip_read_point(p_) == 0; }

private:
    laszip_POINTER p_ = nullptr;
    laszip_header_struct* header_ = nullptr;
    laszip_point_struct* point_ = nullptr;
    std::uint64_t count_ = 0;
    bool open_ = false;
    bool compressed_ = false;
};

// CRS of the VLRs: the WKT record (2112), else the GeoTIFF keys (34735).
void readCrs(const laszip_header_struct& h, LidarHeader& out) {
    for (laszip_U32 i = 0; i < h.number_of_variable_length_records; ++i) {
        const laszip_vlr_struct& v = h.vlrs[i];
        if (std::strncmp(v.user_id, "LASF_Projection", 16) != 0 || v.data == nullptr) {
            continue;
        }
        if (v.record_id == 2112 && v.record_length_after_header > 0) {
            std::string wkt(reinterpret_cast<const char*>(v.data), v.record_length_after_header);
            wkt.erase(std::find(wkt.begin(), wkt.end(), '\0'), wkt.end());
            if (!wkt.empty()) {
                out.crs = wkt;
                return;
            }
        }
    }
    for (laszip_U32 i = 0; i < h.number_of_variable_length_records; ++i) {
        const laszip_vlr_struct& v = h.vlrs[i];
        if (std::strncmp(v.user_id, "LASF_Projection", 16) != 0 || v.record_id != 34735 ||
            v.data == nullptr || v.record_length_after_header < 8) {
            continue;
        }
        const std::size_t words = v.record_length_after_header / 2U;
        std::vector<std::uint16_t> k(words);
        std::memcpy(k.data(), v.data, words * 2U);  // Little-endian, like the file.
        const std::size_t keys = std::min<std::size_t>(k[3], (words - 4U) / 4U);
        int projected = 0;
        int geographic = 0;
        for (std::size_t j = 0; j < keys; ++j) {
            const std::uint16_t id = k[4 + 4 * j];
            const std::uint16_t location = k[4 + 4 * j + 1];
            const std::uint16_t value = k[4 + 4 * j + 3];
            if (location != 0) {
                continue;  // Doubles or text: not needed for EPSG codes.
            }
            if (id == 3072) {
                projected = value;
            } else if (id == 2048) {
                geographic = value;
            } else if (id == 4099) {
                if (value == 9002) {
                    out.verticalToMetres = kFootM;
                } else if (value == 9003) {
                    out.verticalToMetres = kUsFootM;
                } else if (value == 9001) {
                    out.verticalToMetres = 1.0;
                }
            }
        }
        // 32767 is "user-defined": not usable without the full key set.
        if (projected > 0 && projected != 32767) {
            out.crs = std::format("EPSG:{}", projected);
        } else if (geographic > 0 && geographic != 32767) {
            out.crs = std::format("EPSG:{}", geographic);
        }
        return;
    }
}

struct Srs {
    OGRSpatialReferenceH h = nullptr;
    Srs() = default;
    Srs(const Srs&) = delete;
    Srs& operator=(const Srs&) = delete;
    Srs(Srs&&) = delete;
    Srs& operator=(Srs&&) = delete;
    ~Srs() {
        if (h != nullptr) {
            OSRDestroySpatialReference(h);
        }
    }
};

// --- Hole filling -----------------------------------------------------------------------------

struct Pt {
    double x;
    double y;
    double z;
};

struct Tri {
    int a;
    int b;
    int c;
};

// Delaunay triangulation (Bowyer–Watson) of @p pts; the last three points are a super
// triangle added here. Coordinates are small integers, so the tests are exact.
std::vector<Tri> delaunay(std::vector<Pt>& pts) {
    double minX = pts[0].x;
    double maxX = pts[0].x;
    double minY = pts[0].y;
    double maxY = pts[0].y;
    for (const Pt& p : pts) {
        minX = std::min(minX, p.x);
        maxX = std::max(maxX, p.x);
        minY = std::min(minY, p.y);
        maxY = std::max(maxY, p.y);
    }
    const double span = std::max(maxX - minX, maxY - minY) + 1.0;
    const double cx = (minX + maxX) / 2.0;
    const double cy = (minY + maxY) / 2.0;
    const int n = static_cast<int>(pts.size());
    pts.push_back({cx - 20.0 * span, cy - 10.0 * span, 0.0});
    pts.push_back({cx + 20.0 * span, cy - 10.0 * span, 0.0});
    pts.push_back({cx, cy + 20.0 * span, 0.0});
    std::vector<Tri> tris{{n, n + 1, n + 2}};
    const auto orient = [&](const Tri& t) {
        const Pt& a = pts[static_cast<std::size_t>(t.a)];
        const Pt& b = pts[static_cast<std::size_t>(t.b)];
        const Pt& c = pts[static_cast<std::size_t>(t.c)];
        return (b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x);
    };
    // True if @p p is strictly inside the circumcircle of @p t (counter-clockwise).
    const auto inCircle = [&](const Tri& t, const Pt& p) {
        const Pt& a = pts[static_cast<std::size_t>(t.a)];
        const Pt& b = pts[static_cast<std::size_t>(t.b)];
        const Pt& c = pts[static_cast<std::size_t>(t.c)];
        const double ax = a.x - p.x;
        const double ay = a.y - p.y;
        const double bx = b.x - p.x;
        const double by = b.y - p.y;
        const double ccx = c.x - p.x;
        const double ccy = c.y - p.y;
        const double det = (ax * ax + ay * ay) * (bx * ccy - ccx * by) -
                           (bx * bx + by * by) * (ax * ccy - ccx * ay) +
                           (ccx * ccx + ccy * ccy) * (ax * by - bx * ay);
        return det > 0.0;
    };
    std::vector<std::pair<int, int>> edges;
    std::vector<Tri> kept;
    for (int i = 0; i < n; ++i) {
        const Pt& p = pts[static_cast<std::size_t>(i)];
        edges.clear();
        kept.clear();
        for (const Tri& t : tris) {
            if (inCircle(t, p)) {
                edges.emplace_back(t.a, t.b);
                edges.emplace_back(t.b, t.c);
                edges.emplace_back(t.c, t.a);
            } else {
                kept.push_back(t);
            }
        }
        // The cavity's boundary: edges used by one removed triangle only.
        for (std::size_t e = 0; e < edges.size(); ++e) {
            bool shared = false;
            for (std::size_t f = 0; f < edges.size(); ++f) {
                if (e != f && edges[e].first == edges[f].second &&
                    edges[e].second == edges[f].first) {
                    shared = true;
                    break;
                }
            }
            if (!shared) {
                Tri t{edges[e].first, edges[e].second, i};
                if (orient(t) < 0.0) {
                    std::swap(t.a, t.b);
                }
                if (orient(t) != 0.0) {
                    kept.push_back(t);
                }
            }
        }
        tris.swap(kept);
    }
    std::erase_if(tris, [n](const Tri& t) { return t.a >= n || t.b >= n || t.c >= n; });
    pts.resize(static_cast<std::size_t>(n));
    return tris;
}

class Grid {
public:
    Grid(int cols, int rows)
        : cols_(cols),
          rows_(rows),
          mean_(static_cast<std::size_t>(cols) * static_cast<std::size_t>(rows), kNaN),
          count_(mean_.size(), 0) {}

    void add(int col, int row, float z, bool lowest) {
        const std::size_t i = index(col, row);
        std::uint16_t& n = count_[i];
        if (n == 0) {
            mean_[i] = z;
            n = 1;
        } else if (lowest) {
            mean_[i] = std::min(mean_[i], z);
        } else if (n < std::numeric_limits<std::uint16_t>::max()) {
            ++n;
            mean_[i] += (z - mean_[i]) / static_cast<float>(n);
        }
    }

    [[nodiscard]] std::size_t index(int col, int row) const noexcept {
        return static_cast<std::size_t>(row) * static_cast<std::size_t>(cols_) +
               static_cast<std::size_t>(col);
    }
    [[nodiscard]] int cols() const noexcept { return cols_; }
    [[nodiscard]] int rows() const noexcept { return rows_; }
    std::vector<float>& values() noexcept { return mean_; }
    void releaseCounts() { std::vector<std::uint16_t>().swap(count_); }

private:
    int cols_;
    int rows_;
    std::vector<float> mean_;
    std::vector<std::uint16_t> count_;
};

// Fills the gaps of @p grid up to @p maxCells across that do not touch its border. Returns
// the number of filled cells; the rest stays NaN.
std::uint64_t fillHoles(Grid& grid, int maxCells, LidarInterpolation how, std::stop_token stop,
                        const std::function<void(double)>& progress) {
    std::vector<float>& v = grid.values();
    const int cols = grid.cols();
    const int rows = grid.rows();
    std::vector<std::uint8_t> seen(v.size(), 0);  // 1 visited hole, 2 rim marker.
    std::uint64_t filled = 0;
    std::vector<std::size_t> hole;
    std::vector<std::size_t> queue;
    std::vector<std::size_t> rim;
    for (int r0 = 0; r0 < rows; ++r0) {
        if (stop.stop_requested()) {
            return filled;
        }
        if (progress && r0 % 64 == 0) {
            progress(static_cast<double>(r0) / rows);
        }
        for (int c0 = 0; c0 < cols; ++c0) {
            const std::size_t start = grid.index(c0, r0);
            if (!std::isnan(v[start]) || seen[start] != 0) {
                continue;
            }
            // Flood the gap (4-connected).
            hole.clear();
            queue.assign(1, start);
            seen[start] = 1;
            int minC = c0, maxC = c0, minR = r0, maxR = r0;
            bool border = false;
            while (!queue.empty()) {
                const std::size_t i = queue.back();
                queue.pop_back();
                hole.push_back(i);
                const int c = static_cast<int>(i % static_cast<std::size_t>(cols));
                const int r = static_cast<int>(i / static_cast<std::size_t>(cols));
                minC = std::min(minC, c);
                maxC = std::max(maxC, c);
                minR = std::min(minR, r);
                maxR = std::max(maxR, r);
                border = border || c == 0 || r == 0 || c == cols - 1 || r == rows - 1;
                const std::array<std::pair<int, int>, 4> next{
                    {{c - 1, r}, {c + 1, r}, {c, r - 1}, {c, r + 1}}};
                for (const auto& [nc, nr] : next) {
                    if (nc < 0 || nr < 0 || nc >= cols || nr >= rows) {
                        continue;
                    }
                    const std::size_t j = grid.index(nc, nr);
                    if (std::isnan(v[j]) && seen[j] == 0) {
                        seen[j] = 1;
                        queue.push_back(j);
                    }
                }
            }
            if (border || maxC - minC + 1 > maxCells || maxR - minR + 1 > maxCells) {
                continue;  // Outside the data, or too large: NoData.
            }
            // The rim: data cells around the gap (8-connected).
            rim.clear();
            for (const std::size_t i : hole) {
                const int c = static_cast<int>(i % static_cast<std::size_t>(cols));
                const int r = static_cast<int>(i / static_cast<std::size_t>(cols));
                for (int dr = -1; dr <= 1; ++dr) {
                    for (int dc = -1; dc <= 1; ++dc) {
                        const std::size_t j = grid.index(c + dc, r + dr);
                        if (!std::isnan(v[j]) && seen[j] != 2) {
                            seen[j] = 2;
                            rim.push_back(j);
                        }
                    }
                }
            }
            for (const std::size_t j : rim) {
                seen[j] = 0;
            }
            if (rim.size() < 3) {
                continue;
            }
            // A ragged gap can have a long rim; an even subset keeps the triangulation (and
            // IDW) cheap without changing a smooth surface much.
            if (rim.size() > kMaxRimPoints) {
                const std::size_t step = (rim.size() + kMaxRimPoints - 1) / kMaxRimPoints;
                std::size_t kept = 0;
                for (std::size_t j = 0; j < rim.size(); j += step) {
                    rim[kept++] = rim[j];
                }
                rim.resize(kept);
            }
            std::vector<Pt> pts;
            pts.reserve(rim.size() + 3);
            const auto colOf = [cols](std::size_t i) {
                return static_cast<int>(i % static_cast<std::size_t>(cols));
            };
            const auto rowOf = [cols](std::size_t i) {
                return static_cast<int>(i / static_cast<std::size_t>(cols));
            };
            for (const std::size_t j : rim) {
                pts.push_back({static_cast<double>(colOf(j) - minC),
                               static_cast<double>(rowOf(j) - minR), static_cast<double>(v[j])});
            }
            std::vector<float> out(hole.size(), kNaN);
            const auto idw = [&](double x, double y) {
                double sw = 0.0;
                double sz = 0.0;
                for (const Pt& p : pts) {
                    const double d2 = (p.x - x) * (p.x - x) + (p.y - y) * (p.y - y);
                    const double w = 1.0 / d2;
                    sw += w;
                    sz += w * p.z;
                }
                return static_cast<float>(sz / sw);
            };
            if (how == LidarInterpolation::Tin) {
                const std::vector<Tri> tris = delaunay(pts);
                // Rasterize each triangle over the gap's bounding box.
                const int w = maxC - minC + 1;
                const int h = maxR - minR + 1;
                std::vector<float> local(static_cast<std::size_t>(w) * static_cast<std::size_t>(h),
                                         kNaN);
                for (const Tri& t : tris) {
                    const Pt& a = pts[static_cast<std::size_t>(t.a)];
                    const Pt& b = pts[static_cast<std::size_t>(t.b)];
                    const Pt& c = pts[static_cast<std::size_t>(t.c)];
                    const double den = (b.y - c.y) * (a.x - c.x) + (c.x - b.x) * (a.y - c.y);
                    if (den == 0.0) {
                        continue;
                    }
                    const int x0 =
                        std::max(0, static_cast<int>(std::floor(std::min({a.x, b.x, c.x}))));
                    const int x1 =
                        std::min(w - 1, static_cast<int>(std::ceil(std::max({a.x, b.x, c.x}))));
                    const int y0 =
                        std::max(0, static_cast<int>(std::floor(std::min({a.y, b.y, c.y}))));
                    const int y1 =
                        std::min(h - 1, static_cast<int>(std::ceil(std::max({a.y, b.y, c.y}))));
                    for (int y = y0; y <= y1; ++y) {
                        for (int x = x0; x <= x1; ++x) {
                            const double l1 =
                                ((b.y - c.y) * (x - c.x) + (c.x - b.x) * (y - c.y)) / den;
                            const double l2 =
                                ((c.y - a.y) * (x - c.x) + (a.x - c.x) * (y - c.y)) / den;
                            const double l3 = 1.0 - l1 - l2;
                            if (l1 >= -1e-9 && l2 >= -1e-9 && l3 >= -1e-9) {
                                local[static_cast<std::size_t>(y) * static_cast<std::size_t>(w) +
                                      static_cast<std::size_t>(x)] =
                                    static_cast<float>(l1 * a.z + l2 * b.z + l3 * c.z);
                            }
                        }
                    }
                }
                for (std::size_t k = 0; k < hole.size(); ++k) {
                    const int c = colOf(hole[k]) - minC;
                    const int r = rowOf(hole[k]) - minR;
                    out[k] = local[static_cast<std::size_t>(r) * static_cast<std::size_t>(w) +
                                   static_cast<std::size_t>(c)];
                }
            }
            for (std::size_t k = 0; k < hole.size(); ++k) {
                if (std::isnan(out[k])) {  // IDW, or a cell no triangle covered.
                    out[k] = idw(static_cast<double>(colOf(hole[k]) - minC),
                                 static_cast<double>(rowOf(hole[k]) - minR));
                }
            }
            for (std::size_t k = 0; k < hole.size(); ++k) {
                v[hole[k]] = out[k];
            }
            filled += hole.size();
        }
    }
    return filled;
}

// --- COG output --------------------------------------------------------------------------------

struct CopyProgress {
    std::stop_token stop;
    const LidarProgress* progress;
    double from;
    double to;
};

int CPL_STDCALL onCopyProgress(double done, const char* /*message*/, void* arg) {
    const auto* p = static_cast<const CopyProgress*>(arg);
    if (p->progress != nullptr && *p->progress) {
        (*p->progress)(p->from + (p->to - p->from) * done);
    }
    return p->stop.stop_requested() ? FALSE : TRUE;
}

const char* metaKey(std::string_view name) {
    static thread_local std::string text;
    text = std::string("DS_LIDAR_") + std::string(name);
    return text.c_str();
}

}  // namespace

TerrainResult<LidarHeader> readLidarHeader(const fs::path& file) {
    const auto format = elevationFormatOf(file);
    if (!format || *format == ElevationFormat::Raster) {
        return std::unexpected(error(TerrainErrorCode::UnsupportedFormat,
                                     std::format("{}: not a .las or .laz file", file.string())));
    }
    Reader reader;
    if (auto ok = reader.open(file); !ok) {
        return std::unexpected(ok.error());
    }
    const laszip_header_struct& h = reader.header();
    LidarHeader out;
    out.file = file;
    out.version = std::format("{}.{}", h.version_major, h.version_minor);
    out.pointFormat = h.point_data_format;
    out.compressed = reader.compressed();
    out.points = reader.count();
    out.minX = h.min_x;
    out.minY = h.min_y;
    out.maxX = h.max_x;
    out.maxY = h.max_y;
    out.minZ = h.min_z;
    out.maxZ = h.max_z;
    readCrs(h, out);
    return out;
}

TerrainResult<LidarSummary> rasterizeLidar(std::span<const fs::path> files,
                                           const LidarOptions& options, const fs::path& output,
                                           std::stop_token stop, const LidarProgress& progress) {
    gdal::ensureInitialised();
    if (files.empty()) {
        return std::unexpected(error(TerrainErrorCode::InvalidSpec, "no point-cloud files"));
    }
    if (options.classifyGround) {
        log::info("lidar: classifyGround is not available yet; using the file's classes");
    }

    // Headers: one CRS, the bounds and the point count.
    std::vector<LidarHeader> headers;
    for (const fs::path& f : files) {
        auto h = readLidarHeader(f);
        if (!h) {
            return std::unexpected(h.error());
        }
        headers.push_back(std::move(*h));
    }
    std::string crs = headers.front().crs;
    for (const LidarHeader& h : headers) {
        if (h.crs != crs) {
            return std::unexpected(error(
                TerrainErrorCode::MixedCrs,
                std::format("{} and {} are in different CRSs; reproject them to one",
                            headers.front().file.filename().string(), h.file.filename().string())));
        }
    }
    if (crs.empty()) {
        if (options.crs.empty()) {
            return std::unexpected(error(TerrainErrorCode::MissingCrs,
                                         std::format("{} has no CRS; set crs (e.g. \"EPSG:25831\")",
                                                     headers.front().file.filename().string())));
        }
        crs = options.crs;
    }
    Srs srs;
    srs.h = OSRNewSpatialReference(nullptr);
    OSRSetAxisMappingStrategy(srs.h, OAMS_TRADITIONAL_GIS_ORDER);
    if (OSRSetFromUserInput(srs.h, crs.c_str()) != OGRERR_NONE) {
        return std::unexpected(error(TerrainErrorCode::MissingCrs,
                                     std::format("the CRS \"{}\" is not understood",
                                                 crs.size() > 80 ? crs.substr(0, 80) + "…" : crs)));
    }
    if (OSRIsProjected(srs.h) == 0) {
        return std::unexpected(
            error(TerrainErrorCode::NotSupported,
                  "point clouds in geographic coordinates are not supported; use a projected CRS "
                  "(e.g. UTM)"));
    }
    const double unitM = OSRGetLinearUnits(srs.h, nullptr);  // CRS unit → metres.
    const double zToM = headers.front().verticalToMetres.value_or(unitM);

    double minX = headers.front().minX;
    double minY = headers.front().minY;
    double maxX = headers.front().maxX;
    double maxY = headers.front().maxY;
    std::uint64_t total = 0;
    for (const LidarHeader& h : headers) {
        minX = std::min(minX, h.minX);
        minY = std::min(minY, h.minY);
        maxX = std::max(maxX, h.maxX);
        maxY = std::max(maxY, h.maxY);
        total += h.points;
    }
    if (total == 0 || !(maxX > minX) || !(maxY > minY)) {
        return std::unexpected(error(TerrainErrorCode::CorruptData, "the point clouds are empty"));
    }

    // Resolution (metres) and grid (CRS units).
    const double areaM2 = (maxX - minX) * (maxY - minY) * unitM * unitM;
    const auto cellsFor = [&](double resM) {
        const double res = resM / unitM;
        return std::ceil((maxX - minX) / res + 1.0) * std::ceil((maxY - minY) / res + 1.0);
    };
    double resM = 0.0;
    if (options.resolutionM) {
        resM = *options.resolutionM;
        if (resM < 0.25) {
            return std::unexpected(
                error(TerrainErrorCode::InvalidSpec, "resolutionM must be at least 0.25 m"));
        }
        if (cellsFor(resM) > static_cast<double>(options.maxCells)) {
            const double needed = std::sqrt(areaM2 / static_cast<double>(options.maxCells));
            return std::unexpected(error(
                TerrainErrorCode::TooLarge,
                std::format("a {:.2f} m grid of {:.1f} km² is too large; use resolutionM {:.2f} "
                            "or coarser, or fewer tiles",
                            resM, areaM2 / 1e6, std::ceil(needed * 4.0) / 4.0)));
        }
    } else {
        // About two point spacings, so most cells get a ground point.
        resM = std::clamp(
            std::ceil(2.0 * std::sqrt(areaM2 / static_cast<double>(total)) * 4.0) / 4.0, 0.5, 20.0);
        while (cellsFor(resM) > static_cast<double>(options.maxCells)) {
            resM += 0.25;
        }
    }
    const double res = resM / unitM;
    const double x0 = std::floor(minX / res) * res;
    const double y1 = std::ceil(maxY / res) * res;
    const int cols = std::max(1, static_cast<int>(std::ceil((maxX - x0) / res)));
    const int rows = std::max(1, static_cast<int>(std::ceil((y1 - minY) / res)));

    // Pass over the points (80 % of the progress).
    LidarSummary summary;
    summary.resolutionM = resM;
    summary.widthPx = cols;
    summary.heightPx = rows;
    summary.interpolation = options.interpolation == LidarInterpolation::Idw ? "idw" : "tin";
    const bool lowest = options.cellHeight == LidarCellHeight::Lowest;
    summary.cellHeight = lowest ? "lowest" : "mean";
    Grid grid(cols, rows);
    std::uint64_t done = 0;
    for (const LidarHeader& h : headers) {
        Reader reader;
        if (auto ok = reader.open(h.file); !ok) {
            return std::unexpected(ok.error());
        }
        const laszip_header_struct& hd = reader.header();
        const bool extended = hd.point_data_format >= 6;
        const laszip_point_struct& p = reader.point();
        for (std::uint64_t i = 0; i < reader.count(); ++i) {
            if (!reader.next()) {
                return std::unexpected(
                    error(TerrainErrorCode::CorruptData,
                          std::format("{}: cannot read point {} ({})", h.file.filename().string(),
                                      i, reader.lastError())));
            }
            if ((++done & 0xFFFFU) == 0) {
                if (stop.stop_requested()) {
                    return std::unexpected(error(TerrainErrorCode::Cancelled, "cancelled"));
                }
                if (progress) {
                    progress(0.8 * static_cast<double>(done) / static_cast<double>(total));
                }
            }
            ++summary.points;
            const int cls = extended ? p.extended_classification : p.classification;
            const bool withheld =
                p.withheld_flag != 0 || (extended && (p.extended_classification_flags & 0x4U) != 0);
            if (withheld || cls == 7 || cls == 18) {
                ++summary.dropped;
                continue;
            }
            if (cls != 2 && cls != 9) {
                continue;
            }
            const double x = p.X * hd.x_scale_factor + hd.x_offset;
            const double y = p.Y * hd.y_scale_factor + hd.y_offset;
            const double z = (p.Z * hd.z_scale_factor + hd.z_offset) * zToM;
            const int c = std::clamp(static_cast<int>((x - x0) / res), 0, cols - 1);
            const int r = std::clamp(static_cast<int>((y1 - y) / res), 0, rows - 1);
            grid.add(c, r, static_cast<float>(z), lowest);
            ++summary.groundPoints;
        }
    }
    if (summary.groundPoints == 0) {
        return std::unexpected(error(
            TerrainErrorCode::NotSupported,
            std::format("the point cloud has no ground points (class 2){}; classify it first, "
                        "e.g. with PDAL's SMRF filter or LAStools' lasground",
                        options.classifyGround
                            ? ", and automatic classification (classifyGround) is not available yet"
                            : "")));
    }
    grid.releaseCounts();

    // Gaps (10 %).
    const int maxHoleCells = std::max(1, static_cast<int>(options.maxHoleM / resM));
    summary.filledCells = fillHoles(grid, maxHoleCells, options.interpolation, stop, [&](double f) {
        if (progress) {
            progress(0.8 + 0.1 * f);
        }
    });
    if (stop.stop_requested()) {
        return std::unexpected(error(TerrainErrorCode::Cancelled, "cancelled"));
    }
    std::vector<float>& values = grid.values();
    for (float& v : values) {
        if (std::isnan(v)) {
            v = static_cast<float>(kNoData);
            ++summary.noDataCells;
        }
    }
    char* wkt = nullptr;
    OSRExportToWkt(srs.h, &wkt);
    summary.crs = crs.starts_with("EPSG:") ? crs : (OSRGetName(srs.h) ? OSRGetName(srs.h) : "WKT");

    // COG (10 %), written beside the target and renamed when complete.
    GDALDatasetH mem =
        GDALCreate(GDALGetDriverByName("MEM"), "", cols, rows, 1, GDT_Float32, nullptr);
    if (mem == nullptr) {
        CPLFree(wkt);
        return std::unexpected(error(TerrainErrorCode::TooLarge, "not enough memory for the DTM"));
    }
    std::array<double, 6> gt{x0, res, 0.0, y1, 0.0, -res};
    GDALSetGeoTransform(mem, gt.data());
    GDALSetProjection(mem, wkt);
    CPLFree(wkt);
    GDALRasterBandH band = GDALGetRasterBand(mem, 1);
    GDALSetRasterNoDataValue(band, kNoData);
    GDALSetRasterUnitType(band, "m");
    const CPLErr written = GDALRasterIO(band, GF_Write, 0, 0, cols, rows, values.data(), cols, rows,
                                        GDT_Float32, 0, 0);
    std::vector<float>().swap(values);
    const auto put = [&](std::string_view name, const std::string& value) {
        GDALSetMetadataItem(mem, metaKey(name), value.c_str(), nullptr);
    };
    put("POINTS", std::to_string(summary.points));
    put("GROUND", std::to_string(summary.groundPoints));
    put("DROPPED", std::to_string(summary.dropped));
    put("RESOLUTION_M", std::format("{}", summary.resolutionM));
    put("FILLED", std::to_string(summary.filledCells));
    put("NODATA", std::to_string(summary.noDataCells));
    put("CRS", summary.crs);
    put("INTERPOLATION", summary.interpolation);
    put("CELL_HEIGHT", summary.cellHeight);
    if (written != CE_None) {
        GDALClose(mem);
        return std::unexpected(error(TerrainErrorCode::CorruptData, gdal::lastError()));
    }
    std::error_code ec;
    fs::create_directories(output.parent_path(), ec);
    const fs::path partial = output.parent_path() /
                             std::format(".{}.{}.partial", output.filename().string(), ::getpid());
    char** cog = nullptr;
    cog = CSLSetNameValue(cog, "COMPRESS", "DEFLATE");
    cog = CSLSetNameValue(cog, "PREDICTOR", "YES");
    cog = CSLSetNameValue(cog, "BLOCKSIZE", "256");
    cog = CSLSetNameValue(cog, "OVERVIEWS", "AUTO");
    cog = CSLSetNameValue(cog, "RESAMPLING", "AVERAGE");
    CopyProgress cp{stop, &progress, 0.9, 1.0};
    GDALDatasetH out = GDALCreateCopy(GDALGetDriverByName("COG"), partial.c_str(), mem, FALSE, cog,
                                      onCopyProgress, &cp);
    CSLDestroy(cog);
    GDALClose(mem);
    const bool ok = out != nullptr;
    if (out != nullptr) {
        GDALClose(out);
    }
    if (!ok || stop.stop_requested()) {
        fs::remove(partial, ec);
        if (stop.stop_requested()) {
            return std::unexpected(error(TerrainErrorCode::Cancelled, "cancelled"));
        }
        return std::unexpected(error(TerrainErrorCode::CorruptData,
                                     std::format("cannot write the DTM: {}", gdal::lastError())));
    }
    fs::rename(partial, output, ec);
    if (ec) {
        fs::remove(partial, ec);
        return std::unexpected(error(TerrainErrorCode::CorruptData,
                                     std::format("cannot store the DTM: {}", ec.message())));
    }
    if (progress) {
        progress(1.0);
    }
    log::info(
        "lidar: {} points ({} ground, {} dropped) → {} × {} DTM at {:.2f} m, {} cells "
        "filled, {} NoData",
        summary.points, summary.groundPoints, summary.dropped, cols, rows, resM,
        summary.filledCells, summary.noDataCells);
    return summary;
}

TerrainResult<std::string> lidarDemKey(std::span<const fs::path> files,
                                       const LidarOptions& options) {
    // Bump the version when the rasterization changes, so older DTMs are rebuilt.
    std::string text = "ds-lidar-dtm 1\n";
    for (const fs::path& f : files) {
        std::error_code ec;
        const auto size = fs::file_size(f, ec);
        const auto time = fs::last_write_time(f, ec);
        if (ec) {
            return std::unexpected(
                error(TerrainErrorCode::FileNotFound, std::format("{}: not found", f.string())));
        }
        text += std::format("{}\t{}\t{}\n", fs::weakly_canonical(f, ec).string(), size,
                            time.time_since_epoch().count());
    }
    text += std::format("res={} interp={} crs={} classify={} hole={}\n",
                        options.resolutionM ? std::format("{}", *options.resolutionM) : "auto",
                        options.interpolation == LidarInterpolation::Idw ? "idw" : "tin",
                        options.crs, options.classifyGround ? 1 : 0, options.maxHoleM);
    if (options.cellHeight == LidarCellHeight::Lowest) {
        text += "cell=lowest\n";  // Means keep the keys of earlier DTMs.
    }
    return common::sha256Hex(text);
}

TerrainResult<LidarSummary> readLidarSummary(const fs::path& cog) {
    gdal::ensureInitialised();
    gdal::DatasetPtr ds = gdal::openRaster(cog.string(), gdal::kElevationDrivers);
    if (!ds) {
        return std::unexpected(error(TerrainErrorCode::FileNotFound,
                                     std::format("{}: {}", cog.string(), gdal::lastError())));
    }
    const auto get = [&](std::string_view name) -> std::string {
        const char* v = GDALGetMetadataItem(ds.get(), metaKey(name), nullptr);
        return v != nullptr ? v : "";
    };
    if (get("POINTS").empty()) {
        return std::unexpected(
            error(TerrainErrorCode::CorruptData, "not a DTM made from a point cloud"));
    }
    const auto u64 = [&](std::string_view name) {
        return static_cast<std::uint64_t>(std::strtoull(get(name).c_str(), nullptr, 10));
    };
    LidarSummary s;
    s.points = u64("POINTS");
    s.groundPoints = u64("GROUND");
    s.dropped = u64("DROPPED");
    s.resolutionM = CPLAtof(get("RESOLUTION_M").c_str());
    s.filledCells = u64("FILLED");
    s.noDataCells = u64("NODATA");
    s.crs = get("CRS");
    s.interpolation = get("INTERPOLATION");
    if (const std::string cell = get("CELL_HEIGHT"); !cell.empty()) {
        s.cellHeight = cell;  // DTMs made before the option hold means.
    }
    s.widthPx = GDALGetRasterXSize(ds.get());
    s.heightPx = GDALGetRasterYSize(ds.get());
    return s;
}

}  // namespace ds::terrain
