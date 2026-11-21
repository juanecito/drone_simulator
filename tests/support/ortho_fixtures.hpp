// SPDX-FileCopyrightText: 2026 Juan Maria Gomez Lopez <juan-maria.gomez-lopez@tutamail.com>
// SPDX-License-Identifier: MIT

#pragma once

// Orthophoto fixtures written with GDAL inside the tests (SPECIFICATION §9): 8-bit RGB / RGBA
// images whose colours encode their pixel position, as GeoTIFF, COG, JPEG 2000 or PNG with a
// world file.

#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "raster_fixtures.hpp"
#include <cpl_conv.h>
#include <gdal.h>
#include <ogr_srs_api.h>

namespace ds::test {

struct ImageSpec {
    int width = 400;
    int height = 400;
    // UTM 30N near Madrid, 0.5 m pixels.
    std::array<double, 6> geoTransform{kE0, 0.5, 0.0, kN0, 0.0, -0.5};
    int epsg = kUtmEpsg;  // 0 = no CRS.
    int bands = 3;
    bool alpha = false;  // The last band is alpha.
    GDALDataType type = GDT_Byte;
    std::optional<double> noData;
    bool palette = false;
    std::string driver = "GTiff";
    std::vector<std::string> creationOptions;
    bool overviews = false;
    bool worldFile = false;  // Write <stem>.<ext>w beside a PNG/JPEG (and no CRS).
    // Value of band b (0-based) at column i, row j.
    std::function<int(int i, int j, int b)> value = [](int i, int j, int b) {
        // Red = column, green = row (modulo 256), blue = which 256 block.
        return b == 0 ? i % 256 : b == 1 ? j % 256 : b == 2 ? (i / 256) * 16 + j / 256 : 255;
    };
};

inline void writeImage(const std::filesystem::path& file, const ImageSpec& spec) {
    GDALAllRegister();
    GDALDatasetH ds = GDALCreate(GDALGetDriverByName("MEM"), "", spec.width, spec.height,
                                 spec.bands, spec.type, nullptr);
    ASSERT_NE(ds, nullptr);
    auto gt = spec.geoTransform;
    GDALSetGeoTransform(ds, gt.data());
    if (spec.epsg != 0) {
        OGRSpatialReferenceH srs = OSRNewSpatialReference(nullptr);
        OSRImportFromEPSG(srs, spec.epsg);
        GDALSetSpatialRef(ds, srs);
        OSRDestroySpatialReference(srs);
    }
    std::vector<double> row(static_cast<std::size_t>(spec.width));
    for (int b = 0; b < spec.bands; ++b) {
        GDALRasterBandH band = GDALGetRasterBand(ds, b + 1);
        if (spec.alpha && b == spec.bands - 1) {
            GDALSetRasterColorInterpretation(band, GCI_AlphaBand);
        }
        if (spec.noData) {
            GDALSetRasterNoDataValue(band, *spec.noData);
        }
        if (spec.palette && b == 0) {
            GDALColorTableH table = GDALCreateColorTable(GPI_RGB);
            GDALColorEntry e{10, 20, 30, 255};
            GDALSetColorEntry(table, 0, &e);
            GDALSetRasterColorTable(band, table);
            GDALDestroyColorTable(table);
        }
        for (int j = 0; j < spec.height; ++j) {
            for (int i = 0; i < spec.width; ++i) {
                row[static_cast<std::size_t>(i)] = spec.value(i, j, b);
            }
            ASSERT_EQ(GDALRasterIO(band, GF_Write, 0, j, spec.width, 1, row.data(), spec.width, 1,
                                   GDT_Float64, 0, 0),
                      CE_None);
        }
    }
    if (spec.overviews) {
        const std::array<int, 3> levels{2, 4, 8};
        ASSERT_EQ(GDALBuildOverviews(ds, "AVERAGE", 3, levels.data(), 0, nullptr, nullptr, nullptr),
                  CE_None);
    }
    std::vector<const char*> options;
    if (spec.overviews && spec.driver == "GTiff") {
        options.push_back("COPY_SRC_OVERVIEWS=YES");
    }
    for (const auto& o : spec.creationOptions) {
        options.push_back(o.c_str());
    }
    options.push_back(nullptr);
    CPLSetConfigOption("GDAL_PAM_ENABLED", "NO");
    GDALDatasetH out =
        GDALCreateCopy(GDALGetDriverByName(spec.driver.c_str()), file.string().c_str(), ds, FALSE,
                       const_cast<char**>(options.data()), nullptr, nullptr);
    ASSERT_NE(out, nullptr) << CPLGetLastErrorMsg();
    GDALClose(out);
    GDALClose(ds);
    if (spec.worldFile) {
        std::string ext = file.extension().string();  // ".png" ⇒ ".pgw"
        const std::string world = std::string(".") + ext[1] + ext.back() + "w";
        std::filesystem::path wf = file;
        wf.replace_extension(world);
        std::ofstream w(wf);
        // Centre of the top-left pixel.
        w << std::format("{}\n{}\n{}\n{}\n{}\n{}\n", gt[1], gt[4], gt[2], gt[5],
                         gt[0] + 0.5 * gt[1] + 0.5 * gt[2], gt[3] + 0.5 * gt[4] + 0.5 * gt[5]);
    }
}

}  // namespace ds::test
