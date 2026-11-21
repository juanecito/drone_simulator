// SPDX-FileCopyrightText: 2026 Juan Maria Gomez Lopez <juan-maria.gomez-lopez@tutamail.com>
// SPDX-License-Identifier: MIT

#include "gdal_support.hpp"

#include <array>
#include <atomic>
#include <format>
#include <mutex>
#include <string_view>
#include <vector>

#include "ds/common/log.hpp"

#include <cpl_conv.h>
#include <cpl_error.h>

namespace ds::terrain::gdal {
namespace {

// Drivers kept after GDALAllRegister(): elevation rasters (FR-TER-F01), orthophotos
// (FR-TER-I01), and the in-memory / virtual / COG drivers the module itself uses.
constexpr std::array<std::string_view, 11> kKeptDrivers{"GTiff",   "COG",     "VRT", "MEM",
                                                        "SRTMHGT", "AAIGrid", "HFA", "JP2OpenJPEG",
                                                        "PNG",     "JPEG",    "WEBP"};

void CPL_STDCALL logGdalError(CPLErr severity, CPLErrorNum code, const char* message) {
    if (severity >= CE_Warning) {
        ds::log::debug("GDAL {} ({}): {}", static_cast<int>(severity), static_cast<int>(code),
                       message != nullptr ? message : "");
    }
}

}  // namespace

void ensureInitialised() {
    static std::once_flag once;
    std::call_once(once, [] {
        // No .aux.xml next to mission files, no network access, no directory listings
        // of mission folders on open.
        CPLSetConfigOption("GDAL_PAM_ENABLED", "NO");
        CPLSetConfigOption("GDAL_DISABLE_READDIR_ON_OPEN", "EMPTY_DIR");
        CPLSetConfigOption("CPL_VSIL_CURL_ALLOWED_EXTENSIONS", ".ds-network-disabled");
        CPLSetConfigOption("GDAL_HTTP_MAX_RETRY", "0");
        // A VRT may only read files: no nested derived / Python / network sources.
        CPLSetConfigOption("GDAL_VRT_ENABLE_PYTHON", "NO");
        CPLSetConfigOption("GDAL_VRT_ENABLE_RAWRASTERBAND", "NO");
        // Raster block cache, bounded (FR-TER-C02).
        GDALSetCacheMax64(std::int64_t{256} << 20U);
        CPLSetErrorHandler(logGdalError);

        GDALAllRegister();
        for (int i = GDALGetDriverCount() - 1; i >= 0; --i) {
            GDALDriverH driver = GDALGetDriver(i);
            const std::string_view name = GDALGetDriverShortName(driver);
            bool keep = false;
            for (const std::string_view kept : kKeptDrivers) {
                keep = keep || kept == name;
            }
            if (!keep) {
                GDALDeregisterDriver(driver);
                GDALDestroyDriver(driver);
            }
        }
    });
}

void DatasetCloser::operator()(void* dataset) const noexcept {
    if (dataset != nullptr) {
        GDALClose(static_cast<GDALDatasetH>(dataset));
    }
}

DatasetPtr openRaster(const std::string& path, std::span<const char* const> drivers) {
    return openRaster(path, drivers, {});
}

DatasetPtr openRaster(const std::string& path, std::span<const char* const> drivers,
                      std::span<const std::string> siblings) {
    ensureInitialised();
    if (path.starts_with("/vsicurl") || path.starts_with("/vsis3") || path.starts_with("/vsigs") ||
        path.starts_with("/vsiaz") || path.starts_with("/vsihttp")) {
        CPLError(CE_Failure, CPLE_AppDefined, "network paths are not allowed");
        return DatasetPtr{};
    }
    clearError();
    std::vector<const char*> names;
    for (const std::string& s : siblings) {
        names.push_back(s.c_str());
    }
    names.push_back(nullptr);
    return DatasetPtr{GDALOpenEx(path.c_str(), GDAL_OF_RASTER | GDAL_OF_READONLY, drivers.data(),
                                 nullptr, siblings.empty() ? nullptr : names.data())};
}

std::string lastError() {
    const char* message = CPLGetLastErrorMsg();
    return message != nullptr ? std::string(message) : std::string();
}

void clearError() { CPLErrorReset(); }

std::string uniqueMemPath(std::string_view stem, std::string_view extension) {
    static std::atomic<unsigned long long> counter{0};
    return std::format("/vsimem/ds_{}_{}{}", stem, counter.fetch_add(1), extension);
}

}  // namespace ds::terrain::gdal
