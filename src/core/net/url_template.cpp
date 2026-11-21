// SPDX-FileCopyrightText: 2026 Juan Maria Gomez Lopez <juan-maria.gomez-lopez@tutamail.com>
// SPDX-License-Identifier: MIT

#include "ds/net/url_template.hpp"

#include <algorithm>
#include <array>
#include <format>

namespace ds::net {
namespace {

constexpr std::array<std::string_view, 14> kPlaceholders{
    "dataset", "south", "north",  "west",   "east", "bbox", "width",
    "height",  "crs",   "format", "apiKey", "z",    "x",    "y"};

bool isUnreserved(char c) noexcept {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' ||
           c == '.' || c == '_' || c == '~';
}

void replaceAll(std::string& text, std::string_view what, std::string_view with) {
    if (what.empty()) {
        return;
    }
    std::size_t pos = 0;
    while ((pos = text.find(what, pos)) != std::string::npos) {
        text.replace(pos, what.size(), with);
        pos += with.size();
    }
}

}  // namespace

std::span<const std::string_view> urlPlaceholders() noexcept { return kPlaceholders; }

std::string percentEncode(std::string_view text) {
    std::string out;
    out.reserve(text.size());
    for (const char c : text) {
        if (isUnreserved(c)) {
            out += c;
        } else {
            out += std::format("%{:02X}", static_cast<unsigned char>(c));
        }
    }
    return out;
}

NetResult<std::string> expandUrlTemplate(
    std::string_view urlTemplate, const std::map<std::string, std::string, std::less<>>& values) {
    if (!urlTemplate.starts_with("https://") && !urlTemplate.starts_with("http://")) {
        return netError(NetErrorCode::InvalidRequest,
                        "URL template must start with https:// or http://");
    }
    std::string out;
    out.reserve(urlTemplate.size() + 64);
    for (std::size_t i = 0; i < urlTemplate.size(); ++i) {
        const char c = urlTemplate[i];
        if (c == '{' && i + 1 < urlTemplate.size() && urlTemplate[i + 1] == '{') {
            out += '{';
            ++i;
        } else if (c == '}' && i + 1 < urlTemplate.size() && urlTemplate[i + 1] == '}') {
            out += '}';
            ++i;
        } else if (c == '{') {
            const std::size_t close = urlTemplate.find('}', i);
            if (close == std::string_view::npos) {
                return netError(NetErrorCode::InvalidRequest, "URL template: unclosed '{'");
            }
            const std::string_view name = urlTemplate.substr(i + 1, close - i - 1);
            if (std::ranges::find(kPlaceholders, name) == kPlaceholders.end()) {
                return netError(NetErrorCode::InvalidRequest,
                                std::format("URL template: unknown placeholder {{{}}}", name));
            }
            const auto value = values.find(name);
            if (value == values.end()) {
                return netError(NetErrorCode::InvalidRequest,
                                std::format("URL template: no value for {{{}}}", name));
            }
            out += percentEncode(value->second);
            i = close;
        } else if (c == '}') {
            return netError(NetErrorCode::InvalidRequest, "URL template: unmatched '}'");
        } else {
            out += c;
        }
    }
    return out;
}

std::string appendQueryParameter(std::string_view url, std::string_view name,
                                 std::string_view value) {
    std::string out(url.substr(0, url.find('#')));
    const char separator = out.find('?') == std::string::npos
                               ? '?'
                               : (out.ends_with('?') || out.ends_with('&') ? '\0' : '&');
    if (separator != '\0') {
        out += separator;
    }
    out += percentEncode(name);
    out += '=';
    out += percentEncode(value);
    return out;
}

std::string redact(std::string_view text, std::span<const std::string> secrets) {
    std::string out(text);
    for (const std::string& secret : secrets) {
        if (secret.empty()) {
            continue;
        }
        replaceAll(out, percentEncode(secret), "***");
        replaceAll(out, secret, "***");
    }
    return out;
}

}  // namespace ds::net
