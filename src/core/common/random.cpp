// SPDX-FileCopyrightText: 2026 Juan Maria Gomez Lopez <juan-maria.gomez-lopez@tutamail.com>
// SPDX-License-Identifier: MIT

#include "ds/common/random.hpp"

#include <chrono>
#include <random>

namespace ds::common {

std::uint64_t randomSeed() {
    std::random_device device;
    const auto high = static_cast<std::uint64_t>(device()) << 32U;
    const auto low = static_cast<std::uint64_t>(device());
    const auto time =
        static_cast<std::uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count());
    return mix64(high ^ low ^ time);
}

}  // namespace ds::common
