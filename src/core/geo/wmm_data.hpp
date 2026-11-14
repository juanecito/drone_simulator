// SPDX-FileCopyrightText: 2026 Juan Maria Gomez Lopez <juan-maria.gomez-lopez@tutamail.com>
// SPDX-License-Identifier: MIT

#pragma once

#include <array>

/// @file
/// @brief Internal: Gauss coefficients of the World Magnetic Model (data generated into
/// wmm_data.cpp by `scripts/data/gen_wmm.py`).
/// @ingroup geo

namespace ds::geo::detail {

/// @brief One Schmidt semi-normalised Gauss coefficient pair and its secular variation.
struct WmmTerm {
    int n;        ///< Degree, 1…12.
    int m;        ///< Order, 0…n.
    double g;     ///< g(n, m) at the epoch, nT.
    double h;     ///< h(n, m) at the epoch, nT.
    double gDot;  ///< dg/dt, nT/year.
    double hDot;  ///< dh/dt, nT/year.
};

/// @brief Maximum degree of the model.
inline constexpr int kWmmDegree = 12;

/// @brief Model name, e.g. "WMM-2025".
extern const char* const kWmmName;
/// @brief Epoch of the coefficients (decimal year).
extern const double kWmmEpoch;
/// @brief Terms in order of degree, then order.
extern const std::array<WmmTerm, kWmmDegree*(kWmmDegree + 3) / 2> kWmmTerms;

}  // namespace ds::geo::detail
