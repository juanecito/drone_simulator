// SPDX-FileCopyrightText: 2026 Juan Maria Gomez Lopez <juan-maria.gomez-lopez@tutamail.com>
// SPDX-License-Identifier: MIT

#pragma once

#include <string>
#include <vector>

/// @file
/// @brief Brushless motor, propeller and battery models (FR-PHY-02, FR-PHY-03, FR-PHY-07).
/// @ingroup physics

namespace ds::physics {

/// @brief Brushless DC motor (FR-PHY-02).
struct MotorParams {
    double kv = 1900.0;              ///< Speed constant, rpm/V.
    double resistanceOhm = 0.065;    ///< Phase-to-phase winding resistance.
    double noLoadCurrentA = 1.2;     ///< No-load current I0.
    double timeConstantS = 0.03;     ///< First-order spin-up/down time constant.
    double rotorInertiaKgM2 = 1e-5;  ///< Bell + propeller inertia about the shaft.
    double maxCurrentA = 60.0;       ///< Rated peak current (informative, for warnings).
    double massKg = 0.032;           ///< Mass of one motor (informative).
};

/// @brief Fixed-pitch propeller (FR-PHY-03).
struct PropellerParams {
    double diameterM = 0.13;           ///< Diameter D.
    double pitchM = 0.117;             ///< Geometric pitch.
    int blades = 3;                    ///< Number of blades (informative).
    double thrustCoefficient = 0.18;   ///< Static C_T (thrust T = C_T·ρ·n²·D⁴, n in rev/s).
    double torqueCoefficient = 0.012;  ///< Static C_Q (torque Q = C_Q·ρ·n²·D⁵).
    /// Advance ratio J = V/(n·D) at which the thrust reaches zero (forward-flight loss).
    double zeroThrustAdvanceRatio = 0.9;
    /// Rotor drag ("H-force") coefficient: H = k·T·μ, μ = V⊥/(ω·R).
    double rotorDragCoefficient = 0.5;
};

/// @brief Battery chemistry (selects the open-circuit voltage curve and limits).
enum class Chemistry { LiPo, LiIon };

/// @brief Battery pack (FR-PHY-07).
struct BatteryParams {
    Chemistry chemistry = Chemistry::LiPo;  ///< Cell type.
    int cellsInSeries = 6;                  ///< S count.
    double capacityAh = 1.1;                ///< Rated capacity at 25 °C.
    double cellResistanceOhm = 0.006;       ///< DC internal resistance per cell at 25 °C.
    double maxDischargeC = 100.0;           ///< Rated continuous C (informative).
    double massKg = 0.18;                   ///< Pack mass (informative, part of the AUW).
    double heatCapacityJPerK = 180.0;       ///< Thermal mass of the pack.
    double coolingWPerK = 0.6;              ///< Heat transfer to the air.
    double lowCellVoltage = 3.5;            ///< Loaded cell voltage of the low warning.
    double criticalCellVoltage = 3.3;       ///< Loaded cell voltage of the critical warning.
    double cutoffCellVoltage = 3.0;  ///< Loaded cell voltage of the ESC cut-off (held 0.5 s).
};

/// @brief Open-circuit cell voltage at state of charge @p soc (0–1) for @p chemistry.
[[nodiscard]] double openCircuitCellVoltage(Chemistry chemistry, double soc) noexcept;
/// @brief Fraction of the rated capacity usable at @p temperatureC.
[[nodiscard]] double capacityFactor(Chemistry chemistry, double temperatureC) noexcept;
/// @brief Internal-resistance multiplier at @p temperatureC (1 at 25 °C, Arrhenius law).
[[nodiscard]] double resistanceFactor(double temperatureC) noexcept;
/// @brief Multiplier of the polarisation (RC-branch) resistance at @p temperatureC (1 at
/// 25 °C): charge transfer and diffusion, Arrhenius law with Ea/R = 6000 K (LiPo) or 5000 K
/// (Li-ion).
[[nodiscard]] double polarisationFactor(Chemistry chemistry, double temperatureC) noexcept;
/// @brief Name used in profile files ("lipo", "liion").
[[nodiscard]] const char* chemistryName(Chemistry chemistry) noexcept;

/// @brief Battery warning state.
enum class BatteryWarning { None, Low, Critical, Depleted };

/// @brief Equivalent-circuit battery: OCV(SoC, T) − I·R0(T) − V_RC, with one RC branch for the
/// slow polarisation, coulomb counting and a lumped thermal model.
class Battery {
public:
    /// @brief Full pack at @p temperatureC.
    explicit Battery(const BatteryParams& params = {}, double temperatureC = 25.0);

    /// @brief Refills the pack (state of charge @p soc) at @p temperatureC.
    void reset(double temperatureC, double soc = 1.0);
    /// @brief Draws @p currentA for @p dt seconds with the air at @p ambientC.
    void update(double currentA, double ambientC, double dt);

    /// @brief Terminal voltage at the current load.
    [[nodiscard]] double voltage() const noexcept { return voltage_; }
    /// @brief Terminal voltage if @p currentA were drawn now.
    [[nodiscard]] double voltageAt(double currentA) const noexcept;
    /// @brief Current of the last update(), A.
    [[nodiscard]] double current() const noexcept { return current_; }
    /// @brief State of charge 0–1 (of the capacity usable at the pack temperature).
    [[nodiscard]] double stateOfCharge() const noexcept;
    /// @brief Charge drawn so far, mAh.
    [[nodiscard]] double consumedMah() const noexcept { return consumedAh_ * 1000.0; }
    /// @brief Energy drawn so far, Wh.
    [[nodiscard]] double consumedWh() const noexcept { return consumedWh_; }
    /// @brief Pack temperature, °C.
    [[nodiscard]] double temperatureC() const noexcept { return temperatureC_; }
    /// @brief Pack internal resistance at the current temperature, Ω.
    [[nodiscard]] double packResistance() const noexcept;
    /// @brief Open-circuit pack voltage.
    [[nodiscard]] double openCircuitVoltage() const noexcept;
    /// @brief Warning level (from the loaded cell voltage, held for 1 s to ignore spikes).
    [[nodiscard]] BatteryWarning warning() const noexcept { return warning_; }
    /// @brief True once the pack is empty or the voltage fell to the cut-off.
    [[nodiscard]] bool depleted() const noexcept { return warning_ == BatteryWarning::Depleted; }
    /// @brief Parameters.
    [[nodiscard]] const BatteryParams& params() const noexcept { return params_; }

private:
    BatteryParams params_;
    double consumedAh_ = 0.0;
    double consumedWh_ = 0.0;
    double temperatureC_ = 25.0;
    double polarisationV_ = 0.0;
    double current_ = 0.0;
    double voltage_ = 0.0;
    double lowTimer_ = 0.0;
    double criticalTimer_ = 0.0;
    double cutoffTimer_ = 0.0;
    BatteryWarning warning_ = BatteryWarning::None;
};

/// @brief Aerodynamic and electrical state of one motor + propeller.
struct RotorOutput {
    double thrustN = 0.0;   ///< Thrust along the motor axis.
    double torqueNm = 0.0;  ///< Aerodynamic drag torque on the propeller.
    double currentA = 0.0;  ///< Motor phase current.
};

/// @brief Steady-state propeller coefficients at advance ratio @p j (≥ 0 = climbing).
struct PropCoefficients {
    double ct = 0.0;  ///< Thrust coefficient.
    double cq = 0.0;  ///< Torque coefficient.
};

/// @brief C_T and C_Q at advance ratio @p j (linear thrust loss to zero at J₀, torque falls by
/// at most 30 %; descending flow (J < 0) keeps the static values).
[[nodiscard]] PropCoefficients propCoefficients(const PropellerParams& prop, double j) noexcept;

/// @brief Thrust and torque of a propeller turning at @p omegaRad (rad/s) in air of density
/// @p rho with axial inflow @p axialSpeed (m/s, positive when the air enters from the front of
/// the disc, i.e. climbing).
[[nodiscard]] RotorOutput propellerLoads(const PropellerParams& prop, double omegaRad, double rho,
                                         double axialSpeed) noexcept;

/// @brief Steady-state speed (rad/s) of the motor driving the propeller at voltage
/// @p voltage (the motor equation `V = ω/Kv + R·(I0 + Q(ω)/Kt)` solved for ω).
[[nodiscard]] double steadyStateOmega(const MotorParams& motor, const PropellerParams& prop,
                                      double voltage, double rho, double axialSpeed) noexcept;

/// @brief Motor current (A) at speed @p omegaRad delivering torque @p torqueNm plus the rotor
/// acceleration @p omegaDot.
[[nodiscard]] double motorCurrent(const MotorParams& motor, double omegaRad, double torqueNm,
                                  double omegaDot) noexcept;

/// @brief Kv in rad/s per volt.
[[nodiscard]] double kvRadPerVolt(const MotorParams& motor) noexcept;

}  // namespace ds::physics
