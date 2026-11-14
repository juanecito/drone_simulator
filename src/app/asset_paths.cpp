// SPDX-FileCopyrightText: 2026 Juan Maria Gomez Lopez <juan-maria.gomez-lopez@tutamail.com>
// SPDX-License-Identifier: MIT

#include "ds/app/asset_paths.hpp"

#include <QCoreApplication>

#include <cstdlib>
#include <system_error>

#include "ds/common/log.hpp"

namespace ds::app {
namespace {

bool looksLikeAssets(const std::filesystem::path& dir) {
    std::error_code ec;
    return std::filesystem::is_directory(dir / "shaders", ec) &&
           std::filesystem::is_directory(dir / "sky", ec);
}

}  // namespace

std::filesystem::path findAssetDirectory() {
    if (const char* env = std::getenv("DS_ASSETS_DIR"); env != nullptr && *env != '\0') {
        if (looksLikeAssets(env)) {
            return env;
        }
        log::warn("DS_ASSETS_DIR={} has no shaders/ and sky/; ignoring it", env);
    }
    if (QCoreApplication::instance() != nullptr) {
        const std::filesystem::path installed =
            std::filesystem::path(QCoreApplication::applicationDirPath().toStdString()) / ".." /
            "share";
        if (looksLikeAssets(installed)) {
            return installed.lexically_normal();
        }
    }
    return DS_SOURCE_ASSETS_DIR;
}

}  // namespace ds::app
