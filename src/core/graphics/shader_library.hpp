// SPDX-FileCopyrightText: 2026 Juan Maria Gomez Lopez <juan-maria.gomez-lopez@tutamail.com>
// SPDX-License-Identifier: MIT

#pragma once

/// @file
/// @brief Private header: builds GLSL programs from `assets/shaders` and rebuilds them when
/// their files change (FR-GFX-04).
/// @ingroup graphics

#include <filesystem>
#include <string>
#include <vector>

#include "ds/common/error.hpp"

#include "gl_objects.hpp"

namespace ds::graphics {

/// @brief The renderer's shader programs, with error reporting and hot reload.
class ShaderLibrary {
public:
    /// @brief One shader stage of a program.
    struct Stage {
        GLenum type;  ///< `GL_VERTEX_SHADER`, `GL_FRAGMENT_SHADER`, `GL_COMPUTE_SHADER`.
        std::filesystem::path file;  ///< File relative to the shader directory.
    };

    /// @brief Loads shaders from @p directory.
    explicit ShaderLibrary(std::filesystem::path directory) : directory_(std::move(directory)) {}

    /// @brief Registers a program; returns its index. Build with buildAll().
    std::size_t add(std::string name, std::vector<Stage> stages);
    /// @brief Builds every registered program; fails on the first error (message has file:line).
    [[nodiscard]] common::Result<void> buildAll();
    /// @brief Rebuilds programs whose source files changed. On error the previous program
    /// stays in use and the error is returned. Returns true if anything was rebuilt.
    [[nodiscard]] common::Result<bool> reloadChanged();
    /// @brief Program object of the program registered as @p index.
    [[nodiscard]] GLuint program(std::size_t index) const noexcept {
        return entries_[index].program.id();
    }
    /// @brief The shader directory.
    [[nodiscard]] const std::filesystem::path& directory() const noexcept { return directory_; }

private:
    struct Entry {
        std::string name;
        std::vector<Stage> stages;
        gl::Program program;
        std::vector<std::filesystem::path> files;
        std::filesystem::file_time_type newest{};
    };
    [[nodiscard]] common::Result<void> build(Entry& entry) const;
    [[nodiscard]] std::filesystem::file_time_type newestTime(const Entry& entry) const;

    std::filesystem::path directory_;
    std::vector<Entry> entries_;
};

}  // namespace ds::graphics
