// SPDX-License-Identifier: MIT

// Run CSEQ bytecode and record note, controller and program events for MIDI output.
//
// Jumps, calls, variables, conditions and random arguments require execution rather than static translation. The
// performer follows Pokemon X's nw::snd player: MML parsing, track state, tick scheduling, note lengths and note waits.

#pragma once

#include <array>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "csar.h"
#include "envelope.h"
#include "filter.h"

namespace citrusf2
{

class BankSet;

// Note-on settings that a MIDI channel cannot represent. Each distinct PresetKey produces a SoundFont preset.
struct PresetKey
{
    bool operator==(const PresetKey&) const = default;

    uint8_t bank_slot = 0; // index into the sound's bank list (0-3; higher slots have no bank)
    uint32_t program = 0;

    // Track envelope overrides (0xff: none, the bank region's value applies).
    uint8_t attack = 0xff, decay = 0xff, sustain = 0xff, release = 0xff, hold = 0xff;

    // Track LFO parameters in MML units; depth is carried separately by CC1. Pan LFO is supported by SFZ only. Other
    // types have no effect in nw::snd. Negative delays prevent the LFO from starting, so those LFOs are omitted too.
    uint8_t lfo_type = 0; // 0 pitch, 1 volume, 2 pan, 3 none
    uint8_t lfo_range = 1;
    uint8_t lfo_speed = 16; // Hz = speed * 100 / 256 (in envelope time, see envelope.h)
    uint16_t lfo_delay = 0; // in 5 ms units

    int8_t init_pan = 0; // -64..63, added to the region pan

    // Track filter (see filter.h). Filters that change during notes use kFilterController instead of a fixed cutoff.
    // This loses the biquad low-pass's slight resonance.
    SoundFontFilter filter;
    bool filter_controller = false;
};

// Apply the track envelope overrides in `key` to `env`.
EnvelopeValues OverrideEnvelope(EnvelopeValues env, const PresetKey& key);

// Event types, with the fields used by each.
enum class EventKind : uint8_t
{
    kNoteOn,     // key, value: velocity
    kNoteOff,    // key
    kPreset,     // preset: index into Performance::presets
    kVolume,     // value: 0-127 (CC7)
    kExpression, // value: 0-127 (CC11)
    kPan,        // value: 0-127, 64 centre (CC10)
    kPitch,      // semitones: pitch bend plus sweep/portamento
    kModulation, // value: LFO depth / 2 (CC1)
    kReverb,     // value: effect send A (CC91)
    kChorus,     // value: effect send B (CC93)
    kSoundOff,   // cut every sound on the channel at once (CC120)
    kFilter,     // value: the filter's cutoff (kFilterController, see filter.h)
    kSfzLowPass, // value: the low-pass setting for the SFZ files (kSfzLowPassController)
    kSfzBiquad,  // value: the biquad setting for the SFZ files (kSfzBiquadController)
};

// An event on a track's timeline.
struct TrackEvent
{
    uint32_t tick = 0;
    EventKind kind = EventKind::kNoteOn;
    uint8_t key = 0; // 0-127
    uint8_t value = 0;
    uint32_t preset = 0;
    float semitones = 0.0f;
    uint32_t note = 0; // NoteOn/NoteOff ID, starting at 1; distinguishes overlapping notes of the same key
};

// Recorded sequence: tempo, track events, presets, main loop and duration.
struct Performance
{
    // A tempo change.
    struct Tempo
    {
        uint32_t tick = 0;
        double bpm = 120.0; // quarter notes (`timebase` ticks) per minute
    };

    // The main loop, in ticks.
    struct Loop
    {
        uint32_t start = 0, end = 0;
    };

    uint32_t timebase = 48; // ticks per quarter note, 1-255
    std::vector<Tempo> tempo;
    std::array<std::vector<TrackEvent>, 16> tracks;
    std::vector<PresetKey> presets;

    // Biquad type per track (1-5), or 0 for none. SFZ uses the first type each track enables.
    std::array<uint8_t, 16> biquad_types{};

    // MIDI attenuation in dB. Game volumes reach 255, but CC11 stops at 127. Scale all CC11 values equally to fit,
    // preserving track balance. SFZ adds this gain back.
    double level_cut_db = 0.0;

    // Start of each tick's sound frame, expressed as a fractional tick. MIDI events are placed at these positions.
    std::vector<double> frame_ticks;

    uint32_t end_tick = 0;
    double seconds = 0;       // length, release tails included
    std::optional<Loop> loop; // if it loops
    int loops = 0;            // times the main loop was played
    bool truncated = false;   // stopped at the time limit
    bool holds = false;       // holds until stopped by the game; subject to the hold limit
    std::vector<std::string> warnings;

    // Counts of features approximated or omitted in MIDI/SoundFont output.
    std::map<std::string, uint32_t> approximations;
};

// Options for playing a sequence.
struct PerformOptions
{
    // Main loop repetitions, at least 1. The default plays the intro and one loop, with MIDI loop markers.
    int loops = 1;

    // Initial nw::snd PRNG state. The game also advances it every sound frame, so its state depends on elapsed time.
    // 0x12345678 is the startup value.
    uint32_t seed = 0x12345678;
};

// Perform sequence `sound_index` from `archive`. Throw FormatError for corrupt data.
Performance Perform(const SoundArchive& archive, uint32_t sound_index, BankSet& banks, const PerformOptions& options);

// Returns the number of notes that `perf` plays.
uint32_t NoteCount(const Performance& perf);

} // namespace citrusf2
