// SPDX-FileCopyrightText: 2026 Juan Maria Gomez Lopez <juan-maria.gomez-lopez@tutamail.com>
// SPDX-License-Identifier: MIT

#include "ds/app/hud_painter.hpp"

#include <QFont>
#include <QFontMetrics>
#include <QPainter>
#include <QPainterPath>
#include <QPolygonF>

#include <algorithm>
#include <cmath>
#include <numbers>
#include <vector>

namespace ds::app {
namespace {

QColor colorFor(sim::HudLevel level) {
    switch (level) {
        case sim::HudLevel::Caution: return {255, 190, 40};
        case sim::HudLevel::Warning: return {255, 70, 60};
        case sim::HudLevel::Good: return {110, 230, 120};
        case sim::HudLevel::Muted: return {170, 170, 170};
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

// Ground drone gauges under the attitude column (FR-GFX-20): the lateral load transfer as a
// bar (the marker reaching an end means the wheels on the other side lift) and a top view of
// the wheels coloured by their slip (grey: off the ground).
void groundGauges(QPainter& p, QPointF topLeft, const sim::HudGround& g, double scale) {
    p.save();
    const double barW = 110.0 * scale;
    const double barH = 7.0 * scale;
    const QRectF bar(topLeft.x(), topLeft.y(), barW, barH);
    p.setPen(QPen(QColor(0, 0, 0, 200), 1.0));
    p.setBrush(QColor(0, 0, 0, 110));
    p.drawRect(bar);
    // Amber and red zones at both ends.
    for (const double side : {-1.0, 1.0}) {
        for (const auto& [from, color] :
             {std::pair{sim::kRolloverCaution, colorFor(sim::HudLevel::Caution)},
              std::pair{sim::kRolloverWarning, colorFor(sim::HudLevel::Warning)}}) {
            const double x0 = bar.center().x() + side * from * 0.5 * barW;
            const double x1 = bar.center().x() + side * 0.5 * barW;
            QColor c = color;
            c.setAlpha(90);
            p.setPen(Qt::NoPen);
            p.setBrush(c);
            p.drawRect(QRectF(QPointF(std::min(x0, x1), bar.top()),
                              QPointF(std::max(x0, x1), bar.bottom())));
        }
    }
    const double x = bar.center().x() + g.loadTransfer * 0.5 * barW;
    p.setPen(QPen(QColor(0, 0, 0, 220), 3.0 * scale));
    p.drawLine(QPointF(x, bar.top() - 3.0 * scale), QPointF(x, bar.bottom() + 3.0 * scale));
    p.setPen(QPen(colorFor(g.stability), 1.5 * scale));
    p.drawLine(QPointF(x, bar.top() - 3.0 * scale), QPointF(x, bar.bottom() + 3.0 * scale));
    p.setPen(QPen(QColor(255, 255, 255, 160), 1.0));
    p.drawLine(QPointF(bar.center().x(), bar.top()), QPointF(bar.center().x(), bar.bottom()));

    // Wheels seen from above, nose up: left column, right column, front to rear.
    const int perSide = std::max(g.wheelCount / 2, 1);
    const double wheelW = 6.0 * scale;
    const double wheelH = 10.0 * scale;
    const double gap = 3.0 * scale;
    const double bodyW = 18.0 * scale;
    const double top = bar.bottom() + 9.0 * scale;
    const double left = topLeft.x();
    const double bodyH = perSide * wheelH + (perSide - 1) * gap;
    p.setPen(QPen(QColor(0, 0, 0, 200), 1.0));
    p.setBrush(QColor(255, 255, 255, 60));
    p.drawRect(QRectF(left + wheelW + 2.0 * scale, top, bodyW, bodyH));
    for (int i = 0; i < g.wheelCount; ++i) {
        const bool right = i >= perSide;
        const int row = right ? i - perSide : i;
        const double wx = right ? left + wheelW + 4.0 * scale + bodyW : left;
        const double wy = top + row * (wheelH + gap);
        const auto k = static_cast<std::size_t>(i);
        const sim::HudLevel level = !g.wheelContact[k]                    ? sim::HudLevel::Muted
                                    : g.wheelSlip[k] >= sim::kSlipWarning ? sim::HudLevel::Warning
                                    : g.wheelSlip[k] >= sim::kSlipCaution ? sim::HudLevel::Caution
                                                                          : sim::HudLevel::Good;
        QColor c = colorFor(level);
        if (level == sim::HudLevel::Muted) {
            c.setAlpha(110);
        }
        p.setBrush(c);
        p.drawRect(QRectF(wx, wy, wheelW, wheelH));
    }
    p.restore();
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

namespace {

// Mission panel (left, under the position panel); returns its bottom edge.
double missionPanel(QPainter& p, QPointF at, const sim::HudMission& m, double maxWidth) {
    const QFontMetricsF fm(p.font());
    const double line = fm.height() * 1.1;
    std::vector<std::pair<QString, sim::HudLevel>> lines;
    lines.emplace_back(QString::fromStdString(m.title), sim::HudLevel::Normal);
    lines.emplace_back(QString::fromStdString(m.state.text + "   " + m.timer.text),
                       m.timer.level != sim::HudLevel::Normal ? m.timer.level : m.state.level);
    for (const sim::HudItem& o : m.objectives) {
        lines.emplace_back(QString::fromStdString(o.text), o.level);
    }
    for (const sim::HudItem& n : m.notes) {
        lines.emplace_back(QString::fromStdString(n.text), n.level);
    }
    double width = 0.0;
    for (auto& [text, level] : lines) {
        text = fm.elidedText(text, Qt::ElideRight, maxWidth);
        width = std::max(width, fm.horizontalAdvance(text));
    }
    const QRectF plate(at.x() - 6, at.y() - 4, width + 12,
                       line * static_cast<double>(lines.size()) + 8);
    p.setPen(Qt::NoPen);
    p.setBrush(QColor(0, 0, 0, 110));
    p.drawRoundedRect(plate, 4, 4);
    for (std::size_t i = 0; i < lines.size(); ++i) {
        // The state line uses the state's colour, the title is white.
        const QColor color =
            i == 1 && m.timer.level == sim::HudLevel::Normal ? colorFor(m.state.level)
                                                              : colorFor(lines[i].second);
        outlinedText(p, at, lines[i].first, color, Qt::AlignLeft);
        at.ry() += line;
    }
    return plate.bottom();
}

// Guidance marker: a diamond over the target with its label, or an arrow at the screen edge
// pointing towards it.
void marker(QPainter& p, const QRectF& area, const graphics::Camera& camera,
            const sim::HudMarker& m, double scale) {
    const double r = 7.0 * scale;
    const QRectF inner = area.adjusted(30 * scale, 30 * scale, -30 * scale, -30 * scale);
    const auto projected = projectToScreen(camera, area, m.position);
    const QColor color(90, 220, 255);
    if (projected && inner.contains(*projected)) {
        const QPointF c = *projected;
        QPolygonF diamond{{c.x(), c.y() - r}, {c.x() + r, c.y()}, {c.x(), c.y() + r},
                          {c.x() - r, c.y()}};
        p.setPen(QPen(QColor(0, 0, 0, 200), 3.0));
        p.setBrush(Qt::NoBrush);
        p.drawPolygon(diamond);
        p.setPen(QPen(color, 1.8));
        p.drawPolygon(diamond);
        outlinedText(p, {c.x(), c.y() + r + 2}, QString::fromStdString(m.label), color,
                     Qt::AlignHCenter);
        return;
    }
    // Off screen or behind: an arrow at the edge, on the side to turn to.
    QPointF dir;
    const glm::dvec3 rel = m.position - camera.position;
    const glm::vec4 v = camera.view() * glm::vec4(glm::vec3(rel), 0.0F);
    dir = QPointF(v.x, -v.y);  // Left/right and up/down as seen from the camera.
    const double len = std::hypot(dir.x(), dir.y());
    if (len < 1e-6) {
        dir = QPointF(0.0, 1.0);
    } else {
        dir /= len;
    }
    const QPointF c = inner.center();
    const double tx = std::abs(dir.x()) > 1e-6 ? 0.5 * inner.width() / std::abs(dir.x()) : 1e9;
    const double ty = std::abs(dir.y()) > 1e-6 ? 0.5 * inner.height() / std::abs(dir.y()) : 1e9;
    const QPointF tip = c + dir * std::min(tx, ty);
    const QPointF side(-dir.y(), dir.x());
    QPolygonF arrow{tip, tip - dir * (2.2 * r) + side * r, tip - dir * (2.2 * r) - side * r};
    p.setPen(QPen(QColor(0, 0, 0, 200), 2.0));
    p.setBrush(color);
    p.drawPolygon(arrow);
    const QPointF label = tip - dir * (3.5 * r);
    outlinedText(p, {label.x(), label.y() - 0.5 * QFontMetricsF(p.font()).height()},
                 QString::fromStdString(m.label), color, Qt::AlignHCenter);
}

}  // namespace

std::optional<QPointF> projectToScreen(const graphics::Camera& camera, const QRectF& area,
                                       const glm::dvec3& world) {
    const glm::vec4 clip =
        camera.viewProjection() * glm::vec4(glm::vec3(world - camera.position), 1.0F);
    if (clip.w <= 1e-4F) {
        return std::nullopt;
    }
    const double x = clip.x / clip.w;
    const double y = clip.y / clip.w;
    return QPointF(area.left() + (0.5 + 0.5 * x) * area.width(),
                   area.top() + (0.5 - 0.5 * y) * area.height());
}

void paintInsetFrame(QPainter& p, const QRectF& area, const QString& label, double scale) {
    p.save();
    p.setRenderHint(QPainter::Antialiasing, true);
    p.setRenderHint(QPainter::TextAntialiasing, true);
    p.setBrush(Qt::NoBrush);
    p.setPen(QPen(QColor(0, 0, 0, 160), 3.0));
    p.drawRect(area);
    p.setPen(QPen(QColor(235, 245, 255, 200), 1.0));
    p.drawRect(area);
    QFont font(QStringLiteral("monospace"));
    font.setStyleHint(QFont::Monospace);
    font.setBold(true);
    font.setPointSizeF(8.0 * scale);
    p.setFont(font);
    outlinedText(p, area.topLeft() + QPointF(5.0 * scale, 3.0 * scale), label,
                 QColor(235, 245, 255), Qt::AlignLeft);
    p.restore();
}

void paintMapMarker(QPainter& p, QPointF at, double headingDeg, double scale) {
    const double r = 11.0 * scale;
    QPainterPath arrow;
    arrow.moveTo(polar(at, headingDeg, r));
    arrow.lineTo(polar(at, headingDeg + 140.0, r));
    arrow.lineTo(polar(at, headingDeg + 180.0, 0.4 * r));
    arrow.lineTo(polar(at, headingDeg - 140.0, r));
    arrow.closeSubpath();
    p.save();
    p.setRenderHint(QPainter::Antialiasing, true);
    p.setBrush(Qt::NoBrush);  // The vehicle stays visible inside.
    p.setPen(QPen(QColor(0, 0, 0, 200), 3.5 * scale));
    p.drawPath(arrow);
    p.setPen(QPen(QColor(255, 210, 60), 1.5 * scale));
    p.drawPath(arrow);
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

    // Mission panel under the position panel; guidance markers.
    double leftColumnTop = area.center().y() - line;
    if (hud.mission) {
        const double top = area.top() + margin + 4.0 * line;
        const double bottom = missionPanel(p, {area.left() + margin, top}, *hud.mission,
                                           0.42 * area.width());
        leftColumnTop = std::max(leftColumnTop, bottom + 0.6 * line);
    }
    if (options.camera != nullptr) {
        for (const sim::HudMarker& m : hud.markers) {
            marker(p, area, *options.camera, m, options.scale);
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

    // Heights and climb, or a ground drone's attitude and slope (left middle); speeds,
    // distances and wind (right middle).
    const double middle = area.center().y() - line;
    const double leftTop = std::max(middle, leftColumnTop);
    if (hud.ground) {
        const sim::HudGround& g = *hud.ground;
        column(p, {area.left() + margin, leftTop}, {&g.pitch, &g.roll, &g.slope}, Qt::AlignLeft);
        groundGauges(p, {area.left() + margin, leftTop + 3.0 * line + 4.0 * options.scale}, g,
                     options.scale);
        column(p, {area.right() - margin, middle},
               {&hud.speed, &g.slip, &g.odometer, &hud.home, &hud.pilotDistance, &hud.wind},
               Qt::AlignRight);
    } else {
        column(p, {area.left() + margin, leftTop},
               {&hud.altitudeAgl, &hud.altitudeAmsl, &hud.verticalSpeed}, Qt::AlignLeft);
        column(p, {area.right() - margin, middle},
               {&hud.speed, &hud.home, &hud.pilotDistance, &hud.wind}, Qt::AlignRight);
    }
    if (hud.windArrowRad) {
        const double rows = hud.ground ? 7.0 : 5.0;
        windArrow(p, {area.right() - margin - 14.0 * options.scale, middle + rows * line},
                  12.0 * options.scale, *hud.windArrowRad);
    }

    // Vehicle state (bottom).
    const double bottom = area.bottom() - margin - options.bottomReserved - 3.0 * line;
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
