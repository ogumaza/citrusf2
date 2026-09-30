// SPDX-License-Identifier: MIT

// Convert nw::snd pan to the SoundFont pan law.

#pragma once

#include <algorithm>
#include <cmath>
#include <numbers>

namespace citrusf2
{

// nw::snd's default pan law is left = sqrt((1 - p) / 2), right = sqrt((1 + p) / 2), for p in -1..1. FluidSynth and the
// SoundFont 2 reference implementation use a sin/cos law: right = sin((p' + 1) * pi / 4). Both preserve power, so
// matching their left/right ratios also matches their gains:
//
// p' = 4 / pi * atan(sqrt((1 + p) / (1 - p))) - 1
inline double SoundFontPan(double p)
{
    p = std::clamp(p, -1.0, 1.0);
    if (p >= 1.0)
    {
        return 1.0;
    }

    return 4.0 / std::numbers::pi * std::atan(std::sqrt((1.0 + p) / (1.0 - p))) - 1.0;
}

} // namespace citrusf2
