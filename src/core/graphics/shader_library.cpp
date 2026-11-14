// SPDX-FileCopyrightText: 2026 Juan Maria Gomez Lopez <juan-maria.gomez-lopez@tutamail.com>
// SPDX-License-Identifier: MIT

#include "shader_library.hpp"

#include <algorithm>
#include <format>
#include <system_error>

#include "ds/common/log.hpp"
#include "ds/graphics/shader_source.hpp"

namespace ds::graphics {
namespace {

std::string infoLog(GLuint object, bool program) {
    GLint length = 0;
    if (program) {
        glGetProgramiv(object, GL_INFO_LOG_LENGTH, &length);
    } else {
        glGetShaderiv(object, GL_INFO_LOG_LENGTH, &length);
    }
    std::string log(static_cast<std::size_t>(std::max(length, 1)), '\0');
    if (program) {
        glGetProgramInfoLog(object, length, nullptr, log.data());
    } else {
        glGetShaderInfoLog(object, length, nullptr, log.data());
    }
    while (!log.empty() && (log.back() == '\0' || log.back() == '\n')) {
        log.pop_back();
    }
    return log;
}

}  // namespace

std::size_t ShaderLibrary::add(std::string name, std::vector<Stage> stages) {
    entries_.push_back(Entry{.name = std::move(name), .stages = std::move(stages)});
    return entries_.size() - 1;
}

common::Result<void> ShaderLibrary::build(Entry& entry) const {
    gl::Program program(glCreateProgram());
    std::vector<gl::Shader> shaders;
    std::vector<std::filesystem::path> files;
    for (const Stage& stage : entry.stages) {
        auto source = loadShaderSource(directory_, stage.file);
        if (!source) {
            return std::unexpected(source.error());
        }
        gl::Shader shader(glCreateShader(stage.type));
        const char* text = source->text.c_str();
        glShaderSource(shader.id(), 1, &text, nullptr);
        glCompileShader(shader.id());
        GLint ok = GL_FALSE;
        glGetShaderiv(shader.id(), GL_COMPILE_STATUS, &ok);
        if (ok != GL_TRUE) {
            return common::makeError(
                std::format("Shader {} failed to compile:\n{}", stage.file.generic_string(),
                            mapShaderLog(infoLog(shader.id(), false), source->files)));
        }
        glAttachShader(program.id(), shader.id());
        files.insert(files.end(), source->files.begin(), source->files.end());
        shaders.push_back(std::move(shader));
    }
    glLinkProgram(program.id());
    GLint ok = GL_FALSE;
    glGetProgramiv(program.id(), GL_LINK_STATUS, &ok);
    for (const gl::Shader& shader : shaders) {
        glDetachShader(program.id(), shader.id());
    }
    if (ok != GL_TRUE) {
        return common::makeError(std::format("Shader program {} failed to link:\n{}", entry.name,
                                             infoLog(program.id(), true)));
    }
    std::ranges::sort(files);
    files.erase(std::unique(files.begin(), files.end()), files.end());
    entry.program = std::move(program);
    entry.files = std::move(files);
    entry.newest = newestTime(entry);
    return {};
}

std::filesystem::file_time_type ShaderLibrary::newestTime(const Entry& entry) const {
    std::filesystem::file_time_type newest{};
    for (const auto& file : entry.files) {
        std::error_code ec;
        const auto time = std::filesystem::last_write_time(directory_ / file, ec);
        if (!ec) {
            newest = std::max(newest, time);
        }
    }
    return newest;
}

common::Result<void> ShaderLibrary::buildAll() {
    for (Entry& entry : entries_) {
        if (auto result = build(entry); !result) {
            return result;
        }
    }
    log::debug("Built {} shader programs from {}", entries_.size(), directory_.string());
    return {};
}

common::Result<bool> ShaderLibrary::reloadChanged() {
    bool rebuilt = false;
    for (Entry& entry : entries_) {
        if (newestTime(entry) <= entry.newest) {
            continue;
        }
        if (auto result = build(entry); !result) {
            entry.newest = newestTime(entry);  // Don't retry until the file changes again.
            return std::unexpected(result.error());
        }
        log::info("Reloaded shader program {}", entry.name);
        rebuilt = true;
    }
    return rebuilt;
}

}  // namespace ds::graphics
