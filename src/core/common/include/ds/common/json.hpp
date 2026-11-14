// SPDX-FileCopyrightText: 2026 Juan Maria Gomez Lopez <juan-maria.gomez-lopez@tutamail.com>
// SPDX-License-Identifier: MIT

#pragma once

#include <cstddef>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "ds/common/error.hpp"

#include <nlohmann/json.hpp>

/// @file
/// @brief JSON helpers for Core data files (nlohmann/json): bounded file reading and a
/// validating reader whose error messages contain the JSON path of the bad value.
/// @ingroup common

namespace ds::common {

/// @brief JSON value type used by the Core modules.
using Json = nlohmann::json;

/// @brief Parses @p text. Errors carry the parser's position.
[[nodiscard]] Result<Json> parseJson(std::string_view text);

/// @brief Reads and parses a JSON file of at most @p maxBytes (files are untrusted input).
[[nodiscard]] Result<Json> readJsonFile(const std::filesystem::path& file,
                                        std::size_t maxBytes = std::size_t{4} << 20U);

/// @brief Reads typed, range-checked members of a JSON object and collects errors.
/// @details Every accessor returns a usable value (the default or a clamped value) even when
/// the input is wrong, and records an error such as `/motor/kv: must be between 100 and
/// 10000`. Check ok() / errorText() after reading. Child readers share the error list.
class JsonReader {
public:
    /// @brief Reads @p value, located at @p path (a JSON pointer, "" for the document root).
    explicit JsonReader(const Json& value, std::string path = {});

    /// @brief Required number in [@p min, @p max].
    double number(std::string_view key, double min, double max);
    /// @brief Optional number in [@p min, @p max]; @p fallback if absent.
    double number(std::string_view key, double min, double max, double fallback);
    /// @brief Required integer in [@p min, @p max].
    int integer(std::string_view key, int min, int max);
    /// @brief Optional integer in [@p min, @p max]; @p fallback if absent.
    int integer(std::string_view key, int min, int max, int fallback);
    /// @brief Optional boolean; @p fallback if absent.
    bool boolean(std::string_view key, bool fallback);
    /// @brief Required non-empty string.
    std::string string(std::string_view key);
    /// @brief Optional string; @p fallback if absent.
    std::string string(std::string_view key, std::string_view fallback);
    /// @brief Required string that must be one of @p allowed.
    std::string choice(std::string_view key, const std::vector<std::string_view>& allowed);
    /// @brief Required object member.
    JsonReader object(std::string_view key);
    /// @brief Optional object member.
    std::optional<JsonReader> optionalObject(std::string_view key);
    /// @brief Required array member (empty array if missing or wrong); elements are read with
    /// element().
    std::size_t array(std::string_view key);
    /// @brief Optional array member (0 if absent).
    std::size_t optionalArray(std::string_view key);
    /// @brief Element @p index of the array member @p key.
    JsonReader element(std::string_view key, std::size_t index);
    /// @brief This value as a number in [@p min, @p max] (for array elements).
    double asNumber(double min, double max);
    /// @brief True if member @p key exists.
    [[nodiscard]] bool has(std::string_view key) const;

    /// @brief Records a custom error at member @p key ("" for this value).
    void fail(std::string_view key, std::string_view message);

    /// @brief True if no error was recorded (by this reader or any reader sharing its list).
    [[nodiscard]] bool ok() const noexcept { return errors_->empty(); }
    /// @brief All errors, one per line.
    [[nodiscard]] std::string errorText() const;
    /// @brief The errors.
    [[nodiscard]] const std::vector<std::string>& errors() const noexcept { return *errors_; }
    /// @brief JSON pointer of this value.
    [[nodiscard]] const std::string& path() const noexcept { return path_; }
    /// @brief The value.
    [[nodiscard]] const Json& value() const noexcept { return *value_; }

private:
    JsonReader(const Json* value, std::string path,
               std::shared_ptr<std::vector<std::string>> errors);
    [[nodiscard]] const Json* member(std::string_view key) const;
    [[nodiscard]] std::string childPath(std::string_view key) const;

    const Json* value_;
    std::string path_;
    std::shared_ptr<std::vector<std::string>> errors_;
};

}  // namespace ds::common
