// SPDX-FileCopyrightText: 2026 Juan Maria Gomez Lopez <juan-maria.gomez-lopez@tutamail.com>
// SPDX-License-Identifier: MIT

#include "ds/graphics/shader_source.hpp"

#include <algorithm>
#include <format>
#include <fstream>
#include <regex>
#include <sstream>

namespace ds::graphics {
namespace {

constexpr int kMaxIncludeDepth = 16;

struct Loader {
    std::filesystem::path root;
    ShaderSource result;

    common::Result<std::string> read(const std::filesystem::path& relative) {
        const std::filesystem::path full = (root / relative).lexically_normal();
        const auto rel = full.lexically_relative(root);
        if (rel.empty() || rel.begin()->string() == "..") {
            return common::makeError(std::format(
                "shader include '{}' is outside the shader directory", relative.string()));
        }
        std::ifstream in(full, std::ios::binary);
        if (!in) {
            return common::makeError(std::format("cannot read shader file '{}'", full.string()));
        }
        std::ostringstream text;
        text << in.rdbuf();
        return text.str();
    }

    common::Result<void> expand(const std::filesystem::path& relative, const std::string& text,
                                int depth, std::string& out, bool isMain,
                                const std::vector<std::string>& defines) {
        if (depth > kMaxIncludeDepth) {
            return common::makeError("shader includes are nested too deeply");
        }
        const auto index = result.files.size();
        result.files.push_back(relative);
        std::istringstream lines(text);
        std::string line;
        int number = 0;
        bool versionSeen = false;
        static const std::regex kInclude(R"re(^\s*#\s*include\s+"([^"]+)"\s*$)re");
        while (std::getline(lines, line)) {
            ++number;
            if (!line.empty() && line.back() == '\r') {
                line.pop_back();
            }
            if (isMain && !versionSeen) {
                if (line.find_first_not_of(" \t") == std::string::npos || line.starts_with("//")) {
                    out += line + '\n';
                    continue;
                }
                if (!line.starts_with("#version")) {
                    return common::makeError(std::format(
                        "{}:{}: the first directive must be #version", relative.string(), number));
                }
                versionSeen = true;
                out += line + '\n';
                for (const std::string& define : defines) {
                    out += "#define " + define + '\n';
                }
                out += std::format("#line {} {}\n", number + 1, index);
                continue;
            }
            std::smatch match;
            if (std::regex_match(line, match, kInclude)) {
                const std::filesystem::path include = match[1].str();
                const bool already = std::ranges::find(result.files, include) != result.files.end();
                if (!already) {
                    auto content = read(include);
                    if (!content) {
                        return common::makeError(std::format("{}:{}: {}", relative.string(), number,
                                                             content.error().message));
                    }
                    out += std::format("#line 1 {}\n", result.files.size());
                    if (auto ok = expand(include, *content, depth + 1, out, false, defines); !ok) {
                        return ok;
                    }
                }
                out += std::format("#line {} {}\n", number + 1, index);
                continue;
            }
            out += line + '\n';
        }
        if (isMain && !versionSeen) {
            return common::makeError(std::format("{}: missing #version", relative.string()));
        }
        return {};
    }
};

}  // namespace

common::Result<ShaderSource> loadShaderSource(const std::filesystem::path& directory,
                                              const std::filesystem::path& file,
                                              const std::vector<std::string>& defines) {
    Loader loader{directory.lexically_normal(), {}};
    auto text = loader.read(file);
    if (!text) {
        return std::unexpected(text.error());
    }
    std::string out;
    if (auto ok = loader.expand(file, *text, 0, out, true, defines); !ok) {
        return std::unexpected(ok.error());
    }
    loader.result.text = std::move(out);
    return std::move(loader.result);
}

std::string mapShaderLog(std::string_view log, const std::vector<std::filesystem::path>& files) {
    // NVIDIA: "0(12) : error ..."; Mesa: "0:12(5): error: ..."; AMD: "ERROR: 0:12: ...".
    static const std::regex kPosition(R"re((\b\d+)(?:\((\d+)\)|:(\d+)(?:\((\d+)\))?))re");
    std::string text(log);
    std::string out;
    auto begin = std::sregex_iterator(text.begin(), text.end(), kPosition);
    std::size_t last = 0;
    for (auto it = begin; it != std::sregex_iterator(); ++it) {
        const auto& m = *it;
        const auto fileIndex = static_cast<std::size_t>(std::stoul(m[1].str()));
        if (fileIndex >= files.size()) {
            continue;
        }
        const std::string lineNumber = m[2].matched ? m[2].str() : m[3].str();
        out.append(text, last, static_cast<std::size_t>(m.position(0)) - last);
        out += files[fileIndex].string() + ":" + lineNumber;
        last = static_cast<std::size_t>(m.position(0) + m.length(0));
    }
    out.append(text, last);
    return out;
}

}  // namespace ds::graphics
