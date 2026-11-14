// SPDX-FileCopyrightText: 2026 Juan Maria Gomez Lopez <juan-maria.gomez-lopez@tutamail.com>
// SPDX-License-Identifier: MIT

#pragma once

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include "ds/common/error.hpp"

/// @file
/// @brief GLSL source loading with `#include` and error-log mapping (FR-GFX-04).
/// @ingroup graphics

namespace ds::graphics {

/// @brief A preprocessed GLSL source ready for `glShaderSource`.
struct ShaderSource {
    std::string text;  ///< Full source with `#line` directives.
    /// Files that make up the source; the index is the source-string number used in the
    /// `#line` directives and in driver error messages.
    std::vector<std::filesystem::path> files;
};

/// @brief Loads @p file (relative to @p directory) and expands `#include "name"` lines.
/// @details The first line must be the `#version` directive; @p defines (e.g. `"MSAA 4"`)
/// are inserted after it as `#define` lines. Included files are resolved relative to
/// @p directory, may not leave it, and may be included only once (a second include is
/// skipped). `#line <n> <file-index>` directives keep driver error positions exact.
/// @return The source, or an error for a missing file, a bad include or a missing
/// `#version` line.
[[nodiscard]] common::Result<ShaderSource> loadShaderSource(
    const std::filesystem::path& directory, const std::filesystem::path& file,
    const std::vector<std::string>& defines = {});

/// @brief Rewrites positions in a driver info log ("0(12)" for NVIDIA, "0:12(5)" for Mesa)
/// into "file.glsl:12" using @p files.
[[nodiscard]] std::string mapShaderLog(std::string_view log,
                                       const std::vector<std::filesystem::path>& files);

}  // namespace ds::graphics
