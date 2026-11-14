// SPDX-FileCopyrightText: 2026 Juan Maria Gomez Lopez <juan-maria.gomez-lopez@tutamail.com>
// SPDX-License-Identifier: MIT

#include "ds/terrain/height_source.hpp"

#include <cstddef>

namespace ds::terrain {

void HeightSource::sampleGrid(double x0, double z0, double step, int cols, int rows,
                              std::span<double> out) const {
    for (int row = 0; row < rows; ++row) {
        const double z = z0 + row * step;
        for (int col = 0; col < cols; ++col) {
            out[static_cast<std::size_t>(row) * static_cast<std::size_t>(cols) +
                static_cast<std::size_t>(col)] = height(x0 + col * step, z);
        }
    }
}

}  // namespace ds::terrain
