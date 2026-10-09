// SPDX-License-Identifier: MIT

// nw::snd pitch ratios (1/256-semitone units) and pitch LFO.

#pragma once

#include <cstdint>

namespace citrusf2
{

// Convert `pitch` from 1/256 semitones to a frequency ratio, following Util::CalcPitchRatio (code.bin 0x17f77c). Apply
// whole octaves, then single-precision 2^(i/12) and 2^(i/3072) tables for the remainder.
float PitchRatio(int pitch);

// nw::snd channel LFO: a delayed sine from a quarter-wave table (code.bin 0x1811c8 Update, 0x181434 GetValue).
class Lfo
{
public:
    // Set MML depth, speed and range, with delay in milliseconds.
    void Set(uint8_t depth, uint8_t speed, uint8_t range, uint32_t delay_ms);

    // Advance by `msec` envelope milliseconds. Use up the delay first.
    void Update(int msec);

    // Return depth / 128 * range * sine, in semitones for a pitch LFO.
    float Value() const;

private:
    float depth_ = 0.0f;         // MML depth / 128
    float speed_ = 6.25f;        // Hz: MML speed * 100 / 256
    uint32_t delay_ = 0;         // milliseconds
    uint8_t range_ = 1;          // semitones, for a pitch LFO
    uint32_t delay_counter_ = 0; // elapsed delay in milliseconds
    float phase_ = 0.0f;         // 0-1
};

} // namespace citrusf2
