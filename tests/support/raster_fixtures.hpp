// SPDX-FileCopyrightText: 2026 Juan Maria Gomez Lopez <juan-maria.gomez-lopez@tutamail.com>
// SPDX-License-Identifier: MIT

#pragma once

// Elevation raster fixtures written with GDAL inside the tests (SPECIFICATION §9: fixtures
// are generated in-test, never downloaded). Shared by the unit and integration tests.

#include <gtest/gtest.h>

#include <array>
#include <filesystem>
#include <format>
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include <cpl_conv.h>
#include <gdal.h>
#include <ogr_srs_api.h>
#include <unistd.h>

namespace ds::test {

// UTM zone 30N tile near Madrid, 10 m pixels.
inline constexpr double kE0 = 440'000.0;
inline constexpr double kN0 = 4'476'000.0;
inline constexpr int kUtmEpsg = 32630;

struct RasterSpec {
    int width = 200;
    int height = 200;
    std::array<double, 6> geoTransform{kE0, 10.0, 0.0, kN0, 0.0, -10.0};
    int epsg = kUtmEpsg;  // 0 = no CRS.
    GDALDataType type = GDT_Float32;
    std::optional<double> noData;
    double scale = 1.0;
    double offset = 0.0;
    std::string unit;
    std::string driver = "GTiff";
    std::vector<std::string> creationOptions;
    bool overviews = false;
    // Raw pixel value at column i, row j.
    std::function<double(int i, int j)> value;
};

class TempDir {
public:
    explicit TempDir(const std::string& name = "raster")
        : path_(std::filesystem::temp_directory_path() /
                std::format("ds_{}_{}", name, ::getpid())) {
        std::filesystem::remove_all(path_);
        std::filesystem::create_directories(path_);
    }
    ~TempDir() { std::filesystem::remove_all(path_); }
    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;
    [[nodiscard]] std::filesystem::path operator/(const std::string& name) const {
        return path_ / name;
    }
    [[nodiscard]] const std::filesystem::path& path() const { return path_; }

private:
    std::filesystem::path path_;
};

inline void writeRaster(const std::filesystem::path& file, const RasterSpec& spec) {
    GDALAllRegister();
    GDALDriverH mem = GDALGetDriverByName("MEM");
    GDALDatasetH ds = GDALCreate(mem, "", spec.width, spec.height, 1, spec.type, nullptr);
    ASSERT_NE(ds, nullptr);
    auto gt = spec.geoTransform;
    GDALSetGeoTransform(ds, gt.data());
    if (spec.epsg != 0) {
        OGRSpatialReferenceH srs = OSRNewSpatialReference(nullptr);
        OSRImportFromEPSG(srs, spec.epsg);
        GDALSetSpatialRef(ds, srs);
        OSRDestroySpatialReference(srs);
    }
    GDALRasterBandH band = GDALGetRasterBand(ds, 1);
    if (spec.noData) {
        GDALSetRasterNoDataValue(band, *spec.noData);
    }
    if (spec.scale != 1.0 || spec.offset != 0.0) {
        GDALSetRasterScale(band, spec.scale);
        GDALSetRasterOffset(band, spec.offset);
    }
    if (!spec.unit.empty()) {
        GDALSetRasterUnitType(band, spec.unit.c_str());
    }
    std::vector<double> row(static_cast<std::size_t>(spec.width));
    for (int j = 0; j < spec.height; ++j) {
        for (int i = 0; i < spec.width; ++i) {
            row[static_cast<std::size_t>(i)] = spec.value(i, j);
        }
        ASSERT_EQ(GDALRasterIO(band, GF_Write, 0, j, spec.width, 1, row.data(), spec.width, 1,
                               GDT_Float64, 0, 0),
                  CE_None);
    }
    if (spec.overviews) {
        const std::array<int, 3> levels{2, 4, 8};
        ASSERT_EQ(GDALBuildOverviews(ds, "AVERAGE", 3, levels.data(), 0, nullptr, nullptr, nullptr),
                  CE_None);
    }
    std::vector<const char*> options;
    for (const auto& o : spec.creationOptions) {
        options.push_back(o.c_str());
    }
    options.push_back(nullptr);
    GDALDatasetH out =
        GDALCreateCopy(GDALGetDriverByName(spec.driver.c_str()), file.string().c_str(), ds, FALSE,
                       const_cast<char**>(options.data()), nullptr, nullptr);
    ASSERT_NE(out, nullptr) << CPLGetLastErrorMsg();
    GDALClose(out);
    GDALClose(ds);
}

// Plane in UTM: height at easting / northing.
inline double planeUtm(double e, double n) { return 600.0 + 0.01 * (e - kE0) + 0.02 * (n - kN0); }

// 2 km × 2 km of that plane, 10 m pixels.
inline RasterSpec planarUtmSpec() {
    RasterSpec s;
    s.value = [](int i, int j) { return planeUtm(kE0 + (i + 0.5) * 10.0, kN0 - (j + 0.5) * 10.0); };
    return s;
}

}  // namespace ds::test
