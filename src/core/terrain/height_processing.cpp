// SPDX-FileCopyrightText: 2026 Juan Maria Gomez Lopez <juan-maria.gomez-lopez@tutamail.com>
// SPDX-License-Identifier: MIT

#include "ds/terrain/height_processing.hpp"

#include <cmath>
#include <format>

namespace ds::terrain {

std::string_view interpolationName(HeightInterpolation interpolation) noexcept {
    switch (interpolation) {
        case HeightInterpolation::Bicubic: return "bicubic";
        case HeightInterpolation::BSpline: return "bspline";
        case HeightInterpolation::Nearest: return "nearest";
        case HeightInterpolation::Bilinear: break;
    }
    return "bilinear";
}

std::optional<HeightInterpolation> interpolationFromName(std::string_view name) noexcept {
    for (const HeightInterpolation i :
         {HeightInterpolation::Bilinear, HeightInterpolation::Bicubic, HeightInterpolation::BSpline,
          HeightInterpolation::Nearest}) {
        if (name == interpolationName(i)) {
            return i;
        }
    }
    return std::nullopt;
}

std::string validateProcessing(const HeightProcessing& p) {
    if (!(p.despikeRadiusM >= 0.0 && p.despikeRadiusM <= kMaxDespikeRadiusM)) {
        return std::format("the spike filter radius must be 0 … {:g} m", kMaxDespikeRadiusM);
    }
    if (!(p.smoothRadiusM >= 0.0 && p.smoothRadiusM <= kMaxSmoothRadiusM)) {
        return std::format("the smoothing radius must be 0 … {:g} m", kMaxSmoothRadiusM);
    }
    return {};
}

std::string describeProcessing(const HeightProcessing& p) {
    std::string text(interpolationName(p.interpolation));
    if (p.despikeRadiusM > 0.0) {
        text += std::format(", spikes removed within {:g} m", p.despikeRadiusM);
    }
    if (p.smoothRadiusM > 0.0) {
        text += std::format(", smoothed over {:g} m", p.smoothRadiusM);
    }
    return text;
}

}  // namespace ds::terrain
