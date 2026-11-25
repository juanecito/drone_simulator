// SPDX-FileCopyrightText: 2026 Juan Maria Gomez Lopez <juan-maria.gomez-lopez@tutamail.com>
// SPDX-License-Identifier: MIT

#include "ds/app/hud_painter.hpp"

#include <QFont>
#include <QFontDatabase>
#include <QFontMetrics>
#include <QPainter>
#include <QPainterPath>
#include <QPolygonF>
#include <QStringList>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <numbers>
#include <system_error>
#include <vector>

#include "ds/app/asset_paths.hpp"
#include "ds/common/log.hpp"

namespace ds::app {
namespace {

// HUD palette: one green for text and symbols, a paler green to set some apart, amber and
// red only for cautions and warnings.
QColor green(int alpha = 255) { return {70, 200, 85, alpha}; }
QColor paleGreen(int alpha = 255) { return {150, 210, 150, alpha}; }
// Dark green halo around letters and symbols, so they stay readable over sky and snow.
QColor outlineColor() { return {0, 25, 0, 210}; }
// Translucent dark green behind panels.
QColor plateColor() { return {0, 32, 8, 150}; }

QColor colorFor(sim::HudLevel level) {
    switch (level) {
        case sim::HudLevel::Caution: return {255, 190, 40};
        case sim::HudLevel::Warning: return {255, 70, 60};
        case sim::HudLevel::Good: return paleGreen();
        case sim::HudLevel::Muted: return {40, 120, 52};
        case sim::HudLevel::Normal: break;
    }
    return green();
}

// The HUD font: the first of these files found in `fonts/` of the asset directory, loaded on
// first use, with how to draw with it; the system monospace font (bold) if none loads.
struct HudFace {
    QString family;
    bool segments = false;    ///< DSEG: text goes through segmentText().
    bool bold = false;        ///< Ask for the bold weight.
    double sizeFactor = 1.0;  ///< Point size relative to the requested one.
    double lineFactor = 1.1;  ///< Line distance in font heights.
    double outline = 3.0;     ///< Width of the dark outline, in pixels.
    double thicken = 0.0;     ///< Width of a stroke in the text colour that fattens thin glyphs.
};

const HudFace& hudFace() {
    static const HudFace face = [] {
        struct Candidate {
            const char* file;
            HudFace face;
        };
        const Candidate candidates[] = {
            // B612, designed for cockpit displays: plain text, a lighter outline.
            {"B612-Regular.ttf", {.lineFactor = 1.15, .outline = 2.5}},
            // DSEG14 Classic: its segment digits fill the whole cell, so a tenth smaller and
            // more leading.
            {"DSEG14Classic-Regular.ttf", {.segments = true, .sizeFactor = 0.9, .lineFactor = 1.4}},
        };
        const std::filesystem::path dir = findAssetDirectory() / "fonts";
        for (const Candidate& c : candidates) {
            const std::filesystem::path file = dir / c.file;
            std::error_code ec;
            if (!std::filesystem::is_regular_file(file, ec)) {
                continue;
            }
            const int id = QFontDatabase::addApplicationFont(QString::fromStdString(file.string()));
            const QStringList families =
                id >= 0 ? QFontDatabase::applicationFontFamilies(id) : QStringList{};
            if (families.isEmpty()) {
                log::warn("HUD font {} could not be loaded", file.string());
                continue;
            }
            HudFace chosen = c.face;
            chosen.family = families.front();
            return chosen;
        }
        log::warn("No HUD font in {}; using the system monospace font", dir.string());
        return HudFace{.family = QStringLiteral("monospace"), .bold = true};
    }();
    return face;
}

// Text as a 14-segment display shows it. DSEG draws lowercase as capitals; its space is a
// quarter of a cell, too narrow to part words, so each blank becomes two (half a cell); the
// dashes and bullets it lacks become "-" and ">". Other missing symbols (✓ ✗) come from a
// fallback font. Apply once: blanks double each time.
QString segmentText(const QString& text) {
    if (!hudFace().segments) {
        return text;
    }
    QString out;
    out.reserve(text.size() + text.count(u' '));
    for (const QChar c : text) {
        switch (c.unicode()) {
            case u' ': out += QStringLiteral("  "); break;
            case 0x2013:  // – en dash
            case 0x2014:  // — em dash
            case 0x2212:  // − minus
            case 0x00B7:  // · middle dot
                out += u'-';
                break;
            case 0x25B8: out += u'>'; break;  // ▸
            default: out += c; break;
        }
    }
    return out;
}

// Glyph outlines of @p shown (already through segmentText()) in @p font, anchored by @p align
// at @p anchor (top of the line).
QPainterPath textPath(const QFont& font, QPointF anchor, const QString& shown,
                      Qt::Alignment align) {
    const QFontMetricsF fm(font);
    const double w = fm.horizontalAdvance(shown);
    double x = anchor.x();
    if (align & Qt::AlignRight) {
        x -= w;
    } else if (align & Qt::AlignHCenter) {
        x -= 0.5 * w;
    }
    QPainterPath path;
    path.addText(QPointF(x, anchor.y() + fm.ascent()), font, shown);
    return path;
}

void drawOutlined(QPainter& p, const QPainterPath& path, const QColor& color) {
    const HudFace& face = hudFace();
    p.setPen(QPen(outlineColor(), face.outline + face.thicken, Qt::SolidLine, Qt::RoundCap,
                  Qt::RoundJoin));
    p.setBrush(Qt::NoBrush);
    p.drawPath(path);
    if (face.thicken > 0.0) {
        p.setPen(QPen(color, face.thicken, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    } else {
        p.setPen(Qt::NoPen);
    }
    p.setBrush(color);
    p.drawPath(path);
}

// Text with a dark outline, anchored by @p align at @p anchor.
void outlinedText(QPainter& p, QPointF anchor, const QString& text, const QColor& color,
                  Qt::Alignment align) {
    drawOutlined(p, textPath(p.font(), anchor, segmentText(text), align), color);
}

// Left-aligned lines (already through segmentText()) from @p at on a plate that encloses
// every glyph with its outline (sized from the drawn shapes, not from font metrics); returns
// the plate.
QRectF textPanel(QPainter& p, QPointF at, const std::vector<std::pair<QString, QColor>>& lines) {
    const double line = hudLineHeight(p.font());
    QRectF bounds(at, QSizeF(0.0, line * static_cast<double>(lines.size())));
    std::vector<QPainterPath> paths;
    paths.reserve(lines.size());
    for (const auto& [text, color] : lines) {
        paths.push_back(textPath(p.font(), at, text, Qt::AlignLeft));
        bounds = bounds.united(paths.back().boundingRect());
        at.ry() += line;
    }
    const double pad = 0.5 * (hudFace().outline + hudFace().thicken) + 5.0;
    const QRectF plate = bounds.adjusted(-pad, -pad, pad, pad);
    p.setPen(Qt::NoPen);
    p.setBrush(plateColor());
    p.drawRoundedRect(plate, 4, 4);
    for (std::size_t i = 0; i < paths.size(); ++i) {
        drawOutlined(p, paths[i], lines[i].second);
    }
    return plate;
}

// A column of items, top to bottom from @p anchor.
void column(QPainter& p, QPointF anchor, const std::vector<const sim::HudItem*>& items,
            Qt::Alignment align) {
    const double line = hudLineHeight(p.font());
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
    p.setPen(QPen(green(190), 1.5));
    const double half = 0.18 * area.width();
    p.drawLine(QPointF(-half, y0), QPointF(-0.25 * half, y0));
    p.drawLine(QPointF(0.25 * half, y0), QPointF(half, y0));
    // Pitch ladder every 10°.
    p.setPen(QPen(green(120), 1.0));
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
    p.setPen(QPen(green(230), 2.0));
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
    p.setPen(QPen(outlineColor(), 1.0));
    p.setBrush(plateColor());
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
    p.setPen(QPen(outlineColor(), 3.0 * scale));
    p.drawLine(QPointF(x, bar.top() - 3.0 * scale), QPointF(x, bar.bottom() + 3.0 * scale));
    p.setPen(QPen(colorFor(g.stability), 1.5 * scale));
    p.drawLine(QPointF(x, bar.top() - 3.0 * scale), QPointF(x, bar.bottom() + 3.0 * scale));
    p.setPen(QPen(green(160), 1.0));
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
    p.setPen(QPen(outlineColor(), 1.0));
    p.setBrush(green(60));
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
    p.setPen(QPen(outlineColor(), 2.0));
    p.setBrush(green(220));
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
    const QColor trueNorth = paleGreen();
    const QColor magneticNorth = paleGreen();
    const QColor ring = green(220);

    // Plate and ring.
    p.setPen(Qt::NoPen);
    p.setBrush(plateColor());
    p.drawEllipse(c, r, r);
    p.setBrush(Qt::NoBrush);
    p.setPen(QPen(ring, 1.5));
    p.drawEllipse(c, r, r);

    // Ticks: every 10°, longer every 30°, longest and pale at true north.
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
        outlinedText(p, at, QString::fromLatin1(k.text),
                     k.deg == 0.0 ? trueNorth : colorFor(sim::HudLevel::Normal), Qt::AlignHCenter);
    }

    // Magnetic north: pale marker outside the ring, turned by the declination.
    {
        const double d = compass.declinationDeg;
        QPainterPath marker;
        marker.moveTo(polar(c, d, r + 1.0));
        marker.lineTo(polar(c, d - 6.0, r + 0.25 * r));
        marker.lineTo(polar(c, d + 6.0, r + 0.25 * r));
        marker.closeSubpath();
        p.setPen(QPen(outlineColor(), 1.0));
        p.setBrush(magneticNorth);
        p.drawPath(marker);
        const QPointF label =
            polar(c, d, r + 0.25 * r + 0.55 * labelHeight) - QPointF(0.0, 0.5 * labelHeight);
        outlinedText(p, label, QStringLiteral("MN"), magneticNorth, Qt::AlignHCenter);
    }

    // Camera view direction (free camera).
    if (viewHeadingDeg) {
        p.setPen(QPen(green(170), 1.2, Qt::DashLine));
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
        p.setPen(QPen(outlineColor(), 1.5));
        p.setBrush(green());
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
    std::vector<std::pair<QString, QColor>> shown;
    shown.reserve(lines.size());
    for (std::size_t i = 0; i < lines.size(); ++i) {
        // The state line uses the state's colour, the title the normal text colour.
        const QColor color =
            i == 1 && m.timer.level == sim::HudLevel::Normal ? colorFor(m.state.level)
                                                              : colorFor(lines[i].second);
        shown.emplace_back(fm.elidedText(segmentText(lines[i].first), Qt::ElideRight, maxWidth),
                           color);
    }
    const QRectF plate = textPanel(p, at, shown);
    return plate.bottom();
}

// Guidance marker: a diamond over the target with its label, or an arrow at the screen edge
// pointing towards it.
void marker(QPainter& p, const QRectF& area, const graphics::Camera& camera,
            const sim::HudMarker& m, double scale) {
    const double r = 7.0 * scale;
    const QRectF inner = area.adjusted(30 * scale, 30 * scale, -30 * scale, -30 * scale);
    const auto projected = projectToScreen(camera, area, m.position);
    const QColor color = paleGreen();
    if (projected && inner.contains(*projected)) {
        const QPointF c = *projected;
        QPolygonF diamond{{c.x(), c.y() - r}, {c.x() + r, c.y()}, {c.x(), c.y() + r},
                          {c.x() - r, c.y()}};
        p.setPen(QPen(outlineColor(), 3.0));
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
    p.setPen(QPen(outlineColor(), 2.0));
    p.setBrush(color);
    p.drawPolygon(arrow);
    const QPointF label = tip - dir * (3.5 * r);
    outlinedText(p, {label.x(), label.y() - 0.5 * QFontMetricsF(p.font()).height()},
                 QString::fromStdString(m.label), color, Qt::AlignHCenter);
}

}  // namespace

QFont hudFont(double pointSize) {
    QFont font(hudFace().family);
    font.setStyleHint(QFont::Monospace);
    font.setBold(hudFace().bold);
    font.setPointSizeF(hudFace().sizeFactor * pointSize);
    return font;
}

double hudLineHeight(const QFont& font) {
    return QFontMetricsF(font).height() * hudFace().lineFactor;
}

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
    p.setPen(QPen(outlineColor(), 3.0));
    p.drawRect(area);
    // The light line unblended, so it keeps its colour on any pixel grid.
    p.setRenderHint(QPainter::Antialiasing, false);
    p.setPen(QPen(paleGreen(), 1.0));
    p.drawRect(area);
    p.setRenderHint(QPainter::Antialiasing, true);
    p.setFont(hudFont(8.0 * scale));
    outlinedText(p, area.topLeft() + QPointF(5.0 * scale, 3.0 * scale), label,
                 colorFor(sim::HudLevel::Normal), Qt::AlignLeft);
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
    p.setPen(QPen(outlineColor(), 3.5 * scale));
    p.drawPath(arrow);
    p.setPen(QPen(green(), 1.5 * scale));
    p.drawPath(arrow);
    p.restore();
}

void paintHud(QPainter& p, const QRect& rect, const sim::HudModel& hud,
              const HudPaintOptions& options) {
    p.save();
    p.setRenderHint(QPainter::Antialiasing, true);
    p.setRenderHint(QPainter::TextAntialiasing, true);
    const QFont font = hudFont(10.0 * options.scale);
    p.setFont(font);
    const QRectF area(rect);
    const double margin = 12.0 * options.scale;
    const double line = hudLineHeight(font);

    if (options.horizon) {
        horizon(p, area, hud);
    }

    // Position panel (top left, FR-GFX-14a) on a translucent plate.
    QRectF positionPlate;
    {
        const QColor color = colorFor(sim::HudLevel::Normal);
        std::vector<std::pair<QString, QColor>> lines;
        for (const std::string* text :
             {&hud.positionUtm, &hud.positionElevation, &hud.positionLatLon}) {
            if (!text->empty()) {
                lines.emplace_back(segmentText(QString::fromStdString(*text)), color);
            }
        }
        positionPlate = textPanel(p, {area.left() + margin, area.top() + margin}, lines);
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
           {&hud.clockLocal, &hud.clockUtc, &hud.timer, &hud.replay}, Qt::AlignRight);

    // Heading, true and magnetic (top centre); under the position panel if a narrow view
    // would make them overlap.
    double headingTop = area.top() + margin;
    {
        const QString text = segmentText(QString::fromStdString(hud.compass.heading.text));
        QPainterPath heading =
            textPath(font, {area.center().x(), headingTop}, text, Qt::AlignHCenter);
        if (heading.boundingRect().left() < positionPlate.right() + 0.5 * margin) {
            headingTop = positionPlate.bottom() + 0.3 * line;
            heading = textPath(font, {area.center().x(), headingTop}, text, Qt::AlignHCenter);
        }
        drawOutlined(p, heading, colorFor(hud.compass.heading.level));
    }

    // Warnings (under the heading, flashing).
    if (options.blinkOn) {
        QPointF at(area.center().x(), headingTop + 2.0 * line);
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
               {&hud.speed, &g.slip, &g.odometer, &hud.home, &hud.pilotDistance, &hud.wind,
                &hud.weather},
               Qt::AlignRight);
    } else {
        column(p, {area.left() + margin, leftTop},
               {&hud.altitudeAgl, &hud.altitudeAmsl, &hud.verticalSpeed}, Qt::AlignLeft);
        column(p, {area.right() - margin, middle},
               {&hud.speed, &hud.home, &hud.pilotDistance, &hud.wind, &hud.weather},
               Qt::AlignRight);
    }
    if (hud.windArrowRad) {
        const double rows = hud.ground ? 8.0 : 6.0;
        windArrow(p, {area.right() - margin - 14.0 * options.scale, middle + rows * line},
                  12.0 * options.scale, *hud.windArrowRad);
    }

    // Vehicle state (bottom).
    const double bottom = area.bottom() - margin - 3.0 * line;
    column(p, {area.left() + margin, bottom - options.bottomReservedLeft},
           {&hud.battery, &hud.gps, &hud.lights}, Qt::AlignLeft);
    column(p, {area.right() - margin, bottom - options.bottomReservedRight},
           {&hud.armed, &hud.mode, &hud.throttle}, Qt::AlignRight);

    // Compass rose (bottom centre, FR-GFX-14b).
    const double radius = compassRadius(options.scale);
    paintCompass(p, {area.center().x(), area.bottom() - margin - line - 4.0 - radius}, radius,
                 hud.compass);
    p.restore();
}

}  // namespace ds::app
