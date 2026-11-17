// SPDX-FileCopyrightText: 2026 Juan Maria Gomez Lopez <juan-maria.gomez-lopez@tutamail.com>
// SPDX-License-Identifier: MIT

#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <variant>

#include "ds/common/error.hpp"
#include "ds/common/json.hpp"
#include "ds/physics/aerial_profile.hpp"
#include "ds/physics/ground_profile.hpp"
#include "ds/physics/vehicle.hpp"

/// @file
/// @brief A profile of either vehicle type, and the vehicle built from it.
/// @ingroup physics

namespace ds::physics {

/// @brief Aerial or ground profile.
using VehicleProfile = std::variant<AerialProfile, GroundProfile>;

/// @brief Type of the vehicle @p profile describes.
[[nodiscard]] VehicleType profileType(const VehicleProfile& profile) noexcept;
/// @brief Key of @p profile.
[[nodiscard]] const std::string& profileKey(const VehicleProfile& profile) noexcept;
/// @brief Display name of @p profile.
[[nodiscard]] const std::string& profileName(const VehicleProfile& profile) noexcept;
/// @brief Onboard camera of @p profile.
[[nodiscard]] const CameraParams& profileCamera(const VehicleProfile& profile) noexcept;

/// @brief The vehicle for @p profile; randomness from @p seed.
[[nodiscard]] std::unique_ptr<Vehicle> makeVehicle(const VehicleProfile& profile,
                                                   std::uint64_t seed);

/// @brief Parses a profile of either type (by its `vehicleType`).
[[nodiscard]] common::Result<VehicleProfile> vehicleProfileFromJson(const common::Json& json);
/// @brief Reads a profile file of either type.
[[nodiscard]] common::Result<VehicleProfile> loadVehicleProfile(const std::filesystem::path& file);
/// @brief Serialises a profile of either type.
[[nodiscard]] common::Json vehicleProfileToJson(const VehicleProfile& profile);

}  // namespace ds::physics
