/**
 * @file        input/stick_deadzone.h
 * @brief       Radial dead zone for physical controller right sticks.
 *
 * @copyright   Copyright (c) 2026 The gow2-recomp authors
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <utility>

#include <rex/cvar.h>

REXCVAR_DECLARE(double, pad_right_stick_deadzone);

namespace rex::input {

// Applies a radial dead zone to a right thumbstick sample, rescaling the
// surviving deflection back to full range. Only physical pads (SDL, XInput)
// call this, on the state they return before states are merged. The
// keyboard/mouse driver never calls it.
inline std::pair<int16_t, int16_t> ApplyRightStickDeadzone(int16_t rx, int16_t ry,
                                                           double deadzone) {
  double dz = std::clamp(deadzone, 0.0, 0.9);
  if (dz == 0.0) {
    return {rx, ry};
  }
  double x = static_cast<double>(rx);
  double y = static_cast<double>(ry);
  double m = std::sqrt(x * x + y * y) / 32767.0;
  if (m < dz) {
    return {0, 0};
  }
  double scale = ((m - dz) / (1.0 - dz)) / m;
  auto clamp16 = [](double v) {
    return static_cast<int16_t>(std::clamp(std::lround(v), -32768L, 32767L));
  };
  return {clamp16(x * scale), clamp16(y * scale)};
}

}  // namespace rex::input
