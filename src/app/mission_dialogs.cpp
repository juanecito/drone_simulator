// SPDX-FileCopyrightText: 2026 Juan Maria Gomez Lopez <juan-maria.gomez-lopez@tutamail.com>
// SPDX-License-Identifier: MIT

#include "ds/app/mission_dialogs.hpp"

#include <QComboBox>
#include <QCoreApplication>
#include <QDialogButtonBox>
#include <QFontMetricsF>
#include <QHBoxLayout>
#include <QLabel>
#include <QPainter>
#include <QPainterPath>
#include <QPushButton>
#include <QRegularExpression>
#include <QTextBrowser>
#include <QVBoxLayout>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <format>
#include <numbers>

#include "ds/app/mission_setup.hpp"
#include "ds/app/missions_controller.hpp"
#include "ds/app/missions_panel.hpp"
#include "ds/app/simulation_controller.hpp"
#include "ds/astro/events.hpp"
#include "ds/astro/sun_moon.hpp"
#include "ds/missions/flight_rules.hpp"
#include "ds/missions/mission_world.hpp"
#include "ds/terrain/procedural_source.hpp"

namespace ds::app {
namespace {

constexpr int kShadeSize = 192;

QString qs(const std::string& s) { return QString::fromStdString(s); }
QString html(const std::string& s) { return qs(s).toHtmlEscaped(); }
// Fixed decimals without trailing zeros: 2.50 → "2.5", 3.0 → "3", 100 → "100".
QString number(double v, int decimals = 1) {
    QString text = QString::number(v, 'f', decimals);
    if (text.contains(QLatin1Char('.'))) {
        text.remove(QRegularExpression(QStringLiteral("\\.?0+$")));
    }
    return text == QLatin1String("-0") ? QStringLiteral("0") : text;
}

QString tr(const char* text) {
    return QCoreApplication::translate("ds::app::MissionDialogs", text);
}

QString localClock(astro::UtcTime t, const std::string& zone) {
    if (auto local = astro::toLocalTime(t, zone.empty() ? "UTC" : zone)) {
        return qs(std::format("{:%H:%M}", std::chrono::floor<std::chrono::minutes>(local->time)));
    }
    return qs(std::format("{:%H:%M} UTC", std::chrono::floor<std::chrono::minutes>(t)));
}

QString moonPhaseName(const astro::MoonPhase& p) {
    const double k = p.illuminatedFraction;
    if (k < 0.03) {
        return tr("new moon");
    }
    if (k > 0.97) {
        return tr("full moon");
    }
    if (std::abs(k - 0.5) < 0.06) {
        return p.waxing ? tr("first quarter") : tr("last quarter");
    }
    if (k < 0.5) {
        return p.waxing ? tr("waxing crescent") : tr("waning crescent");
    }
    return p.waxing ? tr("waxing gibbous") : tr("waning gibbous");
}

QColor terrainColor(double t) {
    // Low green → tan → grey rock → snow.
    const std::array<std::pair<double, QColor>, 4> ramp{{{0.0, QColor(92, 135, 70)},
                                                         {0.45, QColor(160, 145, 100)},
                                                         {0.8, QColor(130, 125, 118)},
                                                         {1.0, QColor(235, 235, 240)}}};
    t = std::clamp(t, 0.0, 1.0);
    for (std::size_t i = 1; i < ramp.size(); ++i) {
        if (t <= ramp[i].first) {
            const auto f =
                static_cast<float>((t - ramp[i - 1].first) / (ramp[i].first - ramp[i - 1].first));
            const QColor& a = ramp[i - 1].second;
            const QColor& b = ramp[i].second;
            return QColor::fromRgbF(a.redF() + f * (b.redF() - a.redF()),
                                    a.greenF() + f * (b.greenF() - a.greenF()),
                                    a.blueF() + f * (b.blueF() - a.blueF()));
        }
    }
    return ramp.back().second;
}

// Outline of an item's shape on the ground (world x, z).
std::vector<glm::dvec2> footprint(const items::Item& item) {
    using K = items::Shape::Kind;
    const items::Shape& s = item.shape;
    std::vector<glm::dvec3> local;
    switch (s.kind) {
        case K::Prism:
            for (const items::Vec2& v : s.polygon) {
                local.emplace_back(v.x, 0.0, v.y);
            }
            break;
        case K::Box: {
            const glm::dvec3 o = s.offsetM;
            const glm::dvec3 h = s.halfExtentsM;
            local = {{o.x - h.x, 0.0, o.z - h.z},
                     {o.x + h.x, 0.0, o.z - h.z},
                     {o.x + h.x, 0.0, o.z + h.z},
                     {o.x - h.x, 0.0, o.z + h.z}};
            break;
        }
        case K::Sphere:
        case K::Cylinder:
            for (int i = 0; i < 32; ++i) {
                const double a = 2.0 * std::numbers::pi * i / 32.0;
                local.emplace_back(s.offsetM.x + s.radiusM * std::sin(a), 0.0,
                                   s.offsetM.z - s.radiusM * std::cos(a));
            }
            break;
        case K::Gate: local = {{-s.halfExtentsM.x, 0.0, 0.0}, {s.halfExtentsM.x, 0.0, 0.0}}; break;
        case K::Point: local = {{0.0, 0.0, 0.0}}; break;
    }
    std::vector<glm::dvec2> out;
    for (const glm::dvec3& p : local) {
        const glm::dvec3 w = item.transform.toWorld(p);
        out.emplace_back(w.x, w.z);
    }
    return out;
}

}  // namespace

// --- Map ------------------------------------------------------------------------------------

common::Result<MissionMapData> missionMapData(const missions::MissionDefinition& mission,
                                              DataServices* services) {
    auto config = missionTerrain(mission);
    if (!config) {
        return std::unexpected(config.error());
    }
    auto built = buildTerrain(*config, services);
    if (!built) {
        return std::unexpected(built.error());
    }
    MissionMapData data;
    data.terrain = built->source;
    data.extent = built->source->extent();
    missions::MissionPlacement placement;
    placement.origin = geo::LatLon{built->origin.latitudeDeg, built->origin.longitudeDeg};
    placement.ground = [src = data.terrain](double x, double z) { return src->height(x, z); };
    placement.originAltitudeM = built->origin.heightM;
    auto registry = missions::buildItems(mission, placement);
    if (!registry) {
        return std::unexpected(registry.error());
    }
    for (const items::Item* item : registry->items()) {
        data.items.push_back(*item);
    }
    missions::MissionPosition spawn = mission.spawn;
    spawn.altitudeRef = items::AltitudeRef::Agl;
    spawn.altitudeM = 0.0;
    auto at = missions::worldPosition(spawn, mission, placement);
    if (!at) {
        return std::unexpected(at.error());
    }
    data.spawn = *at;
    data.spawnHeadingDeg = mission.spawnHeadingDeg;
    data.pilot = data.spawn;
    for (const items::Item& item : data.items) {
        if (item.type == items::ItemType::PilotPosition) {
            data.pilot = item.transform.position;
            break;
        }
    }
    return data;
}

MissionMapWidget::MissionMapWidget(QWidget* parent) : QWidget(parent) {
    setMinimumSize(240, 240);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
}

void MissionMapWidget::setData(MissionMapData mapData) {
    data_ = std::move(mapData);
    // Area: everything of interest, padded, square.
    double x0 = data_.spawn.x;
    double x1 = x0;
    double z0 = data_.spawn.z;
    double z1 = z0;
    const auto include = [&](double x, double z) {
        x0 = std::min(x0, x);
        x1 = std::max(x1, x);
        z0 = std::min(z0, z);
        z1 = std::max(z1, z);
    };
    include(data_.pilot.x, data_.pilot.z);
    for (const items::Item& item : data_.items) {
        // Large keep-in / keep-out volumes would dwarf the mission: their centre only.
        if (item.type == items::ItemType::Geofence || item.type == items::ItemType::NoFlyZone) {
            include(item.transform.position.x, item.transform.position.z);
            continue;
        }
        for (const glm::dvec2& p : footprint(item)) {
            include(p.x, p.y);
        }
    }
    for (const glm::dvec3& p : data_.path) {
        include(p.x, p.z);
    }
    const double size = std::max({x1 - x0, z1 - z0, 80.0}) * 1.3;
    const double cx = 0.5 * (x0 + x1);
    const double cz = 0.5 * (z0 + z1);
    world_ = QRectF(cx - 0.5 * size, cz - 0.5 * size, size, size);

    // Shaded relief.
    shaded_ = QImage(kShadeSize, kShadeSize, QImage::Format_RGB32);
    if (data_.terrain) {
        const int n = kShadeSize + 1;
        std::vector<double> h(static_cast<std::size_t>(n * n));
        const double step = size / kShadeSize;
        data_.terrain->sampleGrid(world_.left(), world_.top(), step, n, n, h);
        const auto [lo, hi] = std::ranges::minmax(h);
        const double range = std::max(hi - lo, 1.0);
        const std::optional<double> water = data_.terrain->waterLevel();
        const glm::dvec3 light = glm::normalize(glm::dvec3(-1.0, 1.4, -1.0));  // From the NW.
        for (int r = 0; r < kShadeSize; ++r) {
            for (int c = 0; c < kShadeSize; ++c) {
                const auto at = [&](int rr, int cc) {
                    return h[static_cast<std::size_t>(rr * n + cc)];
                };
                const double z = at(r, c);
                const double dx = (at(r, c + 1) - z) / step;
                const double dz = (at(r + 1, c) - z) / step;
                const glm::dvec3 normal = glm::normalize(glm::dvec3(-dx, 1.0, -dz));
                const auto shade =
                    static_cast<float>(0.55 + 0.45 * std::max(glm::dot(normal, light), 0.0));
                QColor color =
                    water && z < *water ? QColor(60, 105, 160) : terrainColor((z - lo) / range);
                color = QColor::fromRgbF(color.redF() * shade, color.greenF() * shade,
                                         color.blueF() * shade);
                shaded_.setPixel(c, r, color.rgb());
            }
        }
    } else {
        shaded_.fill(QColor(90, 110, 80));
    }
    update();
}

QPointF MissionMapWidget::toScreen(const QRectF& target, double x, double z) const {
    return {target.left() + (x - world_.left()) / world_.width() * target.width(),
            target.top() + (z - world_.top()) / world_.height() * target.height()};
}

void MissionMapWidget::paintEvent(QPaintEvent* /*event*/) {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, true);
    const double side = std::min(width(), height());
    const QRectF target((width() - side) / 2.0, (height() - side) / 2.0, side, side);
    p.fillRect(rect(), palette().window());
    if (world_.isEmpty()) {
        return;
    }
    p.drawImage(target, shaded_);
    p.setClipRect(target);
    const double pxPerM = target.width() / world_.width();
    QFont font = p.font();
    font.setPointSizeF(std::max(7.0, font.pointSizeF() - 1.0));
    p.setFont(font);

    const auto polygon = [&](const items::Item& item) {
        QPolygonF poly;
        for (const glm::dvec2& v : footprint(item)) {
            poly << toScreen(target, v.x, v.y);
        }
        return poly;
    };
    const auto label = [&](QPointF at, const std::string& text, const QColor& color,
                           QPointF offset = QPointF(6, -4)) {
        QPainterPath path;
        path.addText(at + offset, p.font(), qs(text));
        p.setPen(QPen(QColor(0, 0, 0, 170), 3.0));
        p.setBrush(Qt::NoBrush);
        p.drawPath(path);
        p.setPen(Qt::NoPen);
        p.setBrush(color);
        p.drawPath(path);
    };
    using T = items::ItemType;
    for (const items::Item& item : data_.items) {
        const QPointF c = toScreen(target, item.transform.position.x, item.transform.position.z);
        const std::string name = item.name.empty() ? item.key : item.name;
        switch (item.type) {
            case T::NoFlyZone:
                p.setPen(QPen(QColor(230, 40, 30), 2.0));
                p.setBrush(QColor(230, 40, 30, 70));
                p.drawPolygon(polygon(item));
                break;
            case T::Geofence:
                p.setPen(QPen(QColor(240, 170, 30), 2.0, Qt::DashLine));
                p.setBrush(Qt::NoBrush);
                p.drawPolygon(polygon(item));
                break;
            case T::SurveyArea:
                p.setPen(QPen(QColor(250, 230, 40), 2.0, Qt::DashLine));
                p.setBrush(QColor(250, 230, 40, 50));
                p.drawPolygon(polygon(item));
                label(c, name, QColor(250, 230, 40));
                break;
            case T::PickupZone:
            case T::DropZone: {
                const QColor color =
                    item.type == T::PickupZone ? QColor(60, 220, 90) : QColor(80, 140, 255);
                p.setPen(QPen(color, 2.0));
                p.setBrush(QColor(color.red(), color.green(), color.blue(), 60));
                p.drawPolygon(polygon(item));
                label(c, name, color);
                break;
            }
            case T::ParkingZone:
                p.setPen(QPen(Qt::white, 2.0));
                p.setBrush(Qt::NoBrush);
                p.drawPolygon(polygon(item));
                break;
            case T::Obstacle:
            case T::Ramp:
            case T::Bridge:
                p.setPen(QPen(QColor(40, 40, 40), 1.0));
                p.setBrush(QColor(110, 110, 110));
                p.drawPolygon(polygon(item));
                break;
            case T::LandingPad: {
                const double r = std::max(item.shape.radiusM * pxPerM, 5.0);
                p.setPen(QPen(QColor(250, 210, 40), 2.0));
                p.setBrush(QColor(30, 30, 30));
                p.drawEllipse(c, r, r);
                label(c, name, QColor(250, 210, 40));
                break;
            }
            case T::Waypoint: {
                const double r = std::max(item.shape.radiusM * pxPerM, 4.0);
                p.setPen(QPen(QColor(60, 200, 255), 2.0));
                p.setBrush(QColor(60, 200, 255, 60));
                p.drawEllipse(c, r, r);
                label(c, name, QColor(60, 200, 255));
                break;
            }
            case T::Gate: {
                const QPolygonF ends = polygon(item);
                const glm::dvec3 ahead = item.transform.toWorld(
                    {0.0, 0.0, -std::max(item.shape.halfExtentsM.x, 6.0 / pxPerM)});
                p.setPen(QPen(QColor(255, 110, 20), 3.0));
                p.drawLine(ends.front(), ends.back());
                p.setPen(QPen(QColor(255, 110, 20), 1.5));
                p.drawLine(c, toScreen(target, ahead.x, ahead.z));
                label(c, name, QColor(255, 140, 60));
                break;
            }
            case T::InspectionTarget:
                p.setPen(QPen(Qt::black, 1.0));
                p.setBrush(QColor(240, 110, 20));
                p.drawEllipse(c, 4.0, 4.0);
                label(c, name, QColor(240, 130, 50));
                break;
            case T::Payload:
                p.setPen(QPen(Qt::black, 1.0));
                p.setBrush(QColor(140, 90, 40));
                p.drawRect(QRectF(c.x() - 3, c.y() - 3, 6, 6));
                break;
            case T::PilotPosition:
            case T::Spawn:
            case T::Drone: break;
        }
    }
    // Flight path: green start, red end.
    if (data_.path.size() >= 2) {
        QPolygonF line;
        for (const glm::dvec3& v : data_.path) {
            line << toScreen(target, v.x, v.z);
        }
        p.setPen(QPen(QColor(0, 0, 0, 150), 4.0));
        p.setBrush(Qt::NoBrush);
        p.drawPolyline(line);
        p.setPen(QPen(QColor(255, 240, 80), 2.0));
        p.drawPolyline(line);
        p.setPen(Qt::NoPen);
        p.setBrush(QColor(220, 40, 40));
        p.drawEllipse(line.back(), 4.0, 4.0);
    }
    // Take-off point with its heading, and the pilot.
    const QPointF s = toScreen(target, data_.spawn.x, data_.spawn.z);
    const double a = data_.spawnHeadingDeg * std::numbers::pi / 180.0;
    const QPointF fwd(std::sin(a), -std::cos(a));
    const QPointF right(-fwd.y(), fwd.x());
    p.setPen(QPen(Qt::black, 1.0));
    p.setBrush(QColor(60, 230, 90));
    p.drawPolygon(
        QPolygonF{s + 9.0 * fwd, s - 6.0 * fwd + 6.0 * right, s - 6.0 * fwd - 6.0 * right});
    const QPointF pilot = toScreen(target, data_.pilot.x, data_.pilot.z);
    p.setBrush(QColor(40, 90, 230));
    p.drawEllipse(pilot, 4.0, 4.0);
    // Below-left: the pilot usually stands next to the take-off pad, labelled above-right.
    const QString pilotText = tr("Pilot");
    label(pilot, pilotText.toStdString(), QColor(140, 170, 255),
          QPointF(-6.0 - QFontMetricsF(p.font()).horizontalAdvance(pilotText), 14.0));

    // North arrow and scale bar.
    p.setClipping(false);
    const QPointF n(target.right() - 18, target.top() + 26);
    p.setPen(QPen(Qt::black, 1.0));
    p.setBrush(Qt::white);
    p.drawPolygon(QPolygonF{n + QPointF(0, -14), n + QPointF(6, 4), n + QPointF(-6, 4)});
    label(n + QPointF(-10, 20), "N", Qt::white);
    double bar = std::pow(10.0, std::floor(std::log10(world_.width() / 4.0)));
    for (const double f : {5.0, 2.0}) {
        if (bar * f <= world_.width() / 4.0) {
            bar *= f;
            break;
        }
    }
    const double barPx = bar * pxPerM;
    const QPointF b(target.left() + 12, target.bottom() - 14);
    p.setPen(QPen(Qt::black, 4.0));
    p.drawLine(b, b + QPointF(barPx, 0));
    p.setPen(QPen(Qt::white, 2.0));
    p.drawLine(b, b + QPointF(barPx, 0));
    label(b + QPointF(barPx - 4, 2),
          bar >= 1000.0 ? std::format("{:g} km", bar / 1000.0) : std::format("{:g} m", bar),
          Qt::white);
}

// --- Texts ----------------------------------------------------------------------------------

QString daylightHtml(const missions::MissionDefinition& m) {
    auto terrainConfig = missionTerrain(m);
    if (!terrainConfig) {
        return {};
    }
    geo::Geodetic observer = terrainConfig->origin;
    if (terrainConfig->kind == TerrainKind::File) {
        // The origin of file terrain comes from the data unless the mission gives it.
        const FileTerrainConfig& f = terrainConfig->file;
        if (f.origin && f.originAltitudeM) {
            observer = {f.origin->latitudeDeg, f.origin->longitudeDeg, *f.originAltitudeM};
        } else if (auto built = buildTerrain(*terrainConfig)) {
            observer = built->origin;
        } else {
            return {};
        }
    }
    const astro::UtcTime t = m.environment.utc;
    const std::string& zone = m.environment.timeZone;
    const astro::BodyPosition sun = astro::sunPosition(t, observer);
    const astro::BodyPosition moon = astro::moonPosition(t, observer);
    const astro::MoonPhase phase = astro::moonPhase(sun, moon, observer.latitudeDeg);
    const double elevation = sun.apparent.elevationDeg;

    QString situation;
    bool night = false;
    if (elevation > missions::kSunsetElevationDeg) {
        situation = tr("Day — the sun is %1° above the horizon").arg(number(elevation, 0));
    } else if (elevation > missions::kCivilTwilightElevationDeg) {
        situation =
            tr("Civil twilight — the sun is %1° below the horizon").arg(number(-elevation, 0));
    } else {
        night = true;
        situation = tr("Night");
    }
    QStringList lines{situation};
    const astro::SunEvents sunDay = astro::sunEvents(t - std::chrono::hours(12), observer);
    if (sunDay.alwaysUp) {
        lines << tr("The sun does not set (polar day)");
    } else if (sunDay.alwaysDown) {
        lines << tr("The sun does not rise (polar night)");
    } else {
        QStringList events;
        if (sunDay.sunrise) {
            events << tr("sunrise %1").arg(localClock(*sunDay.sunrise, zone));
        }
        if (sunDay.sunset) {
            events << tr("sunset %1").arg(localClock(*sunDay.sunset, zone));
        }
        if (!events.isEmpty()) {
            QString text = events.join(QStringLiteral(", "));
            text[0] = text[0].toUpper();
            lines << text;
        }
    }
    QString moonLine = tr("Moon %1 % (%2)")
                           .arg(number(100.0 * phase.illuminatedFraction, 0), moonPhaseName(phase));
    const astro::MoonEvents moonDay = astro::moonEvents(t, observer);
    if (moon.apparent.elevationDeg > 0.0) {
        moonLine += tr(", up");
        if (moonDay.moonset) {
            moonLine += tr(", sets %1").arg(localClock(*moonDay.moonset, zone));
        }
    } else {
        moonLine += tr(", below the horizon");
        if (moonDay.moonrise) {
            moonLine += tr(", rises %1").arg(localClock(*moonDay.moonrise, zone));
        }
    }
    lines << moonLine;
    QString out = QStringLiteral("<p><b>%1</b></p><ul>").arg(tr("Day and night"));
    for (const QString& l : lines) {
        out += QStringLiteral("<li>%1</li>").arg(l);
    }
    out += QStringLiteral("</ul>");
    if (night) {
        out += QStringLiteral("<p><b>%1</b></p>")
                   .arg(tr("⚠ Night mission: keep the vehicle lights on (L) — without them the "
                           "drone is not in visual line of sight."));
    }
    if (m.rules.requireLightsOn) {
        out += QStringLiteral("<p>%1</p>").arg(tr("The vehicle lights must stay on."));
    }
    if (m.rules.daylightOnly) {
        out += QStringLiteral("<p>%1</p>")
                   .arg(tr("Flying is only allowed in daylight and civil twilight."));
    }
    if (m.rules.endAt != missions::EndAt::Never) {
        out +=
            QStringLiteral("<p>%1</p>")
                .arg(m.rules.endAt == missions::EndAt::Sunset ? tr("The mission ends at sunset.")
                                                              : tr("The mission ends at sunrise."));
    }
    return out;
}

QString debriefHtml(const missions::MissionDefinition& m, const missions::MissionResult& r,
                    const std::optional<storage::MissionAttemptRecord>& best) {
    using missions::MissionState;
    QString out;
    switch (r.outcome) {
        case MissionState::Succeeded:
            out += QStringLiteral("<h2>%1 <span style='color:#d9a400'>%2</span></h2>")
                       .arg(tr("Mission succeeded"), starsText(r.stars));
            break;
        case MissionState::Aborted:
            out += QStringLiteral("<h2>%1</h2>").arg(tr("Mission aborted"));
            break;
        default:
            out += QStringLiteral("<h2>%1</h2><p><b>%2</b></p>")
                       .arg(tr("Mission failed"), html(r.failureReason));
            break;
    }
    out += QStringLiteral("<p>%1 · %2 · %3</p>")
               .arg(tr("Time %1").arg(durationText(r.durationS)),
                    tr("battery %1 %").arg(number(r.batteryPct, 0)),
                    tr("score %1").arg(number(r.score.total, 0)));

    // Objectives.
    out += QStringLiteral("<p><b>%1</b></p><table cellspacing='4'>").arg(tr("Objectives"));
    for (const missions::ObjectiveStatus& o : r.objectives) {
        QString state;
        switch (o.state) {
            case missions::ObjectiveState::Completed: state = QStringLiteral("✓"); break;
            case missions::ObjectiveState::Failed: state = QStringLiteral("✗"); break;
            default: state = QStringLiteral("·"); break;
        }
        QStringList facts;
        if (o.completedAtS) {
            facts << durationText(*o.completedAtS);
        }
        if (o.errorM) {
            facts << tr("error %1 m").arg(number(*o.errorM, 2));
        }
        if (!o.detail.empty() && o.state != missions::ObjectiveState::Completed) {
            facts << html(o.detail);
        }
        out += QStringLiteral("<tr><td>%1</td><td>%2%3</td><td>%4</td></tr>")
                   .arg(state, html(o.title), o.optional ? tr(" (bonus)") : QString(),
                        facts.join(QStringLiteral(", ")));
    }
    out += QStringLiteral("</table>");

    // Score breakdown (FR-MIS-10).
    const missions::ScoreBreakdown& s = r.score;
    out += QStringLiteral("<p><b>%1</b></p><table cellspacing='4'>").arg(tr("Score"));
    for (const auto& [name, value] :
         {std::pair{tr("Completion"), s.completion}, std::pair{tr("Time"), s.time},
          std::pair{tr("Precision"), s.precision}, std::pair{tr("Battery"), s.battery},
          std::pair{tr("Smoothness"), s.smoothness}, std::pair{tr("Bonus"), s.bonus},
          std::pair{tr("Penalties"), s.penalties}}) {
        out += QStringLiteral("<tr><td>%1</td><td align='right'>%2</td></tr>")
                   .arg(name, number(value, 0));
    }
    out += QStringLiteral("<tr><td><b>%1</b></td><td align='right'><b>%2</b></td></tr></table>")
               .arg(tr("Total"), number(s.total, 0));
    if (!r.penalties.empty()) {
        out += QStringLiteral("<ul>");
        for (const std::string& p : r.penalties) {
            out += QStringLiteral("<li>%1</li>").arg(html(p));
        }
        out += QStringLiteral("</ul>");
    }
    if (r.worstErrorM) {
        out += QStringLiteral("<p>%1</p>")
                   .arg(tr("Worst position error %1 m").arg(number(*r.worstErrorM, 2)));
    }

    // Personal best.
    if (best) {
        const bool better =
            r.outcome == MissionState::Succeeded &&
            (r.stars > best->stars || (r.stars == best->stars && r.score.total > best->score));
        out += QStringLiteral("<p><b>%1</b> %2 · %3 · %4%5</p>")
                   .arg(tr("Personal best:"), starsText(best->stars),
                        tr("score %1").arg(number(best->score, 0)), durationText(best->durationS),
                        better ? QStringLiteral(" — <b>%1</b>").arg(tr("new personal best!"))
                               : QString());
    } else if (r.outcome == MissionState::Succeeded) {
        out += QStringLiteral("<p><b>%1</b></p>").arg(tr("First success — new personal best!"));
    }
    (void)m;
    return out;
}

// --- Briefing ------------------------------------------------------------------------------

MissionBriefingDialog::MissionBriefingDialog(const MissionRow& row,
                                             const missions::CatalogEntry& entry,
                                             const QList<VehicleProfileInfo>& profiles,
                                             bool keyboardOnly, DataServices* services,
                                             QWidget* parent)
    : QDialog(parent) {
    const missions::MissionDefinition& m = *entry.load.definition;
    setWindowTitle(tr("Briefing — %1").arg(qs(m.title)));
    setObjectName(QStringLiteral("MissionBriefingDialog"));
    resize(900, 600);

    auto* map = new MissionMapWidget;
    map->setObjectName(QStringLiteral("BriefingMap"));
    if (auto mapData = missionMapData(m, services)) {
        map->setData(std::move(*mapData));
    }
    text_ = new QTextBrowser;
    text_->setObjectName(QStringLiteral("BriefingText"));
    text_->setOpenLinks(false);
    text_->setHtml(missionDetailsHtml(row, entry) + daylightHtml(m));

    auto* split = new QHBoxLayout;
    split->addWidget(map, 1);
    split->addWidget(text_, 1);

    // Vehicle.
    auto* vehicleRow = new QHBoxLayout;
    vehicleRow->addWidget(new QLabel(tr("Vehicle:")));
    QString missionProfileName = qs(m.vehicle.profile);
    for (const VehicleProfileInfo& p : profiles) {
        if (p.key == qs(m.vehicle.profile)) {
            missionProfileName = p.name;
        }
    }
    if (!m.vehicle.customProfile.is_null()) {
        missionProfileName = tr("the mission's own drone");
    }
    if (m.vehicle.allowProfileChange) {
        profile_ = new QComboBox;
        profile_->setObjectName(QStringLiteral("BriefingProfile"));
        profile_->addItem(tr("%1 (mission default)").arg(missionProfileName), QString());
        for (const VehicleProfileInfo& p : profiles) {
            if (p.key != qs(m.vehicle.profile)) {
                profile_->addItem(p.name, p.key);
            }
        }
        vehicleRow->addWidget(profile_, 1);
    } else {
        vehicleRow->addWidget(new QLabel(missionProfileName), 1);
    }

    auto* buttons = new QDialogButtonBox;
    fly_ = buttons->addButton(tr("&Fly"), QDialogButtonBox::AcceptRole);
    fly_->setObjectName(QStringLiteral("FlyButton"));
    fly_->setDefault(true);
    buttons->addButton(QDialogButtonBox::Cancel);
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

    auto* layout = new QVBoxLayout(this);
    layout->addLayout(split, 1);
    layout->addLayout(vehicleRow);
    if (!m.rules.allowKeyboard && keyboardOnly) {
        auto* warning = new QLabel(
            tr("This mission cannot be flown with the keyboard. Set up a transmitter or gamepad "
               "in the Controls tab first."));
        warning->setObjectName(QStringLiteral("BriefingInputWarning"));
        warning->setWordWrap(true);
        warning->setStyleSheet(QStringLiteral("color: #c0392b; font-weight: bold;"));
        layout->addWidget(warning);
        fly_->setEnabled(false);
    }
    auto* hint =
        new QLabel(tr("After Fly the drone waits on the take-off point: arm it (Space or "
                      "the arm switch) to start the clock."));
    hint->setWordWrap(true);
    layout->addWidget(hint);
    layout->addWidget(buttons);
}

QString MissionBriefingDialog::chosenProfile() const {
    return profile_ != nullptr ? profile_->currentData().toString() : QString();
}

// --- Debrief -------------------------------------------------------------------------------

MissionDebriefDialog::MissionDebriefDialog(const missions::MissionDefinition& mission,
                                           const missions::MissionResult& result,
                                           std::vector<glm::dvec3> path,
                                           const std::optional<storage::MissionAttemptRecord>& best,
                                           bool hasNext, DataServices* services, QWidget* parent)
    : QDialog(parent) {
    setWindowTitle(tr("Debrief — %1").arg(qs(mission.title)));
    setObjectName(QStringLiteral("MissionDebriefDialog"));
    resize(900, 560);

    auto* map = new MissionMapWidget;
    map->setObjectName(QStringLiteral("DebriefMap"));
    if (auto mapData = missionMapData(mission, services)) {
        mapData->path = std::move(path);
        map->setData(std::move(*mapData));
    }
    text_ = new QTextBrowser;
    text_->setObjectName(QStringLiteral("DebriefText"));
    text_->setHtml(debriefHtml(mission, result, best));
    auto* split = new QHBoxLayout;
    split->addWidget(map, 1);
    split->addWidget(text_, 1);

    retry_ = new QPushButton(tr("&Retry"));
    retry_->setObjectName(QStringLiteral("RetryButton"));
    next_ = new QPushButton(tr("&Next Mission"));
    next_->setObjectName(QStringLiteral("NextButton"));
    next_->setEnabled(hasNext);
    back_ = new QPushButton(tr("&Back to Free Flight"));
    back_->setObjectName(QStringLiteral("BackButton"));
    (result.outcome == missions::MissionState::Succeeded && hasNext ? next_ : retry_)
        ->setDefault(true);
    connect(retry_, &QPushButton::clicked, this, [this] {
        choice_ = Choice::Retry;
        accept();
    });
    connect(next_, &QPushButton::clicked, this, [this] {
        choice_ = Choice::Next;
        accept();
    });
    connect(back_, &QPushButton::clicked, this, [this] {
        choice_ = Choice::Back;
        accept();
    });
    auto* buttons = new QHBoxLayout;
    buttons->addStretch(1);
    buttons->addWidget(back_);
    buttons->addWidget(retry_);
    buttons->addWidget(next_);

    auto* layout = new QVBoxLayout(this);
    layout->addLayout(split, 1);
    layout->addLayout(buttons);
}

QPushButton* MissionDebriefDialog::button(Choice choice) const noexcept {
    switch (choice) {
        case Choice::Retry: return retry_;
        case Choice::Next: return next_;
        case Choice::Back: break;
    }
    return back_;
}

}  // namespace ds::app
