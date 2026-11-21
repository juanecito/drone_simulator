// SPDX-FileCopyrightText: 2026 Juan Maria Gomez Lopez <juan-maria.gomez-lopez@tutamail.com>
// SPDX-License-Identifier: MIT

#pragma once

#include <map>
#include <span>
#include <string>
#include <string_view>

#include "ds/net/net_error.hpp"

/// @file
/// @brief Provider URL templates (FR-TER-O02) and API-key redaction (FR-NET-07).
/// @ingroup net

namespace ds::net {

/// @brief Placeholders understood in provider URL templates.
/// @details `{dataset} {south} {north} {west} {east} {bbox} {width} {height} {crs} {format}
/// {apiKey}`, and `{z} {x} {y}` for tile providers.
[[nodiscard]] std::span<const std::string_view> urlPlaceholders() noexcept;

/// @brief Percent-encodes @p text for use in a URL query or path (RFC 3986 unreserved
/// characters are kept).
[[nodiscard]] std::string percentEncode(std::string_view text);

/// @brief Replaces every `{name}` in @p urlTemplate by the percent-encoded value of @p values.
/// @details Unknown placeholder names, placeholders without a value, unbalanced braces and
/// templates that are not `http://` or `https://` URLs are InvalidRequest errors. `{{` and `}}`
/// stand for literal braces.
[[nodiscard]] NetResult<std::string> expandUrlTemplate(
    std::string_view urlTemplate, const std::map<std::string, std::string, std::less<>>& values);

/// @brief Appends `name=value` (percent-encoded) to the query of @p url.
[[nodiscard]] std::string appendQueryParameter(std::string_view url, std::string_view name,
                                               std::string_view value);

/// @brief Replaces every occurrence of each non-empty secret in @p text (raw and
/// percent-encoded) by `***`. Used on URLs, logs and error messages (FR-NET-07).
[[nodiscard]] std::string redact(std::string_view text, std::span<const std::string> secrets);

}  // namespace ds::net
