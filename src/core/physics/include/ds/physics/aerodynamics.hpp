// SPDX-FileCopyrightText: 2026 Juan Maria Gomez Lopez <juan-maria.gomez-lopez@tutamail.com>
// SPDX-License-Identifier: MIT

#pragma once

#include "ds/physics/aerial_profile.hpp"
#include "ds/physics/frames.hpp"

/// @file
/// @brief Aerodynamic effects of a multirotor (FR-PHY-04): body drag, rotor drag (H-force),
/// ground effect and vortex-ring state.
/// @ingroup physics

namespace ds::physics {

/// @brief Body drag (body frame, N) for the air velocity relative to the vehicle
/// @p airVelocityBody (vehicle velocity minus wind, body frame): `−½·ρ·C_dA_i·|v_i|·v_i` per axis.
[[nodiscard]] Vec3 bodyDrag(const FrameParams& frame, const Vec3& airVelocityBody,
                            double rho) noexcept;

/// @brief Rotor drag (body frame, N) of one rotor producing @p thrustN at @p omegaRad:
/// `H = −k·T·μ` in the rotor plane, with the advance ratio `μ = V∥ / (ω·R)`.
[[nodiscard]] Vec3 rotorDrag(const PropellerParams& prop, double thrustN, double omegaRad,
                             const Vec3& airVelocityBody) noexcept;

/// @brief Thrust gain in ground effect (Cheeseman–Bennett `1 / (1 − (R/4z)²)`), for the rotor
/// plane at @p heightM above the ground; 1 out of ground effect, at most 1.4.
[[nodiscard]] double groundEffectGain(double heightM, double rotorRadiusM) noexcept;

/// @brief Induced velocity of a rotor in hover, `√(T / (2·ρ·A))`.
[[nodiscard]] double hoverInducedVelocity(double thrustN, double rho, double diskAreaM2) noexcept;

/// @brief Mean thrust factor in the vortex-ring state for a descent speed @p descentMs along
/// the rotor axis (positive = descending into the own wake): up to 25 % loss around
/// descent ≈ v_h (@p inducedMs), none below 0.3·v_h or above 2·v_h.
[[nodiscard]] double vortexRingFactor(double descentMs, double inducedMs) noexcept;

}  // namespace ds::physics
