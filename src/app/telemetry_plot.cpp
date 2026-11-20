// SPDX-FileCopyrightText: 2026 Juan Maria Gomez Lopez <juan-maria.gomez-lopez@tutamail.com>
// SPDX-License-Identifier: MIT

#include "ds/app/telemetry_plot.hpp"

#include <QPainter>
#include <QPainterPath>
#include <QPalette>

#include <algorithm>
#include <cmath>
#include <limits>
#include <numbers>

namespace ds::app {
namespace {

constexpr double kRadToDeg = 180.0 / std::numbers::pi;

QString tickLabel(double v, double step) {
    const int decimals = step >= 1.0 ? 0 : static_cast<int>(std::ceil(-std::log10(step)));
    return QString::number(v, 'f', std::clamp(decimals, 0, 3));
}

struct Range {
    double low = std::numeric_limits<double>::infinity();
    double high = -std::numeric_limits<double>::infinity();
    void add(double v) {
        if (std::isfinite(v)) {
            low = std::min(low, v);
            high = std::max(high, v);
        }
    }
    [[nodiscard]] bool valid() const { return low <= high; }
    void pad() {
        if (high - low < 1e-6) {
            low -= 1.0;
            high += 1.0;
        }
    }
};

}  // namespace

std::string_view plotQuantityTitle(PlotQuantity quantity) noexcept {
    switch (quantity) {
        case PlotQuantity::Altitude: return "Altitude AGL (m)";
        case PlotQuantity::Speed: return "Ground speed (m/s)";
        case PlotQuantity::BatteryVoltage: return "Battery voltage (V)";
        case PlotQuantity::BatteryCurrent: return "Battery current (A)";
        case PlotQuantity::Throttle: return "Throttle / drive (%)";
        case PlotQuantity::Attitude: return "Roll and pitch (°)";
        case PlotQuantity::Track: return "Ground track (m)";
    }
    return "";
}

double plotValue(const storage::TelemetryRow& r, PlotQuantity quantity) noexcept {
    switch (quantity) {
        case PlotQuantity::Altitude: return r.altAgl;
        case PlotQuantity::Speed: return r.speed;
        case PlotQuantity::BatteryVoltage: return r.batteryV;
        case PlotQuantity::BatteryCurrent: return r.batteryA;
        case PlotQuantity::Throttle: return r.throttle * 100.0;
        case PlotQuantity::Attitude: return r.roll * kRadToDeg;
        case PlotQuantity::Track: return r.posX;
    }
    return std::nan("");
}

std::vector<double> niceTicks(double low, double high, int count) {
    std::vector<double> ticks;
    if (!(high > low) || count < 1) {
        return ticks;
    }
    const double raw = (high - low) / count;
    const double magnitude = std::pow(10.0, std::floor(std::log10(raw)));
    double step = magnitude;
    for (const double m : {1.0, 2.0, 5.0, 10.0}) {
        if (m * magnitude >= raw) {
            step = m * magnitude;
            break;
        }
    }
    for (double v = std::ceil(low / step) * step; v <= high + step * 1e-9; v += step) {
        ticks.push_back(std::abs(v) < step * 1e-9 ? 0.0 : v);
    }
    return ticks;
}

TelemetryPlot::TelemetryPlot(QWidget* parent) : QWidget(parent) {
    setMinimumHeight(160);
    setAutoFillBackground(true);
}

void TelemetryPlot::setSamples(std::span<const storage::TelemetryRow> samples) {
    samples_.assign(samples.begin(), samples.end());
    update();
}

void TelemetryPlot::setQuantity(PlotQuantity quantity) {
    quantity_ = quantity;
    update();
}

void TelemetryPlot::paintEvent(QPaintEvent* /*event*/) {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    const QPalette& pal = palette();
    p.fillRect(rect(), pal.color(QPalette::Base));
    const QFontMetrics fm(font());
    if (samples_.size() < 2) {
        p.setPen(pal.color(QPalette::PlaceholderText));
        p.drawText(rect(), Qt::AlignCenter, tr("Select a session to plot its telemetry."));
        return;
    }
    const bool track = quantity_ == PlotQuantity::Track;
    // Data ranges: x is time (or east), y the quantity (or north).
    Range xr;
    Range yr;
    for (const storage::TelemetryRow& r : samples_) {
        if (track) {
            xr.add(r.posX);
            yr.add(-r.posZ);
        } else {
            xr.add(r.tS);
            yr.add(plotValue(r, quantity_));
            if (quantity_ == PlotQuantity::Attitude) {
                yr.add(r.pitch * kRadToDeg);
            }
        }
    }
    if (!xr.valid() || !yr.valid()) {
        return;
    }
    xr.pad();
    yr.pad();
    const QRectF plot(fm.horizontalAdvance(QStringLiteral("-00000")) + 8.0, fm.height() + 6.0,
                      width() - fm.horizontalAdvance(QStringLiteral("-00000")) - 18.0,
                      height() - 3.0 * fm.height() - 14.0);
    if (plot.width() < 20.0 || plot.height() < 20.0) {
        return;
    }
    if (track) {
        // Same scale on both axes.
        const double scale =
            std::min(plot.width() / (xr.high - xr.low), plot.height() / (yr.high - yr.low));
        const double cx = 0.5 * (xr.low + xr.high);
        const double cy = 0.5 * (yr.low + yr.high);
        xr = {cx - 0.5 * plot.width() / scale, cx + 0.5 * plot.width() / scale};
        yr = {cy - 0.5 * plot.height() / scale, cy + 0.5 * plot.height() / scale};
    }
    const auto toX = [&](double v) {
        return plot.left() + (v - xr.low) / (xr.high - xr.low) * plot.width();
    };
    const auto toY = [&](double v) {
        return plot.bottom() - (v - yr.low) / (yr.high - yr.low) * plot.height();
    };

    // Grid and labels.
    const QColor text = pal.color(QPalette::Text);
    QColor grid = pal.color(QPalette::Mid);
    grid.setAlpha(110);
    const auto xTicks = niceTicks(xr.low, xr.high, 6);
    const auto yTicks = niceTicks(yr.low, yr.high, 5);
    const double xStep = xTicks.size() > 1 ? xTicks[1] - xTicks[0] : 1.0;
    const double yStep = yTicks.size() > 1 ? yTicks[1] - yTicks[0] : 1.0;
    for (const double v : xTicks) {
        p.setPen(QPen(grid, 1.0));
        p.drawLine(QPointF(toX(v), plot.top()), QPointF(toX(v), plot.bottom()));
        p.setPen(text);
        p.drawText(QRectF(toX(v) - 40.0, plot.bottom() + 2.0, 80.0, fm.height()),
                   Qt::AlignHCenter | Qt::AlignTop, tickLabel(v, xStep));
    }
    for (const double v : yTicks) {
        p.setPen(QPen(grid, 1.0));
        p.drawLine(QPointF(plot.left(), toY(v)), QPointF(plot.right(), toY(v)));
        p.setPen(text);
        p.drawText(QRectF(0.0, toY(v) - fm.height() / 2.0, plot.left() - 4.0, fm.height()),
                   Qt::AlignRight | Qt::AlignVCenter, tickLabel(v, yStep));
    }
    p.setPen(QPen(pal.color(QPalette::Dark), 1.0));
    p.drawRect(plot);
    p.setPen(text);
    p.drawText(QRectF(plot.left(), 0.0, plot.width(), fm.height() + 4.0),
               Qt::AlignLeft | Qt::AlignVCenter,
               QString::fromUtf8(plotQuantityTitle(quantity_).data(),
                                 static_cast<qsizetype>(plotQuantityTitle(quantity_).size())));
    p.drawText(QRectF(plot.left(), plot.bottom() + fm.height() + 2.0, plot.width(), fm.height()),
               Qt::AlignHCenter | Qt::AlignTop, track ? tr("east (m) — north up") : tr("time (s)"));

    // Lines (gaps where a value is missing).
    const auto line = [&](auto value, const QColor& color) {
        QPainterPath path;
        bool pen = false;
        for (const storage::TelemetryRow& r : samples_) {
            const auto [x, y] = value(r);
            if (!std::isfinite(x) || !std::isfinite(y)) {
                pen = false;
                continue;
            }
            const QPointF point(toX(x), toY(y));
            if (pen) {
                path.lineTo(point);
            } else {
                path.moveTo(point);
                pen = true;
            }
        }
        p.setPen(QPen(color, 1.6));
        p.setBrush(Qt::NoBrush);
        p.drawPath(path);
    };
    p.save();
    p.setClipRect(plot.adjusted(-1, -1, 1, 1));
    const QColor primary(31, 119, 180);
    if (track) {
        line([](const storage::TelemetryRow& r) { return std::pair(r.posX, -r.posZ); }, primary);
        const storage::TelemetryRow& first = samples_.front();
        const storage::TelemetryRow& last = samples_.back();
        p.setPen(Qt::NoPen);
        p.setBrush(QColor(44, 160, 44));
        p.drawEllipse(QPointF(toX(first.posX), toY(-first.posZ)), 4.0, 4.0);
        p.setBrush(QColor(214, 39, 40));
        p.drawEllipse(QPointF(toX(last.posX), toY(-last.posZ)), 4.0, 4.0);
    } else {
        const PlotQuantity q = quantity_;
        line([q](const storage::TelemetryRow& r) { return std::pair(r.tS, plotValue(r, q)); },
             primary);
        if (q == PlotQuantity::Attitude) {
            line(
                [](const storage::TelemetryRow& r) { return std::pair(r.tS, r.pitch * kRadToDeg); },
                QColor(255, 127, 14));
        }
    }
    p.restore();
    if (quantity_ == PlotQuantity::Attitude) {
        p.setPen(QColor(31, 119, 180));
        p.drawText(QRectF(plot.right() - 120.0, 0.0, 55.0, fm.height() + 4.0),
                   Qt::AlignRight | Qt::AlignVCenter, tr("roll"));
        p.setPen(QColor(255, 127, 14));
        p.drawText(QRectF(plot.right() - 60.0, 0.0, 60.0, fm.height() + 4.0),
                   Qt::AlignRight | Qt::AlignVCenter, tr("pitch"));
    } else if (track) {
        const QString start = tr("start");
        const QString end = tr("end");
        const double y = (fm.height() + 4.0) / 2.0;
        double x = plot.right() - fm.horizontalAdvance(end);
        p.setPen(text);
        p.drawText(QPointF(x, y + fm.ascent() / 2.0 - 1.0), end);
        x -= 10.0;
        p.setPen(Qt::NoPen);
        p.setBrush(QColor(214, 39, 40));
        p.drawEllipse(QPointF(x, y), 4.0, 4.0);
        x -= 14.0 + fm.horizontalAdvance(start);
        p.setPen(text);
        p.drawText(QPointF(x, y + fm.ascent() / 2.0 - 1.0), start);
        x -= 10.0;
        p.setPen(Qt::NoPen);
        p.setBrush(QColor(44, 160, 44));
        p.drawEllipse(QPointF(x, y), 4.0, 4.0);
    }
}

}  // namespace ds::app
