// SPDX-License-Identifier: MIT

// Track filters and their SoundFont/SFZ equivalents.
//
// A track has a low-pass filter (0xd8) and a biquad filter (0xb4 type, 0xb5 value). nw::snd updates both on sounding
// voices every frame (SequenceTrack::UpdateChannelParam, code.bin 0x31de18). The low-pass has one pole and 23 cutoffs,
// from 80 to 12800 Hz. The biquad selects coefficients from five preset tables: one low-pass, one high-pass and three
// band-passes, all Robert Bristow-Johnson cookbook filters.
//
// SoundFont provides one resonant two-pole low-pass. It matches the biquad low-pass and approximates the one-pole
// filter with fitted cutoffs. High-pass and band-pass are omitted. When both low-passes are active, use the lower
// cutoff.
//
// SFZ supports all these filters: sfizz's lpf_1p matches the DSP one-pole; lpf_2p, hpf_2p and bpf_2p are cookbook
// filters. The track settings are sent on two MIDI controllers.
//
// Generate filter.cpp's tables from the game code with tools/filter_tables.py.

#pragma once

#include <cstdint>

namespace citrusf2
{

// Open SoundFont filter cutoff (initialFilterFc, absolute cents).
constexpr uint16_t kFilterOpen = 13500;

// SoundFont cutoff controller for filters that change during a note. A modulator subtracts kFilterControllerRange *
// value / 128 cents from kFilterOpen: 75 cents per step. Zero leaves the filter open.
constexpr uint8_t kFilterController = 102;
constexpr int kFilterControllerRange = 9600;

// Returns the value of kFilterController that comes closest to `cutoff`.
uint8_t FilterControllerValue(uint16_t cutoff);

// SFZ filter controllers. Low-pass uses 64 minus the 0xd8 argument (6 or less means off). Biquad uses the 0xb5
// argument, or 0 for types without a preset. The default controller value of 0 disables both.
constexpr uint8_t kSfzLowPassController = 103;
constexpr uint8_t kSfzBiquadController = 104;

// Returns the value of kSfzLowPassController for the 0xd8 argument `low_pass`.
uint8_t SfzLowPassValue(int low_pass);

// Returns the value of kSfzBiquadController for the 0xb4 and 0xb5 arguments `biquad_type` and `biquad_value`.
uint8_t SfzBiquadValue(int biquad_type, int biquad_value);

// Active filters derived from a track's settings.
struct GameFilters
{
    int low_pass = -1;       // kLpfFreq index (0-22), or -1 for off
    uint8_t biquad_type = 0; // 1 low-pass, 2 high-pass, 3-5 band-pass, 0 off
    int biquad_step = 0;     // index into the preset's coefficient table
};

// Decode 0xd8 (`low_pass`), 0xb4 (`biquad_type`) and 0xb5 (`biquad_value`) as nw::snd and nn::snd do.
GameFilters TrackFilters(int low_pass, int biquad_type, int biquad_value);

// A SoundFont filter setting.
struct SoundFontFilter
{
    bool operator==(const SoundFontFilter&) const = default;

    uint16_t cutoff = kFilterOpen; // initialFilterFc, in absolute cents
    uint8_t resonance = 0;         // initialFilterQ, in centibels
};

// Convert `filters` to a SoundFont filter approximation.
SoundFontFilter ToSoundFont(const GameFilters& filters);

// Open SFZ low-pass cutoff, lowered by kSfzLowPassController. This is sfizz's maximum for lpf_1p.
constexpr int kSfzLowPassOpen = 20000; // Hz

// Return the SFZ lpf_1p cutoff matching kLpfFreq[`index`] (0-22), in cents relative to kSfzLowPassOpen.
int SfzLowPassCutoff(int index);

// Reference frequency for SFZ biquad cutoffs.
constexpr int kSfzBiquadBase = 1000; // Hz

// An SFZ biquad setting: cutoff/centre frequency in cents relative to kSfzBiquadBase, plus resonance (Q in dB) and gain
// in 0.01 dB units.
struct SfzBiquad
{
    int16_t cutoff = 0;
    int16_t resonance = 0;
    int16_t gain = 0;
};

// Returns biquad preset `type` (1-5) at `step` as an SFZ filter.
SfzBiquad SfzBiquadStep(int type, int step);

} // namespace citrusf2
