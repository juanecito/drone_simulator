// SPDX-FileCopyrightText: 2026 Juan Maria Gomez Lopez <juan-maria.gomez-lopez@tutamail.com>
// SPDX-License-Identifier: MIT

#include "ds/sim/item_visuals.hpp"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace ds::sim {
namespace {

using graphics::Mesh;
using graphics::MeshMaterial;
using graphics::MeshVertex;
using items::ItemType;
using items::Shape;

constexpr float kTwoPi = 2.0F * std::numbers::pi_v<float>;
constexpr int kSegments = 32;
// Marker emission (cd/m²): visible at dusk and at night, subtle in daylight.
constexpr float kMarkerGlow = 150.0F;

// Colours (linear albedo).
constexpr glm::vec3 kPadColor{0.06F};
constexpr glm::vec3 kWhite{0.8F};
constexpr glm::vec3 kYellow{0.8F, 0.6F, 0.04F};
constexpr glm::vec3 kGateColor{0.9F, 0.25F, 0.02F};
constexpr glm::vec3 kWaypointColor{0.1F, 0.6F, 0.9F};
constexpr glm::vec3 kPickupColor{0.1F, 0.75F, 0.2F};
constexpr glm::vec3 kDropColor{0.15F, 0.35F, 0.95F};
constexpr glm::vec3 kNoFlyColor{0.9F, 0.08F, 0.04F};
constexpr glm::vec3 kGeofenceColor{0.9F, 0.6F, 0.08F};
constexpr glm::vec3 kSurveyColor{0.9F, 0.85F, 0.1F};
constexpr glm::vec3 kConcrete{0.35F, 0.34F, 0.32F};
constexpr glm::vec3 kWall{0.5F, 0.47F, 0.42F};
constexpr glm::vec3 kRoof{0.25F, 0.12F, 0.08F};
constexpr glm::vec3 kBark{0.18F, 0.11F, 0.05F};
constexpr glm::vec3 kLeaves{0.05F, 0.22F, 0.04F};
constexpr glm::vec3 kCardboard{0.4F, 0.24F, 0.1F};
constexpr glm::vec3 kTargetColor{0.85F, 0.35F, 0.05F};
constexpr glm::vec3 kPilotColor{0.08F, 0.15F, 0.45F};
constexpr glm::vec3 kSkin{0.6F, 0.42F, 0.3F};
constexpr glm::vec3 kCable{0.02F};

std::string propertyString(const common::Json& p, const char* key, const char* fallback) {
    if (p.is_object() && p.contains(key) && p[key].is_string()) {
        return p[key].get<std::string>();
    }
    return fallback;
}

void addQuad(Mesh& m, const glm::vec3& a, const glm::vec3& b, const glm::vec3& c,
             const glm::vec3& d, const MeshMaterial& mat) {
    const glm::vec3 n = glm::normalize(glm::cross(b - a, d - a));
    const auto base = static_cast<std::uint32_t>(m.vertices.size());
    const glm::vec4 color(mat.albedo, mat.opacity);
    for (const glm::vec3& p : {a, b, c, d}) {
        m.vertices.push_back(MeshVertex{p, n, color, mat.emissive});
    }
    m.indices.insert(m.indices.end(), {base, base + 1, base + 2, base, base + 2, base + 3});
}

// Vertical walls along a closed outline (x, z) from @p bottom to @p top, facing both ways.
void appendWalls(Mesh& m, const std::vector<glm::vec2>& outline, float bottom, float top,
                 const MeshMaterial& mat) {
    for (std::size_t i = 0; i < outline.size(); ++i) {
        const glm::vec2 p = outline[i];
        const glm::vec2 q = outline[(i + 1) % outline.size()];
        const glm::vec3 a(p.x, bottom, p.y);
        const glm::vec3 b(q.x, bottom, q.y);
        const glm::vec3 c(q.x, top, q.y);
        const glm::vec3 d(p.x, top, p.y);
        addQuad(m, a, b, c, d, mat);
        addQuad(m, b, a, d, c, mat);
    }
}

std::vector<glm::vec2> circle(float radius, int segments = kSegments) {
    std::vector<glm::vec2> out;
    for (int i = 0; i < segments; ++i) {
        const float a = kTwoPi * static_cast<float>(i) / static_cast<float>(segments);
        out.emplace_back(radius * std::sin(a), -radius * std::cos(a));
    }
    return out;
}

// Flat ring (annulus) at height @p y, both faces.
void appendRing(Mesh& m, float inner, float outer, float y, const MeshMaterial& mat) {
    const auto in = circle(inner);
    const auto out = circle(outer);
    for (std::size_t i = 0; i < in.size(); ++i) {
        const std::size_t j = (i + 1) % in.size();
        const glm::vec3 a(in[i].x, y, in[i].y);
        const glm::vec3 b(out[i].x, y, out[i].y);
        const glm::vec3 c(out[j].x, y, out[j].y);
        const glm::vec3 d(in[j].x, y, in[j].y);
        addQuad(m, a, d, c, b, mat);  // Up.
        addQuad(m, a, b, c, d, mat);  // Down.
    }
}

// Outline of a polygon as flat stripes along its edges at height @p y.
void appendOutline(Mesh& m, const std::vector<glm::vec2>& outline, float width, float y,
                   const MeshMaterial& mat) {
    for (std::size_t i = 0; i < outline.size(); ++i) {
        const glm::vec2 p = outline[i];
        const glm::vec2 q = outline[(i + 1) % outline.size()];
        const glm::vec2 d = q - p;
        const float len = glm::length(d);
        if (len < 1e-4F) {
            continue;
        }
        const glm::vec2 c = 0.5F * (p + q);
        const float angle = std::atan2(d.x, d.y);
        graphics::appendBox(m, glm::vec3(c.x, y, c.y),
                            glm::vec3(0.5F * width, 0.01F, 0.5F * (len + width)), mat,
                            glm::angleAxis(angle, glm::vec3(0.0F, 1.0F, 0.0F)));
    }
}

// Vertical extent of an open volume, clamped for drawing.
std::pair<float, float> drawnRange(const Shape& s) {
    const double bottom = std::max(s.bottomM, -5.0);
    const double top = std::min(s.topM, kOpenZoneHeightM);
    return {static_cast<float>(bottom), static_cast<float>(std::max(top, bottom + 0.5))};
}

std::vector<glm::vec2> outlineOf(const Shape& s) {
    if (s.kind == Shape::Kind::Prism) {
        std::vector<glm::vec2> out;
        for (const items::Vec2& v : s.polygon) {
            out.emplace_back(static_cast<float>(v.x), static_cast<float>(v.y));
        }
        return out;
    }
    if (s.kind == Shape::Kind::Box) {
        const auto hx = static_cast<float>(s.halfExtentsM.x);
        const auto hz = static_cast<float>(s.halfExtentsM.z);
        const auto ox = static_cast<float>(s.offsetM.x);
        const auto oz = static_cast<float>(s.offsetM.z);
        return {{ox - hx, oz - hz}, {ox + hx, oz - hz}, {ox + hx, oz + hz}, {ox - hx, oz + hz}};
    }
    return circle(static_cast<float>(s.radiusM));
}

std::shared_ptr<Mesh> landingPad(const Shape& s) {
    auto m = std::make_shared<Mesh>();
    const auto r = static_cast<float>(s.radiusM);
    const float y = 0.03F;
    graphics::appendCylinder(*m, glm::vec3(0.0F, -0.05F, 0.0F), glm::vec3(0.0F, y, 0.0F), r,
                             kSegments, {kPadColor});
    appendRing(*m, 0.82F * r, 0.92F * r, y + 0.005F, {kYellow, 1.0F, kMarkerGlow});
    // "H", 0.9 r tall, facing the pad's forward direction.
    const float h = 0.45F * r;
    const float w = 0.07F * r;
    const MeshMaterial white{kWhite, 1.0F, kMarkerGlow};
    graphics::appendBox(*m, glm::vec3(-0.3F * r, y + 0.005F, 0.0F), glm::vec3(w, 0.004F, h), white);
    graphics::appendBox(*m, glm::vec3(0.3F * r, y + 0.005F, 0.0F), glm::vec3(w, 0.004F, h), white);
    graphics::appendBox(*m, glm::vec3(0.0F, y + 0.005F, 0.0F), glm::vec3(0.3F * r, 0.004F, w),
                        white);
    return m;
}

std::shared_ptr<Mesh> gate(const items::Item& item) {
    auto m = std::make_shared<Mesh>();
    const auto hw = static_cast<float>(item.shape.halfExtentsM.x);
    const auto hh = static_cast<float>(item.shape.halfExtentsM.y);
    const auto t = static_cast<float>(items::gateFrameThicknessM(hw, hh));
    const MeshMaterial frame{kGateColor, 1.0F, kMarkerGlow};
    graphics::appendBox(*m, glm::vec3(0.0F, hh + 0.5F * t, 0.0F),
                        glm::vec3(hw + t, 0.5F * t, 0.5F * t), frame);
    graphics::appendBox(*m, glm::vec3(0.0F, -hh - 0.5F * t, 0.0F),
                        glm::vec3(hw + t, 0.5F * t, 0.5F * t), frame);
    // Side posts down to the ground when the height above it is known.
    const float below = item.altitudeRef == items::AltitudeRef::Agl
                            ? std::max(static_cast<float>(item.altitudeM) - hh - t, 0.0F)
                            : 0.0F;
    for (const float x : {-hw - 0.5F * t, hw + 0.5F * t}) {
        const float bottom = -hh - t - below;
        const float top = hh + t;
        graphics::appendBox(*m, glm::vec3(x, 0.5F * (bottom + top), 0.0F),
                            glm::vec3(0.5F * t, 0.5F * (top - bottom), 0.5F * t), frame);
    }
    // Arrow on the top bar pointing through the gate (local −z).
    graphics::appendBox(*m, glm::vec3(0.0F, hh + 1.02F * t, -0.6F * t),
                        glm::vec3(0.15F * hw, 0.02F * t, 0.6F * t), {kWhite, 1.0F, kMarkerGlow});
    return m;
}

std::shared_ptr<Mesh> obstacle(const items::Item& item) {
    auto m = std::make_shared<Mesh>();
    const Shape& s = item.shape;
    const std::string kind = propertyString(item.properties, "shape", "box");
    if (s.kind == Shape::Kind::Cylinder) {
        const auto r = static_cast<float>(s.radiusM);
        const auto top = static_cast<float>(s.topM);
        const auto bottom = static_cast<float>(s.bottomM);
        if (kind == "tree") {
            graphics::appendCylinder(*m, glm::vec3(0.0F, bottom, 0.0F),
                                     glm::vec3(0.0F, 0.45F * top, 0.0F), 0.12F * r, 10, {kBark});
            graphics::appendSphere(*m, glm::vec3(0.0F, 0.62F * top, 0.0F), r, 12, {kLeaves});
            graphics::appendSphere(*m, glm::vec3(0.0F, 0.86F * top, 0.0F), 0.6F * r, 10, {kLeaves});
        } else if (kind == "windTurbine") {
            graphics::appendCylinder(*m, glm::vec3(0.0F, bottom, 0.0F),
                                     glm::vec3(0.0F, 0.75F * top, 0.0F), 0.04F * top, 16, {kWhite});
            const glm::vec3 hub(0.0F, 0.75F * top, -0.05F * top);
            graphics::appendBox(*m, glm::vec3(0.0F, 0.75F * top, 0.02F * top),
                                glm::vec3(0.035F * top, 0.035F * top, 0.08F * top), {kWhite});
            for (int i = 0; i < 3; ++i) {
                const float a = kTwoPi * static_cast<float>(i) / 3.0F;
                const glm::quat q = glm::angleAxis(a, glm::vec3(0.0F, 0.0F, 1.0F));
                graphics::appendBox(*m, hub + q * glm::vec3(0.0F, 0.12F * top, 0.0F),
                                    glm::vec3(0.012F * top, 0.12F * top, 0.004F * top), {kWhite},
                                    q);
            }
        } else {
            graphics::appendCylinder(*m, glm::vec3(0.0F, bottom, 0.0F), glm::vec3(0.0F, top, 0.0F),
                                     r, 20, {kConcrete});
        }
        return m;
    }
    const glm::vec3 half(s.halfExtentsM);
    const glm::vec3 centre(s.offsetM);
    if (kind == "building") {
        graphics::appendBox(*m, centre, half, {kWall});
        graphics::appendBox(*m, centre + glm::vec3(0.0F, half.y + 0.1F, 0.0F),
                            glm::vec3(half.x + 0.1F, 0.1F, half.z + 0.1F), {kRoof});
    } else if (kind == "powerLine") {
        // Poles at both ends along x, wires at the top.
        const float top = centre.y + half.y;
        const float bottom = centre.y - half.y;
        for (const float x : {-half.x, half.x}) {
            graphics::appendCylinder(*m, glm::vec3(x, bottom, centre.z),
                                     glm::vec3(x, top, centre.z), 0.15F, 8, {kBark});
            graphics::appendBox(*m, glm::vec3(x, top - 0.3F, centre.z),
                                glm::vec3(0.1F, 0.08F, std::max(half.z, 1.0F)), {kBark});
        }
        for (const float z : {-std::max(half.z, 1.0F) * 0.8F, std::max(half.z, 1.0F) * 0.8F}) {
            graphics::appendCylinder(*m, glm::vec3(-half.x, top - 0.25F, centre.z + z),
                                     glm::vec3(half.x, top - 0.25F, centre.z + z), 0.02F, 6,
                                     {kCable});
        }
    } else {
        graphics::appendBox(*m, centre, half, {kConcrete});
    }
    return m;
}

std::shared_ptr<Mesh> translucentVolume(const Shape& s, const glm::vec3& color, float opacity,
                                        std::optional<std::pair<float, float>> range = {}) {
    auto m = std::make_shared<Mesh>();
    m->transparent = true;
    const auto [bottom, top] = range.value_or(
        s.kind == Shape::Kind::Box
            ? std::pair<float, float>{static_cast<float>(s.offsetM.y - s.halfExtentsM.y),
                                      static_cast<float>(s.offsetM.y + s.halfExtentsM.y)}
            : drawnRange(s));
    const auto outline = outlineOf(s);
    appendWalls(*m, outline, bottom, top, {color, opacity, kMarkerGlow});
    // Solid outline on the ground, easier to see from above.
    appendOutline(*m, outline, 0.25F, std::max(bottom, 0.0F) + 0.05F, {color, 0.8F, kMarkerGlow});
    return m;
}

std::shared_ptr<Mesh> parkingZone(const Shape& s) {
    auto m = std::make_shared<Mesh>();
    appendOutline(*m, outlineOf(s), 0.12F, 0.03F, {kWhite, 1.0F, kMarkerGlow});
    // Heading mark at the front edge.
    graphics::appendBox(*m, glm::vec3(0.0F, 0.03F, static_cast<float>(-0.7 * s.halfExtentsM.z)),
                        glm::vec3(0.06F, 0.01F, 0.25F), {kYellow, 1.0F, kMarkerGlow});
    return m;
}

std::shared_ptr<Mesh> waypoint(const Shape& s) {
    auto m = std::make_shared<Mesh>();
    m->transparent = true;
    graphics::appendSphere(*m, glm::vec3(s.offsetM), static_cast<float>(s.radiusM), 24,
                           {kWaypointColor, 0.28F, kMarkerGlow});
    return m;
}

std::shared_ptr<Mesh> inspectionTarget(const items::Item& item) {
    auto m = std::make_shared<Mesh>();
    const auto r = static_cast<float>(item.shape.radiusM);
    graphics::appendSphere(*m, glm::vec3(item.shape.offsetM), r, 16, {kTargetColor});
    // Mast to the ground.
    if (item.altitudeRef == items::AltitudeRef::Agl && item.altitudeM > static_cast<double>(r)) {
        graphics::appendCylinder(*m, glm::vec3(0.0F, -static_cast<float>(item.altitudeM), 0.0F),
                                 glm::vec3(0.0F, -r, 0.0F), std::max(0.05F, 0.1F * r), 8,
                                 {kConcrete});
    }
    // White band, so its orientation reads in the camera.
    graphics::appendCylinder(*m, glm::vec3(0.0F, -0.15F * r, 0.0F),
                             glm::vec3(0.0F, 0.15F * r, 0.0F), 1.02F * r, 16, {kWhite});
    return m;
}

std::shared_ptr<Mesh> payload(const Shape& s) {
    auto m = std::make_shared<Mesh>();
    const glm::vec3 half(s.halfExtentsM);
    graphics::appendBox(*m, glm::vec3(s.offsetM) + glm::vec3(0.0F, half.y, 0.0F), half,
                        {kCardboard});
    graphics::appendBox(*m, glm::vec3(s.offsetM) + glm::vec3(0.0F, half.y, 0.0F),
                        glm::vec3(1.01F * half.x, 1.01F * half.y, 0.15F * half.z), {kYellow});
    return m;
}

std::shared_ptr<Mesh> pilot() {
    auto m = std::make_shared<Mesh>();
    graphics::appendCylinder(*m, glm::vec3(0.0F), glm::vec3(0.0F, 1.45F, 0.0F), 0.2F, 12,
                             {kPilotColor});
    graphics::appendSphere(*m, glm::vec3(0.0F, 1.6F, 0.0F), 0.13F, 10, {kSkin});
    return m;
}

}  // namespace

std::shared_ptr<const Mesh> ItemVisuals::meshFor(const items::Item& item, bool showMarkers) {
    if (!item.visible) {
        return nullptr;
    }
    switch (item.type) {
        case ItemType::Drone:
        case ItemType::Spawn: return nullptr;
        case ItemType::LandingPad: return landingPad(item.shape);
        case ItemType::Gate: return gate(item);
        case ItemType::Obstacle:
        case ItemType::Ramp:
        case ItemType::Bridge: return obstacle(item);
        case ItemType::Payload: return payload(item.shape);
        case ItemType::InspectionTarget: return inspectionTarget(item);
        case ItemType::PilotPosition: return pilot();
        case ItemType::NoFlyZone: return translucentVolume(item.shape, kNoFlyColor, 0.2F);
        case ItemType::Geofence: return translucentVolume(item.shape, kGeofenceColor, 0.1F);
        case ItemType::Waypoint: return showMarkers ? waypoint(item.shape) : nullptr;
        case ItemType::PickupZone:
            return showMarkers ? translucentVolume(item.shape, kPickupColor, 0.22F,
                                                   std::pair<float, float>{-0.5F, 3.0F})
                               : nullptr;
        case ItemType::DropZone:
            return showMarkers ? translucentVolume(item.shape, kDropColor, 0.22F,
                                                   std::pair<float, float>{-0.5F, 3.0F})
                               : nullptr;
        case ItemType::SurveyArea:
            return showMarkers ? translucentVolume(item.shape, kSurveyColor, 0.25F,
                                                   std::pair<float, float>{-1.0F, 2.0F})
                               : nullptr;
        case ItemType::ParkingZone: return showMarkers ? parkingZone(item.shape) : nullptr;
    }
    return nullptr;
}

ItemVisuals::ItemVisuals(const items::ItemRegistry& registry, bool showMarkers) {
    for (const items::Item* item : registry.items()) {
        if (auto mesh = meshFor(*item, showMarkers); mesh && !mesh->indices.empty()) {
            parts_.push_back({.key = item->key,
                              .mesh = std::move(mesh),
                              .position = item->transform.position,
                              .orientation = item->transform.rotation,
                              .scale = glm::vec3(item->transform.scale)});
        }
    }
}

void ItemVisuals::appendDrawItems(double timeS, std::span<const std::string> highlighted,
                                  std::vector<graphics::DrawItem>& out) const {
    const auto pulse =
        static_cast<float>(1.5 + 1.5 * std::sin(2.0 * std::numbers::pi * 1.2 * timeS));
    for (const Part& p : parts_) {
        const bool lit =
            !p.key.empty() && std::ranges::find(highlighted, p.key) != highlighted.end();
        out.push_back({.mesh = p.mesh,
                       .position = p.position,
                       .orientation = p.orientation,
                       .scale = p.scale,
                       .tint = lit ? glm::vec4(1.25F, 1.25F, 1.25F, 1.0F) : glm::vec4(1.0F),
                       .emissiveScale = lit ? pulse : 0.25F,
                       .castsShadow = !p.mesh->transparent});
    }
}

}  // namespace ds::sim
