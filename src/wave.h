// SPDX-License-Identifier: MIT

// CWAV decoding to 16-bit PCM.

#pragma once

#include <cstdint>
#include <span>
#include <vector>

namespace citrusf2
{

// Decoded PCM for the first two wave channels, with loop_end samples per channel.
struct Pcm
{
    uint32_t Length() const
    {
        return loop_end;
    }

    uint32_t sample_rate = 32728;
    bool loop = false;       // only when loop_start < loop_end
    uint32_t loop_start = 0; // first sample of the loop
    uint32_t loop_end = 0;   // one past the last sample played (the length)
    std::vector<std::vector<int16_t>> channels;
};

// Like nw::snd, decode the first two CWAV channels. Supports DSP-ADPCM, PCM8, PCM16 and IMA-ADPCM. Throw FormatError on
// failure.
Pcm DecodeWave(std::span<const uint8_t> cwav);

// Samples and loop bounds prepared for SoundFont or SFZ output.
struct PreparedWave
{
    std::vector<std::vector<int16_t>> channels;
    bool loop = false;
    uint32_t loop_start = 0, loop_end = 0;
};

// Repeat loop data to meet SoundFont requirements: 8 samples before the loop, at least 32 within it and 8 after it.
// Playback is unchanged.
PreparedWave PrepareForSoundFont(const Pcm& pcm);

// Prepare samples for sfizz. Its loop crossfade spans at least 1 ms and blends the loop end with data before the loop
// start. Prepend 4 ms of repeated loop data so the crossfade blends identical samples. Extend the loop to at least 64
// samples and append 64 samples from its start for interpolation. Playback is unchanged.
PreparedWave PrepareForSfz(const Pcm& pcm);

// Decode DSP-ADPCM (14 samples per 8-byte frame). Exposed for tests.
std::vector<int16_t> DecodeDspAdpcm(std::span<const uint8_t> data, uint32_t samples, std::span<const int16_t, 16> coefs,
                                    int16_t hist1, int16_t hist2);

} // namespace citrusf2
