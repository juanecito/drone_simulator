// SPDX-FileCopyrightText: 2026 Juan Maria Gomez Lopez <juan-maria.gomez-lopez@tutamail.com>
// SPDX-License-Identifier: MIT

#pragma once

#include <filesystem>

/// @file
/// @brief Test helper: a copy of the assets without the built-in mission pack, for tests about
/// the user's mission folder that must not see the pack's missions.

namespace ds::test {

/// @brief Copies DS_TEST_ASSETS_DIR to @p dir / "assets" without `missions/*.mission.json`
/// (the schema stays) and returns the copy's path.
inline std::filesystem::path assetsWithoutMissionPack(const std::filesystem::path& dir) {
    namespace fs = std::filesystem;
    const fs::path copy = dir / "assets";
    fs::copy(DS_TEST_ASSETS_DIR, copy, fs::copy_options::recursive);
    for (const auto& e : fs::directory_iterator(copy / "missions")) {
        if (e.path().filename().string().ends_with(".mission.json")) {
            fs::remove(e.path());
        }
    }
    return copy;
}

}  // namespace ds::test
