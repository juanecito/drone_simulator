// SPDX-FileCopyrightText: 2026 Juan Maria Gomez Lopez <juan-maria.gomez-lopez@tutamail.com>
// SPDX-License-Identifier: MIT

#include "ds/physics/vehicle_factory.hpp"

#include <format>

#include "ds/physics/aerial_vehicle.hpp"
#include "ds/physics/ground_vehicle.hpp"

namespace ds::physics {

VehicleType profileType(const VehicleProfile& profile) noexcept {
    return std::holds_alternative<GroundProfile>(profile) ? VehicleType::Ground
                                                          : VehicleType::Aerial;
}

const std::string& profileKey(const VehicleProfile& profile) noexcept {
    return std::visit([](const auto& p) -> const std::string& { return p.key; }, profile);
}

const std::string& profileName(const VehicleProfile& profile) noexcept {
    return std::visit([](const auto& p) -> const std::string& { return p.name; }, profile);
}

const CameraParams& profileCamera(const VehicleProfile& profile) noexcept {
    return std::visit([](const auto& p) -> const CameraParams& { return p.camera; }, profile);
}

std::unique_ptr<Vehicle> makeVehicle(const VehicleProfile& profile, std::uint64_t seed) {
    if (const auto* ground = std::get_if<GroundProfile>(&profile)) {
        return std::make_unique<GroundVehicle>(*ground, seed);
    }
    return std::make_unique<AerialVehicle>(std::get<AerialProfile>(profile), seed);
}

common::Result<VehicleProfile> vehicleProfileFromJson(const common::Json& json) {
    if (json.is_object() && json.contains("vehicleType") && json["vehicleType"] == "ground") {
        auto p = groundProfileFromJson(json);
        if (!p) {
            return std::unexpected(p.error());
        }
        return VehicleProfile{std::move(*p)};
    }
    auto p = aerialProfileFromJson(json);
    if (!p) {
        return std::unexpected(p.error());
    }
    return VehicleProfile{std::move(*p)};
}

common::Result<VehicleProfile> loadVehicleProfile(const std::filesystem::path& file) {
    auto json = common::readJsonFile(file, std::size_t{1} << 20U);
    if (!json) {
        return std::unexpected(json.error());
    }
    auto profile = vehicleProfileFromJson(*json);
    if (!profile) {
        return common::makeError(std::format("{}: {}", file.string(), profile.error().message));
    }
    return profile;
}

common::Json vehicleProfileToJson(const VehicleProfile& profile) {
    if (const auto* ground = std::get_if<GroundProfile>(&profile)) {
        return groundProfileToJson(*ground);
    }
    return aerialProfileToJson(std::get<AerialProfile>(profile));
}

}  // namespace ds::physics
