// SPDX-License-Identifier: MIT

// SoundFont 2.01 file structure and writer.

#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace citrusf2::sf2
{

// Generator operators (SoundFont 2.01, section 8.1.2).
enum Gen : uint16_t
{
    kVibLfoToPitch = 6,
    kInitialFilterFc = 8,
    kInitialFilterQ = 9,
    kModLfoToVolume = 13,
    kPan = 17,
    kDelayModLfo = 21,
    kFreqModLfo = 22,
    kDelayVibLfo = 23,
    kFreqVibLfo = 24,
    kAttackVolEnv = 34,
    kHoldVolEnv = 35,
    kDecayVolEnv = 36,
    kSustainVolEnv = 37,
    kReleaseVolEnv = 38,
    kInstrument = 41,
    kKeyRange = 43,
    kVelRange = 44,
    kInitialAttenuation = 48,
    kCoarseTune = 51,
    kFineTune = 52,
    kSampleId = 53,
    kSampleModes = 54,
    kExclusiveClass = 57,
    kOverridingRootKey = 58,
};

// A generator: an operator and its amount.
struct Generator
{
    // A generator with a signed amount.
    static Generator Value(uint16_t oper, int v)
    {
        return {oper, static_cast<uint16_t>(static_cast<int16_t>(v))};
    }

    // Create a key-range or velocity-range generator.
    static Generator Range(uint16_t oper, int lo, int hi)
    {
        return {oper, static_cast<uint16_t>((lo & 0xff) | ((hi & 0xff) << 8))};
    }

    uint16_t oper = 0;
    uint16_t amount = 0; // signed values as two's complement; ranges as lo | hi << 8
};

// Modulator (section 8.2): source, destination generator, amount and optional amount-scaling source.
struct Modulator
{
    uint16_t source = 0;
    uint16_t dest = 0;
    int16_t amount = 0;
    uint16_t amount_source = 0;
};

struct Zone
{
    std::vector<Generator> generators;
    std::vector<Modulator> modulators;
};

struct Instrument
{
    std::string name;
    std::vector<Zone> zones; // a leading zone without a sample is global
};

struct Preset
{
    std::string name;
    uint16_t program = 0, bank = 0;
    std::vector<Zone> zones;
};

enum SampleType : uint16_t
{
    kMono = 1,
    kRight = 2,
    kLeft = 4
};

struct Sample
{
    std::string name;
    std::vector<int16_t> data;
    uint32_t sample_rate = 32728;
    uint8_t original_key = 60;
    uint32_t loop_start = 0, loop_end = 0; // relative to the sample's start
    uint16_t link = 0;                     // sample index of the other stereo channel
    uint16_t type = kMono;
};

struct SoundFont
{
    std::string name;
    std::string comment;
    std::vector<Sample> samples;
    std::vector<Instrument> instruments;
    std::vector<Preset> presets;
};

// Serialise a SoundFont. Order generators as required: key range, velocity range, other generators, then sample or
// instrument. Append 46 zero samples after each sample.
std::vector<uint8_t> Write(const SoundFont& sf);

} // namespace citrusf2::sf2
