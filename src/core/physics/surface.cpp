// SPDX-FileCopyrightText: 2026 Juan Maria Gomez Lopez <juan-maria.gomez-lopez@tutamail.com>
// SPDX-License-Identifier: MIT

#include "ds/physics/surface.hpp"

#include <algorithm>
#include <array>

namespace ds::physics {
namespace {

constexpr std::array kMaterials{SurfaceMaterial::Paved, SurfaceMaterial::Grass,
                                SurfaceMaterial::Dirt,  SurfaceMaterial::Sand,
                                SurfaceMaterial::Snow,  SurfaceMaterial::Mud};

}  // namespace

SurfaceProperties surfaceProperties(SurfaceMaterial material) noexcept {
    switch (material) {
        case SurfaceMaterial::Paved: return {.peakFriction = 0.9, .rollingResistance = 0.015};
        case SurfaceMaterial::Grass: return {.peakFriction = 0.55, .rollingResistance = 0.07};
        case SurfaceMaterial::Dirt: return {.peakFriction = 0.65, .rollingResistance = 0.04};
        case SurfaceMaterial::Sand: return {.peakFriction = 0.5, .rollingResistance = 0.16};
        case SurfaceMaterial::Snow: return {.peakFriction = 0.3, .rollingResistance = 0.08};
        case SurfaceMaterial::Mud: return {.peakFriction = 0.35, .rollingResistance = 0.14};
    }
    return {};
}

namespace {

constexpr double kFullSnowCoverM = 0.02;
constexpr double kWetDescriptionThreshold = 0.3;

SurfaceProperties wetProperties(SurfaceMaterial material) noexcept {
    switch (material) {
        case SurfaceMaterial::Paved: return {.peakFriction = 0.6, .rollingResistance = 0.018};
        case SurfaceMaterial::Grass: return {.peakFriction = 0.35, .rollingResistance = 0.08};
        case SurfaceMaterial::Dirt: return {.peakFriction = 0.4, .rollingResistance = 0.10};
        case SurfaceMaterial::Sand: return {.peakFriction = 0.55, .rollingResistance = 0.10};
        case SurfaceMaterial::Snow: return {.peakFriction = 0.2, .rollingResistance = 0.10};
        case SurfaceMaterial::Mud: return {.peakFriction = 0.25, .rollingResistance = 0.18};
    }
    return {};
}

SurfaceProperties mix(const SurfaceProperties& a, const SurfaceProperties& b, double s) noexcept {
    return {
        .peakFriction = a.peakFriction + (b.peakFriction - a.peakFriction) * s,
        .rollingResistance = a.rollingResistance + (b.rollingResistance - a.rollingResistance) * s};
}

}  // namespace

SurfaceProperties surfaceProperties(SurfaceMaterial material,
                                    const weather::GroundCover& cover) noexcept {
    const double wet = std::clamp(cover.wetness, 0.0, 1.0);
    SurfaceProperties p = mix(surfaceProperties(material), wetProperties(material), wet);
    if (cover.snowDepthM > 0.0) {
        const SurfaceProperties snow = mix(surfaceProperties(SurfaceMaterial::Snow),
                                           wetProperties(SurfaceMaterial::Snow), wet);
        p = mix(p, snow, std::clamp(cover.snowDepthM / kFullSnowCoverM, 0.0, 1.0));
        p.rollingResistance += std::min(0.25, 2.0 * cover.snowDepthM);
    }
    return p;
}

std::string surfaceDescription(SurfaceMaterial material, const weather::GroundCover& cover) {
    if (cover.snowDepthM >= kFullSnowCoverM) {
        return "snow";
    }
    const std::string name(surfaceMaterialName(material));
    return cover.wetness >= kWetDescriptionThreshold ? "wet " + name : name;
}

std::string_view surfaceMaterialName(SurfaceMaterial material) noexcept {
    switch (material) {
        case SurfaceMaterial::Paved: return "paved";
        case SurfaceMaterial::Grass: return "grass";
        case SurfaceMaterial::Dirt: return "dirt";
        case SurfaceMaterial::Sand: return "sand";
        case SurfaceMaterial::Snow: return "snow";
        case SurfaceMaterial::Mud: return "mud";
    }
    return "?";
}

std::optional<SurfaceMaterial> parseSurfaceMaterial(std::string_view name) noexcept {
    for (const SurfaceMaterial m : kMaterials) {
        if (surfaceMaterialName(m) == name) {
            return m;
        }
    }
    return std::nullopt;
}

SurfaceMaterial surfaceFromTerrain(terrain::Material material) noexcept {
    switch (material) {
        case terrain::Material::Grass: return SurfaceMaterial::Grass;
        case terrain::Material::Rock: return SurfaceMaterial::Paved;
        case terrain::Material::Sand: return SurfaceMaterial::Sand;
        case terrain::Material::Snow: return SurfaceMaterial::Snow;
    }
    return SurfaceMaterial::Grass;
}

std::span<const SurfaceMaterial> allSurfaceMaterials() noexcept { return kMaterials; }

}  // namespace ds::physics
