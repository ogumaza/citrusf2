// SPDX-License-Identifier: MIT

// nw::snd pitch ratio and LFO (see pitch.h), from Pokemon X. Keep the game's exact table values and constants to
// reproduce its rounding consistently across platforms.

#include "pitch.h"

namespace citrusf2
{

namespace
{

// 2^(i/12) for the semitones of an octave, code.bin 0x56e388.
constexpr float kSemitoneTable[12] = {
    0x1.0000000000000p+0f, 0x1.0f38fa0000000p+0f, 0x1.1f59ac0000000p+0f, 0x1.306fe00000000p+0f,
    0x1.428a300000000p+0f, 0x1.55b8100000000p+0f, 0x1.6a09e60000000p+0f, 0x1.7f910e0000000p+0f,
    0x1.965fea0000000p+0f, 0x1.ae89fa0000000p+0f, 0x1.c823e00000000p+0f, 0x1.e3437e0000000p+0f,
};

// round(127 sin(i pi / 64)), the LFO's quarter sine, code.bin 0x576a64.
constexpr int8_t kLfoSine[33] = {0,  6,  12,  19,  25,  31,  37,  43,  49,  54,  60,  65,  71,  76,  81,  85, 90,
                                 94, 98, 102, 106, 109, 112, 115, 117, 120, 122, 123, 125, 126, 126, 127, 127};

// Single-precision 1/127 and 0.001 (code.bin 0x1814dc and 0x181224).
constexpr float kOneOver127 = 0x1.020408p-7f;
constexpr float kOneThousandth = 0x1.0624dep-10f;

// 2^(i/3072) for the 1/256 semitones of a semitone, code.bin 0x56e3b8.
constexpr float kFineTable[256] = {
    0x1.0000000000000p+0f, 0x1.000eca0000000p+0f, 0x1.001d940000000p+0f, 0x1.002c600000000p+0f, 0x1.003b2c0000000p+0f,
    0x1.0049fa0000000p+0f, 0x1.0058c80000000p+0f, 0x1.0067980000000p+0f, 0x1.0076680000000p+0f, 0x1.0085380000000p+0f,
    0x1.00940a0000000p+0f, 0x1.00a2dc0000000p+0f, 0x1.00b1b00000000p+0f, 0x1.00c0840000000p+0f, 0x1.00cf580000000p+0f,
    0x1.00de2e0000000p+0f, 0x1.00ed060000000p+0f, 0x1.00fbde0000000p+0f, 0x1.010ab60000000p+0f, 0x1.0119900000000p+0f,
    0x1.01286a0000000p+0f, 0x1.0137440000000p+0f, 0x1.0146200000000p+0f, 0x1.0154fc0000000p+0f, 0x1.0163da0000000p+0f,
    0x1.0172ba0000000p+0f, 0x1.0181980000000p+0f, 0x1.0190780000000p+0f, 0x1.019f5a0000000p+0f, 0x1.01ae3c0000000p+0f,
    0x1.01bd1e0000000p+0f, 0x1.01cc020000000p+0f, 0x1.01dae60000000p+0f, 0x1.01e9cc0000000p+0f, 0x1.01f8b20000000p+0f,
    0x1.02079a0000000p+0f, 0x1.0216820000000p+0f, 0x1.02256a0000000p+0f, 0x1.0234540000000p+0f, 0x1.02433e0000000p+0f,
    0x1.02522a0000000p+0f, 0x1.0261160000000p+0f, 0x1.0270040000000p+0f, 0x1.027ef20000000p+0f, 0x1.028de00000000p+0f,
    0x1.029cd00000000p+0f, 0x1.02abc00000000p+0f, 0x1.02bab20000000p+0f, 0x1.02c9a40000000p+0f, 0x1.02d8980000000p+0f,
    0x1.02e78c0000000p+0f, 0x1.02f6800000000p+0f, 0x1.0305760000000p+0f, 0x1.03146c0000000p+0f, 0x1.0323640000000p+0f,
    0x1.03325c0000000p+0f, 0x1.0341560000000p+0f, 0x1.0350500000000p+0f, 0x1.035f4a0000000p+0f, 0x1.036e460000000p+0f,
    0x1.037d420000000p+0f, 0x1.038c400000000p+0f, 0x1.039b3e0000000p+0f, 0x1.03aa3e0000000p+0f, 0x1.03b93e0000000p+0f,
    0x1.03c8400000000p+0f, 0x1.03d7420000000p+0f, 0x1.03e6440000000p+0f, 0x1.03f5480000000p+0f, 0x1.04044c0000000p+0f,
    0x1.0413520000000p+0f, 0x1.0422580000000p+0f, 0x1.04315e0000000p+0f, 0x1.0440660000000p+0f, 0x1.044f700000000p+0f,
    0x1.045e780000000p+0f, 0x1.046d840000000p+0f, 0x1.047c8e0000000p+0f, 0x1.048b9c0000000p+0f, 0x1.049aa80000000p+0f,
    0x1.04a9b60000000p+0f, 0x1.04b8c60000000p+0f, 0x1.04c7d60000000p+0f, 0x1.04d6e60000000p+0f, 0x1.04e5f80000000p+0f,
    0x1.04f50a0000000p+0f, 0x1.05041c0000000p+0f, 0x1.0513300000000p+0f, 0x1.0522460000000p+0f, 0x1.05315c0000000p+0f,
    0x1.0540720000000p+0f, 0x1.054f8a0000000p+0f, 0x1.055ea20000000p+0f, 0x1.056dbc0000000p+0f, 0x1.057cd60000000p+0f,
    0x1.058bf20000000p+0f, 0x1.059b0e0000000p+0f, 0x1.05aa2a0000000p+0f, 0x1.05b9480000000p+0f, 0x1.05c8660000000p+0f,
    0x1.05d7860000000p+0f, 0x1.05e6a60000000p+0f, 0x1.05f5c80000000p+0f, 0x1.0604ea0000000p+0f, 0x1.06140c0000000p+0f,
    0x1.0623300000000p+0f, 0x1.0632540000000p+0f, 0x1.06417a0000000p+0f, 0x1.0650a00000000p+0f, 0x1.065fc80000000p+0f,
    0x1.066ef00000000p+0f, 0x1.067e1a0000000p+0f, 0x1.068d440000000p+0f, 0x1.069c6e0000000p+0f, 0x1.06ab9a0000000p+0f,
    0x1.06bac60000000p+0f, 0x1.06c9f40000000p+0f, 0x1.06d9220000000p+0f, 0x1.06e8520000000p+0f, 0x1.06f7820000000p+0f,
    0x1.0706b20000000p+0f, 0x1.0715e40000000p+0f, 0x1.0725180000000p+0f, 0x1.07344a0000000p+0f, 0x1.0743800000000p+0f,
    0x1.0752b40000000p+0f, 0x1.0761ea0000000p+0f, 0x1.0771220000000p+0f, 0x1.07805a0000000p+0f, 0x1.078f920000000p+0f,
    0x1.079ecc0000000p+0f, 0x1.07ae080000000p+0f, 0x1.07bd420000000p+0f, 0x1.07cc800000000p+0f, 0x1.07dbbc0000000p+0f,
    0x1.07eafa0000000p+0f, 0x1.07fa3a0000000p+0f, 0x1.08097a0000000p+0f, 0x1.0818ba0000000p+0f, 0x1.0827fc0000000p+0f,
    0x1.08373e0000000p+0f, 0x1.0846820000000p+0f, 0x1.0855c60000000p+0f, 0x1.08650c0000000p+0f, 0x1.0874520000000p+0f,
    0x1.0883980000000p+0f, 0x1.0892e00000000p+0f, 0x1.08a22a0000000p+0f, 0x1.08b1720000000p+0f, 0x1.08c0be0000000p+0f,
    0x1.08d0080000000p+0f, 0x1.08df540000000p+0f, 0x1.08eea20000000p+0f, 0x1.08fdf00000000p+0f, 0x1.090d3e0000000p+0f,
    0x1.091c8e0000000p+0f, 0x1.092be00000000p+0f, 0x1.093b300000000p+0f, 0x1.094a840000000p+0f, 0x1.0959d60000000p+0f,
    0x1.09692a0000000p+0f, 0x1.0978800000000p+0f, 0x1.0987d60000000p+0f, 0x1.09972c0000000p+0f, 0x1.09a6840000000p+0f,
    0x1.09b5de0000000p+0f, 0x1.09c5360000000p+0f, 0x1.09d4920000000p+0f, 0x1.09e3ec0000000p+0f, 0x1.09f3480000000p+0f,
    0x1.0a02a60000000p+0f, 0x1.0a12040000000p+0f, 0x1.0a21620000000p+0f, 0x1.0a30c20000000p+0f, 0x1.0a40240000000p+0f,
    0x1.0a4f840000000p+0f, 0x1.0a5ee80000000p+0f, 0x1.0a6e4a0000000p+0f, 0x1.0a7dae0000000p+0f, 0x1.0a8d140000000p+0f,
    0x1.0a9c7a0000000p+0f, 0x1.0aabe00000000p+0f, 0x1.0abb480000000p+0f, 0x1.0acab00000000p+0f, 0x1.0ada1a0000000p+0f,
    0x1.0ae9840000000p+0f, 0x1.0af8f00000000p+0f, 0x1.0b085c0000000p+0f, 0x1.0b17ca0000000p+0f, 0x1.0b27380000000p+0f,
    0x1.0b36a60000000p+0f, 0x1.0b46160000000p+0f, 0x1.0b55860000000p+0f, 0x1.0b64f80000000p+0f, 0x1.0b746a0000000p+0f,
    0x1.0b83de0000000p+0f, 0x1.0b93520000000p+0f, 0x1.0ba2c80000000p+0f, 0x1.0bb23e0000000p+0f, 0x1.0bc1b40000000p+0f,
    0x1.0bd12c0000000p+0f, 0x1.0be0a40000000p+0f, 0x1.0bf01e0000000p+0f, 0x1.0bff980000000p+0f, 0x1.0c0f140000000p+0f,
    0x1.0c1e900000000p+0f, 0x1.0c2e0e0000000p+0f, 0x1.0c3d8c0000000p+0f, 0x1.0c4d0a0000000p+0f, 0x1.0c5c8a0000000p+0f,
    0x1.0c6c0c0000000p+0f, 0x1.0c7b8e0000000p+0f, 0x1.0c8b100000000p+0f, 0x1.0c9a940000000p+0f, 0x1.0caa180000000p+0f,
    0x1.0cb99c0000000p+0f, 0x1.0cc9220000000p+0f, 0x1.0cd8aa0000000p+0f, 0x1.0ce8320000000p+0f, 0x1.0cf7ba0000000p+0f,
    0x1.0d07440000000p+0f, 0x1.0d16ce0000000p+0f, 0x1.0d265a0000000p+0f, 0x1.0d35e60000000p+0f, 0x1.0d45740000000p+0f,
    0x1.0d55020000000p+0f, 0x1.0d64920000000p+0f, 0x1.0d74220000000p+0f, 0x1.0d83b20000000p+0f, 0x1.0d93440000000p+0f,
    0x1.0da2d60000000p+0f, 0x1.0db26a0000000p+0f, 0x1.0dc1fe0000000p+0f, 0x1.0dd1940000000p+0f, 0x1.0de12a0000000p+0f,
    0x1.0df0c20000000p+0f, 0x1.0e005a0000000p+0f, 0x1.0e0ff20000000p+0f, 0x1.0e1f8c0000000p+0f, 0x1.0e2f280000000p+0f,
    0x1.0e3ec40000000p+0f, 0x1.0e4e600000000p+0f, 0x1.0e5dfe0000000p+0f, 0x1.0e6d9c0000000p+0f, 0x1.0e7d3c0000000p+0f,
    0x1.0e8cdc0000000p+0f, 0x1.0e9c7c0000000p+0f, 0x1.0eac1e0000000p+0f, 0x1.0ebbc20000000p+0f, 0x1.0ecb660000000p+0f,
    0x1.0edb0a0000000p+0f, 0x1.0eeab00000000p+0f, 0x1.0efa560000000p+0f, 0x1.0f09fe0000000p+0f, 0x1.0f19a60000000p+0f,
    0x1.0f29500000000p+0f,
};

} // namespace

float PitchRatio(int pitch)
{
    int octave = 0;
    if (pitch < 0)
    {
        const int down = (3071 - pitch) / 3072;
        octave = -down;
        pitch += down * 3072;
    }
    if (pitch >= 3072)
    {
        const int up = pitch / 3072;
        octave += up;
        pitch -= up * 3072;
    }

    // Apply 2^octave, then the semitone and fractional tables in the game's order. The game handles octaves in pairs,
    // but powers of two multiply exactly. The result is the same.
    float ratio = 1.0f;
    for (int i = 0; i < octave; i++)
    {
        ratio *= 2.0f;
    }
    for (int i = 0; i > octave; i--)
    {
        ratio *= 0.5f;
    }

    return kFineTable[pitch % 256] * (kSemitoneTable[pitch / 256] * ratio);
}

void Lfo::Set(uint8_t depth, uint8_t speed, uint8_t range, uint32_t delay_ms)
{
    // Store MML parameters as SequenceTrack does.
    depth_ = static_cast<float>(depth) * 0.0078125f;
    speed_ = static_cast<float>(speed) * 0.390625f;
    range_ = range;
    delay_ = delay_ms;
}

void Lfo::Update(int msec)
{
    if (delay_counter_ < delay_)
    {
        if (delay_counter_ + static_cast<uint32_t>(msec) <= delay_)
        {
            delay_counter_ += static_cast<uint32_t>(msec);
            return;
        }

        msec -= static_cast<int>(delay_ - delay_counter_);
        delay_counter_ = delay_;
    }

    // Wrap the phase to its fractional part.
    const float phase = phase_ + speed_ * (static_cast<float>(msec) * kOneThousandth);
    phase_ = phase - static_cast<float>(static_cast<int>(phase));
}

float Lfo::Value() const
{
    if (depth_ == 0.0f || delay_ > delay_counter_)
    {
        return 0.0f;
    }

    // Read the quarter sine forwards, backwards, then repeat with negative sign. Phase is below 1. The index therefore
    // stays in bounds.
    const int p = static_cast<int>(phase_ * 128.0f);
    int sine;
    if (p < 32)
    {
        sine = kLfoSine[p];
    }
    else if (p < 64)
    {
        sine = kLfoSine[64 - p];
    }
    else if (p < 96)
    {
        sine = -kLfoSine[p - 64];
    }
    else
    {
        sine = -kLfoSine[128 - p];
    }

    return static_cast<float>(range_) * (depth_ * (static_cast<float>(sine) * kOneOver127));
}

} // namespace citrusf2
