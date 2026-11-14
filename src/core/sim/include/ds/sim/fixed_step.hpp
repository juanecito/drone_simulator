// SPDX-FileCopyrightText: 2026 Juan Maria Gomez Lopez <juan-maria.gomez-lopez@tutamail.com>
// SPDX-License-Identifier: MIT

#pragma once

/// @file
/// @brief Fixed-timestep accumulator (FR-LOOP-01/02/04).
/// @ingroup sim

namespace ds::sim {

/// @brief Converts elapsed real time into a whole number of fixed simulation steps.
class FixedStepClock {
public:
    /// @brief Default rate (FR-LOOP-01).
    static constexpr double kDefaultRateHz = 500.0;
    /// @brief Allowed rates.
    static constexpr double kMinRateHz = 240.0;
    /// @brief Allowed rates.
    static constexpr double kMaxRateHz = 2000.0;
    /// @brief Default catch-up limit per call (FR-LOOP-02).
    static constexpr double kDefaultMaxCatchUpS = 0.25;
    /// @brief Allowed time scales (FR-LOOP-04).
    static constexpr double kMinTimeScale = 0.1;
    /// @brief Allowed time scales.
    static constexpr double kMaxTimeScale = 4.0;

    /// @brief Sets the rate (clamped to 240–2000 Hz).
    void setRate(double hz) noexcept;
    /// @brief Rate, Hz.
    [[nodiscard]] double rate() const noexcept { return rate_; }
    /// @brief Step length, s.
    [[nodiscard]] double step() const noexcept { return 1.0 / rate_; }
    /// @brief Simulated seconds per real second (clamped to 0.1–4).
    void setTimeScale(double scale) noexcept;
    /// @brief Time scale.
    [[nodiscard]] double timeScale() const noexcept { return timeScale_; }
    /// @brief Real time that may be caught up in one advance() (excess is dropped).
    void setMaxCatchUp(double seconds) noexcept { maxCatchUp_ = seconds; }

    /// @brief Adds @p realSeconds and returns how many steps to run now.
    int advance(double realSeconds) noexcept;
    /// @brief Fraction of a step left in the accumulator (0–1; render interpolation).
    [[nodiscard]] double alpha() const noexcept { return accumulator_ * rate_; }
    /// @brief Real time dropped by the catch-up clamp so far.
    [[nodiscard]] double droppedSeconds() const noexcept { return dropped_; }
    /// @brief Clears the accumulator.
    void reset() noexcept { accumulator_ = 0.0; }

private:
    double rate_ = kDefaultRateHz;
    double timeScale_ = 1.0;
    double maxCatchUp_ = kDefaultMaxCatchUpS;
    double accumulator_ = 0.0;
    double dropped_ = 0.0;
};

}  // namespace ds::sim
