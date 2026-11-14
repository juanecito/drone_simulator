// SPDX-FileCopyrightText: 2026 Juan Maria Gomez Lopez <juan-maria.gomez-lopez@tutamail.com>
// SPDX-License-Identifier: MIT

#include "ds/app/hud_painter.hpp"

#include <QFont>
#include <QFontMetrics>
#include <QPainter>
#include <QPainterPath>

#include <cmath>
#include <numbers>
#include <vector>

namespace ds::app {
namespace {

QColor colorFor(sim::HudLevel level) {
    switch (level) {
        case sim::HudLevel::Caution: return {255, 190, 40};
        case sim::HudLevel::Warning: return {255, 70, 60};
        case sim::HudLevel::Normal: break;
    }
    return {245, 245, 245};
}

// Text with a dark outline, anchored by @p align at @p anchor.
void outlinedText(QPainter& p, QPointF anchor, const QString& text, const QColor& color,
                  Qt::Alignment align) {
    const QFontMetricsF fm(p.font());
    const double w = fm.horizontalAdvance(text);
    double x = anchor.x();
    if (align & Qt::AlignRight) {
        x -= w;
    } else if (align & Qt::AlignHCenter) {
        x -= 0.5 * w;
    }
    QPainterPath path;
    path.addText(QPointF(x, anchor.y() + fm.ascent()), p.font(), text);
    p.setPen(QPen(QColor(0, 0, 0, 200), 3.0, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    p.setBrush(Qt::NoBrush);
    p.drawPath(path);
    p.setPen(Qt::NoPen);
    p.setBrush(color);
    p.drawPath(path);
}

// A column of items, top to bottom from @p anchor.
void column(QPainter& p, QPointF anchor, const std::vector<const sim::HudItem*>& items,
            Qt::Alignment align) {
    const double line = QFontMetricsF(p.font()).height() * 1.1;
    for (const sim::HudItem* item : items) {
        if (item->text.empty()) {
            continue;
        }
        outlinedText(p, anchor, QString::fromStdString(item->text), colorFor(item->level), align);
        anchor.ry() += line;
    }
}

void horizon(QPainter& p, const QRectF& area, const sim::HudModel& hud) {
    const QPointF c = area.center();
    const double pxPerRad = 0.5 * area.height() / 0.6;  // ±34° of pitch on screen.
    p.save();
    p.translate(c);
    p.rotate(-hud.rollRad * 180.0 / std::numbers::pi);
    const double y0 = hud.pitchRad * pxPerRad;
    p.setPen(QPen(QColor(255, 255, 255, 190), 1.5));
    const double half = 0.18 * area.width();
    p.drawLine(QPointF(-half, y0), QPointF(-0.25 * half, y0));
    p.drawLine(QPointF(0.25 * half, y0), QPointF(half, y0));
    // Pitch ladder every 10°.
    p.setPen(QPen(QColor(255, 255, 255, 120), 1.0));
    for (int deg = -30; deg <= 30; deg += 10) {
        if (deg == 0) {
            continue;
        }
        const double y = y0 - deg * std::numbers::pi / 180.0 * pxPerRad;
        if (std::abs(y) > 0.4 * area.height()) {
            continue;
        }
        p.drawLine(QPointF(-0.4 * half, y), QPointF(-0.15 * half, y));
        p.drawLine(QPointF(0.15 * half, y), QPointF(0.4 * half, y));
    }
    p.restore();
    // Fixed aircraft symbol.
    p.setPen(QPen(QColor(255, 255, 255, 230), 2.0));
    p.drawLine(c + QPointF(-22, 0), c + QPointF(-8, 0));
    p.drawLine(c + QPointF(8, 0), c + QPointF(22, 0));
    p.drawEllipse(c, 3.0, 3.0);
}

void windArrow(QPainter& p, QPointF centre, double radius, double angleRad) {
    p.save();
    p.translate(centre);
    p.rotate(angleRad * 180.0 / std::numbers::pi);  // 0 = towards the top (the nose).
    QPainterPath arrow;
    arrow.moveTo(0, -radius);
    arrow.lineTo(0.45 * radius, -0.1 * radius);
    arrow.lineTo(0.15 * radius, -0.1 * radius);
    arrow.lineTo(0.15 * radius, radius);
    arrow.lineTo(-0.15 * radius, radius);
    arrow.lineTo(-0.15 * radius, -0.1 * radius);
    arrow.lineTo(-0.45 * radius, -0.1 * radius);
    arrow.closeSubpath();
    p.setPen(QPen(QColor(0, 0, 0, 200), 2.0));
    p.setBrush(QColor(120, 200, 255, 220));
    p.drawPath(arrow);
    p.restore();
}

// Point at @p distance from @p centre in compass direction @p deg (clockwise from up).
QPointF polar(QPointF centre, double deg, double distance) {
    const double a = deg * std::numbers::pi / 180.0;
    return centre + QPointF(distance * std::sin(a), -distance * std::cos(a));
}

}  // namespace

void paintCompass(QPainter& p, QPointF c, double r, const sim::HudCompass& compass,
                  std::optional<double> viewHeadingDeg) {
    p.save();
    p.setRenderHint(QPainter::Antialiasing, true);
    const QColor trueNorth(255, 80, 70);
    const QColor magneticNorth(90, 170, 255);
    const QColor ring(245, 245, 245, 220);

    // Plate and ring.
    p.setPen(Qt::NoPen);
    p.setBrush(QColor(0, 0, 0, 110));
    p.drawEllipse(c, r, r);
    p.setBrush(Qt::NoBrush);
    p.setPen(QPen(ring, 1.5));
    p.drawEllipse(c, r, r);

    // Ticks: every 10°, longer every 30°, longest and red at true north.
    for (int deg = 0; deg < 360; deg += 10) {
        const bool cardinal = deg % 90 == 0;
        const double inner = r * (cardinal ? 0.80 : deg % 30 == 0 ? 0.86 : 0.91);
        p.setPen(QPen(deg == 0 ? trueNorth : ring, cardinal ? 2.0 : 1.0));
        p.drawLine(polar(c, deg, inner), polar(c, deg, r));
    }

    // Cardinal points of true (geographic) north.
    QFont small = p.font();
    small.setPointSizeF(p.font().pointSizeF() * 0.85);
    p.setFont(small);
    const double labelHeight = QFontMetricsF(small).height();
    const struct {
        const char* text;
        double deg;
    } cardinals[] = {{"N", 0.0}, {"E", 90.0}, {"S", 180.0}, {"W", 270.0}};
    for (const auto& k : cardinals) {
        const QPointF at = polar(c, k.deg, r * 0.66) - QPointF(0.0, 0.5 * labelHeight);
        outlinedText(p, at, QString::fromLatin1(k.text), k.deg == 0.0 ? trueNorth : ring,
                     Qt::AlignHCenter);
    }

    // Magnetic north: blue marker outside the ring, turned by the declination.
    {
        const double d = compass.declinationDeg;
        QPainterPath marker;
        marker.moveTo(polar(c, d, r + 1.0));
        marker.lineTo(polar(c, d - 6.0, r + 0.25 * r));
        marker.lineTo(polar(c, d + 6.0, r + 0.25 * r));
        marker.closeSubpath();
        p.setPen(QPen(QColor(0, 0, 0, 200), 1.0));
        p.setBrush(magneticNorth);
        p.drawPath(marker);
        const QPointF label =
            polar(c, d, r + 0.25 * r + 0.55 * labelHeight) - QPointF(0.0, 0.5 * labelHeight);
        outlinedText(p, label, QStringLiteral("MN"), magneticNorth, Qt::AlignHCenter);
    }

    // Camera view direction (free camera).
    if (viewHeadingDeg) {
        p.setPen(QPen(QColor(255, 255, 255, 170), 1.2, Qt::DashLine));
        p.drawLine(c, polar(c, *viewHeadingDeg, 0.92 * r));
    }

    // Vehicle heading: arrow from the centre.
    {
        const double h = compass.trueHeadingDeg;
        QPainterPath arrow;
        arrow.moveTo(polar(c, h, 0.48 * r));
        arrow.lineTo(polar(c, h + 145.0, 0.30 * r));
        arrow.lineTo(polar(c, h + 180.0, 0.12 * r));
        arrow.lineTo(polar(c, h - 145.0, 0.30 * r));
        arrow.closeSubpath();
        p.setPen(QPen(QColor(0, 0, 0, 220), 1.5));
        p.setBrush(QColor(255, 210, 60));
        p.drawPath(arrow);
    }

    // Declination (or a warning) under the rose.
    outlinedText(p, c + QPointF(0.0, r + 4.0), QString::fromStdString(compass.declination.text),
                 colorFor(compass.declination.level), Qt::AlignHCenter);
    p.restore();
}

void paintHud(QPainter& p, const QRect& rect, const sim::HudModel& hud,
              const HudPaintOptions& options) {
    p.save();
    p.setRenderHint(QPainter::Antialiasing, true);
    p.setRenderHint(QPainter::TextAntialiasing, true);
    QFont font(QStringLiteral("monospace"));
    font.setStyleHint(QFont::Monospace);
    font.setBold(true);
    font.setPointSizeF(10.0 * options.scale);
    p.setFont(font);
    const QRectF area(rect);
    const double margin = 12.0 * options.scale;
    const double line = QFontMetricsF(font).height() * 1.1;

    if (options.horizon) {
        horizon(p, area, hud);
    }

    // Position panel (top left, FR-GFX-14a) on a translucent plate.
    {
        std::vector<QString> lines{QString::fromStdString(hud.positionUtm),
                                   QString::fromStdString(hud.positionElevation)};
        if (!hud.positionLatLon.empty()) {
            lines.push_back(QString::fromStdString(hud.positionLatLon));
        }
        const QFontMetricsF fm(font);
        double width = 0.0;
        for (const QString& l : lines) {
            width = std::max(width, fm.horizontalAdvance(l));
        }
        const QRectF plate(area.left() + margin - 6, area.top() + margin - 4, width + 12,
                           line * static_cast<double>(lines.size()) + 8);
        p.setPen(Qt::NoPen);
        p.setBrush(QColor(0, 0, 0, 110));
        p.drawRoundedRect(plate, 4, 4);
        QPointF at(area.left() + margin, area.top() + margin);
        for (const QString& l : lines) {
            outlinedText(p, at, l, QColor(235, 245, 255), Qt::AlignLeft);
            at.ry() += line;
        }
    }

    // Clocks and timer (top right).
    column(p, {area.right() - margin, area.top() + margin},
           {&hud.clockLocal, &hud.clockUtc, &hud.timer}, Qt::AlignRight);

    // Heading, true and magnetic (top centre).
    outlinedText(p, {area.center().x(), area.top() + margin},
                 QString::fromStdString(hud.compass.heading.text),
                 colorFor(hud.compass.heading.level), Qt::AlignHCenter);

    // Warnings (under the heading, flashing).
    if (options.blinkOn) {
        QPointF at(area.center().x(), area.top() + margin + 2.0 * line);
        for (const sim::HudItem& w : hud.warnings) {
            outlinedText(p, at, QString::fromStdString(w.text), colorFor(w.level),
                         Qt::AlignHCenter);
            at.ry() += line;
        }
    }

    // Heights and climb (left middle); speeds, distances and wind (right middle).
    const double middle = area.center().y() - line;
    column(p, {area.left() + margin, middle},
           {&hud.altitudeAgl, &hud.altitudeAmsl, &hud.verticalSpeed}, Qt::AlignLeft);
    column(p, {area.right() - margin, middle},
           {&hud.speed, &hud.home, &hud.pilotDistance, &hud.wind}, Qt::AlignRight);
    if (hud.windArrowRad) {
        windArrow(p, {area.right() - margin - 14.0 * options.scale, middle + 5.0 * line},
                  12.0 * options.scale, *hud.windArrowRad);
    }

    // Vehicle state (bottom).
    const double bottom = area.bottom() - margin - 3.0 * line;
    column(p, {area.left() + margin, bottom}, {&hud.battery, &hud.gps, &hud.lights}, Qt::AlignLeft);
    column(p, {area.right() - margin, bottom}, {&hud.armed, &hud.mode, &hud.throttle},
           Qt::AlignRight);

    // Compass rose (bottom centre, FR-GFX-14b).
    const double radius = compassRadius(options.scale);
    paintCompass(p, {area.center().x(), area.bottom() - margin - line - 4.0 - radius}, radius,
                 hud.compass);
    p.restore();
}

}  // namespace ds::app
