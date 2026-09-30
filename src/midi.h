// SPDX-License-Identifier: MIT

// Standard MIDI File output for a Performance.

#pragma once

#include <array>
#include <cstdint>
#include <set>
#include <string>
#include <vector>

#include "performance.h"

namespace citrusf2
{

// MIDI channel allocation, bend ranges and conversion warnings.
struct MidiReport
{
    int channels = 0;                      // MIDI channels used
    std::array<int, 16> bend_ranges{};     // each track's pitch bend range (RPN 0), in semitones
    uint32_t cut_notes = 0;                // overlapping notes cut short for lack of channels
    std::set<uint32_t> channel_10_presets; // presets that MIDI channel 10 plays
    std::vector<std::string> warnings;
};

// Write Format 1 MIDI: a conductor track for tempo and loop markers, plus a track for each sequence track/channel pair.
// Use at least 7680 ticks per quarter note, with an integer number of MIDI ticks per performance tick. Place events at
// the start of their sound frame (Performance::frame_ticks). SoundFont preset i uses CC0 = i / 128 and program = i %
// 128. Fill `report` with allocation details and warnings.
//
// A MIDI channel supports one active note per key. Repeated strikes that overlap get extra channels (layers) with
// matching controllers, up to the 16-channel limit. Allocate in track order, using channel 10 (index 9, General MIDI
// drums) last. Synths use bank 128 for that channel regardless of bank select, so record its presets in `report` for
// the SoundFont writer to copy there.
std::vector<uint8_t> WriteMidi(const Performance& perf, const std::string& title, MidiReport& report);

} // namespace citrusf2
