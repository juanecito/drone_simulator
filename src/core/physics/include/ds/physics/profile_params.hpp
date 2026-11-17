// SPDX-FileCopyrightText: 2026 Juan Maria Gomez Lopez <juan-maria.gomez-lopez@tutamail.com>
// SPDX-License-Identifier: MIT

#pragma once

#include <functional>
#include <string_view>

/// @file
/// @brief Editable numeric parameters of a vehicle profile (Vehicle tab and JSON reader).
/// @ingroup physics

namespace ds::physics {

/// @brief Kind of an editable parameter.
enum class ParamKind { Number, Integer };

/// @brief An editable numeric parameter of a profile of type @p Profile.
template <typename Profile>
struct BasicParamDescriptor {
    std::string_view pointer;                   ///< JSON pointer, e.g. "/motor/kv".
    std::string_view group;                     ///< Group shown in the Vehicle tab.
    std::string_view label;                     ///< Label.
    std::string_view unit;                      ///< Unit symbol ("" if none).
    double min;                                 ///< Smallest valid value.
    double max;                                 ///< Largest valid value.
    int decimals;                               ///< Decimals shown.
    ParamKind kind;                             ///< Number or integer.
    std::function<double(const Profile&)> get;  ///< Reads the value.
    std::function<void(Profile&, double)> set;  ///< Writes the value.
};

}  // namespace ds::physics
