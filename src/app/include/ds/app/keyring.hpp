// SPDX-FileCopyrightText: 2026 Juan Maria Gomez Lopez <juan-maria.gomez-lopez@tutamail.com>
// SPDX-License-Identifier: MIT

#pragma once

#include <chrono>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>

#include "ds/common/error.hpp"

/// @file
/// @brief Secrets in the OS keyring (FR-NET-07, NFR-SEC-02a): Secret Service on Linux,
/// Credential Manager on Windows, through QtKeychain.
/// @ingroup app

namespace ds::app {

/// @brief Stores short secrets (API keys) by name.
/// @details Errors mean the keyring cannot be used (no backend, access denied, timeout); a
/// missing entry is not an error. Secrets never appear in error messages.
class Keyring {
public:
    virtual ~Keyring() = default;

    /// @brief The secret named @p name, nullopt if there is none.
    [[nodiscard]] virtual common::Result<std::optional<std::string>> read(
        std::string_view name) = 0;
    /// @brief Stores @p secret as @p name, replacing an older one.
    [[nodiscard]] virtual common::Result<void> write(std::string_view name,
                                                     std::string_view secret) = 0;
    /// @brief Removes @p name (no error if there is none).
    [[nodiscard]] virtual common::Result<void> remove(std::string_view name) = 0;
};

/// @brief The OS keyring through QtKeychain, service `DroneSimulator`.
/// @details Each call runs a local event loop until the job ends, at most @p timeout (a locked
/// keyring may ask the user to unlock it). QtKeychain's plain-text fallback is never used: the
/// application's fallback is the user database, with a warning (FR-NET-07). Use it on the GUI
/// thread only.
class OsKeyring final : public Keyring {
public:
    /// @brief Uses the keyring with @p timeout per call.
    explicit OsKeyring(std::chrono::milliseconds timeout = std::chrono::seconds(20));

    [[nodiscard]] common::Result<std::optional<std::string>> read(std::string_view name) override;
    [[nodiscard]] common::Result<void> write(std::string_view name,
                                             std::string_view secret) override;
    [[nodiscard]] common::Result<void> remove(std::string_view name) override;

private:
    std::chrono::milliseconds timeout_;
};

/// @brief A keyring in memory (tests), which can pretend to be unavailable.
class MemoryKeyring final : public Keyring {
public:
    [[nodiscard]] common::Result<std::optional<std::string>> read(std::string_view name) override;
    [[nodiscard]] common::Result<void> write(std::string_view name,
                                             std::string_view secret) override;
    [[nodiscard]] common::Result<void> remove(std::string_view name) override;

    /// @brief Makes every call fail as if no keyring backend were available.
    void setAvailable(bool available);
    /// @brief The stored secret of @p name (tests).
    [[nodiscard]] std::optional<std::string> peek(std::string_view name) const;

private:
    mutable std::mutex mutex_;
    std::map<std::string, std::string, std::less<>> secrets_;
    bool available_ = true;
};

/// @brief Keyring entry name of the API key of provider @p providerKey: `provider/<key>`.
[[nodiscard]] std::string apiKeyEntry(std::string_view providerKey);

}  // namespace ds::app
