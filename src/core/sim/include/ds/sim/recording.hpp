// SPDX-FileCopyrightText: 2026 Juan Maria Gomez Lopez <juan-maria.gomez-lopez@tutamail.com>
// SPDX-License-Identifier: MIT

#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "ds/common/error.hpp"
#include "ds/common/json.hpp"
#include "ds/input/control.hpp"
#include "ds/physics/failures.hpp"
#include "ds/physics/vehicle.hpp"
#include "ds/sim/world_snapshot.hpp"

/// @file
/// @brief Recording of a session (M7 step 7): telemetry samples for the History tab's plots and
/// the replay log of FR-X-06 — the world's set-up and seeds at the start, the controls of every
/// tick and the changes made to the world while it ran. Replaying the log in a world built
/// from the same set-up reproduces the session exactly (FR-X-05).
/// @ingroup sim

namespace ds::sim {

/// @brief Rate of the telemetry samples (the `telemetry_samples` table).
inline constexpr double kTelemetryRateHz = 20.0;
/// @brief Longest session recorded, seconds of simulated time (2 h); later ticks are dropped
/// and the replay is marked truncated.
inline constexpr double kMaxRecordedS = 7200.0;

/// @brief One row of `telemetry_samples`.
struct TelemetrySample {
    double tS = 0.0;              ///< Time since the start of the session.
    physics::Vec3 position{0.0};  ///< World position.
    double rollRad = 0.0;         ///< Attitude.
    double pitchRad = 0.0;        ///< Attitude.
    double yawRad = 0.0;          ///< Attitude.
    double speedMs = 0.0;         ///< Ground speed.
    double aglM = 0.0;            ///< Height above ground.
    double throttle = 0.0;        ///< Throttle stick (aerial) or drive (ground, −1 … 1).
    double steering = 0.0;        ///< Steering (ground) or roll stick (aerial).
    double batteryV = 0.0;        ///< Battery voltage.
    double batteryA = 0.0;        ///< Battery current.
    /// @brief Equality.
    bool operator==(const TelemetrySample&) const = default;
};

/// @brief A change to the world during a session, applied before the tick with index
/// @ref tick. `data.kind` is one of `dt` (`value`), `failure` (a failure object),
/// `clearFailures`, `weather` (WeatherSettings JSON), `wind`, `air`, `visibility`, `utc`.
struct ReplayCommand {
    std::uint64_t tick = 0;  ///< Tick it applies before.
    common::Json data;       ///< What.
    /// @brief Equality.
    bool operator==(const ReplayCommand&) const = default;
};

/// @brief Incremental encoder of the replay input stream: per tick a mask of the fields that
/// changed, then their values (16-bit analog, a flags word, the mode). Typically 1–5 bytes a
/// tick.
class InputEncoder {
public:
    InputEncoder();
    /// @brief Appends the controls of one tick.
    void add(const input::ControlInputs& controls);
    /// @brief Ticks added.
    [[nodiscard]] std::uint64_t ticks() const noexcept { return ticks_; }
    /// @brief The stream so far.
    [[nodiscard]] const std::vector<std::uint8_t>& bytes() const noexcept { return bytes_; }
    /// @brief Moves the stream out (the encoder is then empty).
    [[nodiscard]] std::vector<std::uint8_t> take() noexcept;

private:
    std::vector<std::uint8_t> bytes_;
    std::array<std::int16_t, 6> last_{};
    std::uint16_t lastFlags_ = 0;
    int lastMode_ = -1;
    std::uint64_t ticks_ = 0;
};

/// @brief Reads InputEncoder's stream one tick at a time.
class InputDecoder {
public:
    /// @brief Reads @p bytes, which must outlive the decoder. Fails on a stream that is not a
    /// replay input stream of this format.
    [[nodiscard]] static common::Result<InputDecoder> open(std::span<const std::uint8_t> bytes);
    /// @brief The next tick's controls; nullopt at the end or on malformed data (see
    /// damaged()).
    [[nodiscard]] std::optional<input::ControlInputs> next();
    /// @brief True if next() stopped on malformed data.
    [[nodiscard]] bool damaged() const noexcept { return damaged_; }

private:
    explicit InputDecoder(std::span<const std::uint8_t> bytes) noexcept : bytes_(bytes) {}
    std::span<const std::uint8_t> bytes_;
    std::size_t at_ = 0;
    input::ControlInputs current_{};
    bool damaged_ = false;
};

/// @brief Encodes @p inputs in one go.
[[nodiscard]] std::vector<std::uint8_t> encodeInputs(std::span<const input::ControlInputs> inputs);
/// @brief Decodes a whole stream. Fails on truncated or malformed data.
[[nodiscard]] common::Result<std::vector<input::ControlInputs>> decodeInputs(
    std::span<const std::uint8_t> bytes);

/// @brief Quantises analog controls to the 16-bit grid the stream stores, so a replay sees
/// exactly what the live session saw (the world applies it to every tick it records).
[[nodiscard]] input::ControlInputs quantized(const input::ControlInputs& controls) noexcept;

/// @brief The replay of a session (FR-X-06).
struct ReplayLog {
    /// @brief Format of header, commands and inputs.
    static constexpr int kFormat = 1;
    /// World state at the start (World::startReplay() reads it): seeds, reset count, clock,
    /// vehicle profile, weather settings, air, wind, visibility, take-off point, time step;
    /// plus `scene` (the application's terrain, environment and items, opaque to the world)
    /// and `mission` (id of the mission flown, "" in free flight).
    common::Json header = common::Json::object();
    std::vector<std::uint8_t> inputs;     ///< InputEncoder stream: the controls of every tick.
    std::uint64_t ticks = 0;              ///< Ticks in @ref inputs.
    std::vector<ReplayCommand> commands;  ///< Changes, in tick order.
    bool replayable = true;  ///< False after a change the log cannot hold (terrain, items…).
    bool truncated = false;  ///< Longer than kMaxRecordedS.

    /// @brief Header, commands and flags as one JSON object (stored next to the inputs).
    [[nodiscard]] common::Json metaJson() const;
    /// @brief Reads metaJson()'s form; @ref inputs and @ref ticks are set by the caller.
    [[nodiscard]] static common::Result<ReplayLog> fromJson(const common::Json& meta);
    /// @brief Duration, seconds (from the header's and the dt commands' time steps).
    [[nodiscard]] double durationS() const;
};

/// @brief Failure as JSON (the mission file's names plus `type`).
[[nodiscard]] common::Json failureToJson(const physics::Failure& failure);
/// @brief Failure from JSON: `type` and the parameters it gives (others keep the defaults).
[[nodiscard]] std::optional<physics::Failure> failureFromJson(const common::Json& json);

/// @brief A finished session: its statistics, telemetry and replay.
struct FlightRecording {
    SessionStats stats;                                        ///< Figures for `sessions`.
    std::string profileKey;                                    ///< Vehicle profile.
    physics::VehicleType type = physics::VehicleType::Aerial;  ///< Vehicle type.
    std::string missionId;                                     ///< Mission flown ("" = free).
    std::vector<TelemetrySample> samples;                      ///< 20 Hz telemetry.
    ReplayLog replay;                                          ///< Replay log.
};

}  // namespace ds::sim
