// SPDX-FileCopyrightText: 2026 Juan Maria Gomez Lopez <juan-maria.gomez-lopez@tutamail.com>
// SPDX-License-Identifier: MIT

#include "ds/graphics/image.hpp"

#include <cctype>
#include <format>
#include <fstream>
#include <string>

namespace ds::graphics {
namespace {

// Reads the next header token, skipping whitespace and '#' comments.
bool token(std::istream& in, std::string& out) {
    out.clear();
    int c = in.get();
    while (c != EOF) {
        if (c == '#') {
            while (c != EOF && c != '\n') {
                c = in.get();
            }
        } else if (std::isspace(c) != 0) {
            c = in.get();
        } else {
            break;
        }
    }
    while (c != EOF && std::isspace(c) == 0) {
        out.push_back(static_cast<char>(c));
        c = in.get();
    }
    return !out.empty();  // The single whitespace after the last token has been consumed.
}

}  // namespace

common::Result<GrayImage> loadPgm(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        return common::makeError(std::format("cannot read image '{}'", path.string()));
    }
    std::string magic;
    std::string w;
    std::string h;
    std::string maxval;
    if (!token(in, magic) || magic != "P5" || !token(in, w) || !token(in, h) ||
        !token(in, maxval)) {
        return common::makeError(std::format("'{}' is not a binary PGM image", path.string()));
    }
    GrayImage image;
    try {
        image.width = std::stoi(w);
        image.height = std::stoi(h);
        if (std::stoi(maxval) > 255) {
            throw std::out_of_range("maxval");
        }
    } catch (const std::exception&) {
        return common::makeError(std::format("'{}' has an invalid PGM header", path.string()));
    }
    if (image.width <= 0 || image.height <= 0 || image.width > 16384 || image.height > 16384) {
        return common::makeError(std::format("'{}' has an invalid size", path.string()));
    }
    image.pixels.resize(static_cast<std::size_t>(image.width) *
                        static_cast<std::size_t>(image.height));
    in.read(reinterpret_cast<char*>(image.pixels.data()),
            static_cast<std::streamsize>(image.pixels.size()));
    if (in.gcount() != static_cast<std::streamsize>(image.pixels.size())) {
        return common::makeError(std::format("'{}' is truncated", path.string()));
    }
    return image;
}

}  // namespace ds::graphics
