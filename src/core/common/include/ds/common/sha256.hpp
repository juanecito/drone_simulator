// SPDX-FileCopyrightText: 2026 Juan Maria Gomez Lopez <juan-maria.gomez-lopez@tutamail.com>
// SPDX-License-Identifier: MIT

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <span>
#include <stop_token>
#include <string>
#include <string_view>

#include "ds/common/error.hpp"

/// @file
/// @brief SHA-256 (FIPS 180-4) for file integrity checks (FR-TER-F09, FR-TER-I09) and cache
/// content hashes (FR-TER-O14).
/// @ingroup common

namespace ds::common {

/// @brief Incremental SHA-256.
/// @details Feed data with update() in any number of pieces, then call finish() once.
class Sha256 {
public:
    /// @brief A 32-byte digest.
    using Digest = std::array<std::uint8_t, 32>;

    Sha256() noexcept;

    /// @brief Adds @p data to the message.
    void update(std::span<const std::byte> data) noexcept;
    /// @brief Adds the bytes of @p text to the message.
    void update(std::string_view text) noexcept;
    /// @brief Pads the message and returns the digest. The object must not be used afterwards.
    [[nodiscard]] Digest finish() noexcept;

    /// @brief Lower-case hexadecimal form of @p digest (64 characters).
    [[nodiscard]] static std::string toHex(const Digest& digest);

private:
    void block(const std::uint8_t* data) noexcept;

    std::array<std::uint32_t, 8> state_{};
    std::array<std::uint8_t, 64> buffer_{};
    std::size_t buffered_ = 0;
    std::uint64_t length_ = 0;  // Message length in bytes.
};

/// @brief Hex SHA-256 of @p text.
[[nodiscard]] std::string sha256Hex(std::string_view text);

/// @brief Hex SHA-256 of the file @p file, read in 1 MiB pieces.
/// @details Stops with an error ("cancelled") as soon as @p stop is requested, so hashing a
/// multi-gigabyte file can be cancelled.
[[nodiscard]] Result<std::string> sha256File(const std::filesystem::path& file,
                                             std::stop_token stop = {});

/// @brief True if @p text is 64 hexadecimal characters (either case).
[[nodiscard]] bool isSha256Hex(std::string_view text) noexcept;

}  // namespace ds::common
