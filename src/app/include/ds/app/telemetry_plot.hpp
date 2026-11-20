// SPDX-FileCopyrightText: 2026 Juan Maria Gomez Lopez <juan-maria.gomez-lopez@tutamail.com>
// SPDX-License-Identifier: MIT

#pragma once

#include <QWidget>

#include <span>
#include <string_view>
#include <vector>

#include "ds/storage/recording_store.hpp"

/// @file
/// @brief Telemetry plot of the History tab (M7 step 7): one quantity of a recorded session
/// against time, or its ground track seen from above. Drawn with QPainter (no Qt Charts).
/// @ingroup app

namespace ds::app {

/// @brief What the plot shows.
enum class PlotQuantity {
    Altitude,        ///< Height above ground, m.
    Speed,           ///< Ground speed, m/s.
    BatteryVoltage,  ///< Battery voltage, V.
    BatteryCurrent,  ///< Battery current, A.
    Throttle,        ///< Throttle or drive, %.
    Attitude,        ///< Roll and pitch, degrees (two lines).
    Track,           ///< Ground track from above (x east, z south), m.
};

/// @brief Every quantity, in menu order.
inline constexpr std::array<PlotQuantity, 7> kPlotQuantities{
    PlotQuantity::Altitude,       PlotQuantity::Speed,    PlotQuantity::BatteryVoltage,
    PlotQuantity::BatteryCurrent, PlotQuantity::Throttle, PlotQuantity::Attitude,
    PlotQuantity::Track};

/// @brief Menu title, with the unit ("Altitude AGL (m)").
[[nodiscard]] std::string_view plotQuantityTitle(PlotQuantity quantity) noexcept;

/// @brief Value of @p quantity in sample @p row (the first line for Attitude; NaN if absent).
[[nodiscard]] double plotValue(const storage::TelemetryRow& row, PlotQuantity quantity) noexcept;

/// @brief "Nice" axis ticks (1, 2 or 5 × 10ⁿ apart) covering [@p low, @p high], about
/// @p count of them.
[[nodiscard]] std::vector<double> niceTicks(double low, double high, int count = 5);

/// @brief Plots recorded telemetry.
class TelemetryPlot final : public QWidget {
    Q_OBJECT
public:
    /// @brief Empty plot.
    explicit TelemetryPlot(QWidget* parent = nullptr);
    /// @brief Shows @p samples (copied).
    void setSamples(std::span<const storage::TelemetryRow> samples);
    /// @brief Shows @p quantity.
    void setQuantity(PlotQuantity quantity);
    /// @brief Quantity shown.
    [[nodiscard]] PlotQuantity quantity() const noexcept { return quantity_; }
    /// @brief Samples shown.
    [[nodiscard]] std::size_t sampleCount() const noexcept { return samples_.size(); }
    /// @brief Preferred size.
    [[nodiscard]] QSize sizeHint() const override { return {420, 220}; }

protected:
    /// @brief Draws axes, ticks and the line(s).
    void paintEvent(QPaintEvent* event) override;

private:
    std::vector<storage::TelemetryRow> samples_;
    PlotQuantity quantity_ = PlotQuantity::Altitude;
};

}  // namespace ds::app
