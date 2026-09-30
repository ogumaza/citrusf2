// SPDX-License-Identifier: MIT

// Sequence conversion to MIDI, SoundFont and SFZ.

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "csar.h"
#include "performance.h"
#include "sfz.h"

namespace citrusf2
{

// Converted files, performance data and statistics for the conversion report.
struct Conversion
{
    Performance performance;
    std::vector<uint8_t> midi;
    std::vector<uint8_t> sf2;
    SfzInstruments sfz;
    std::size_t samples = 0; // SoundFont samples (two per stereo wave)
    int channels = 0;        // MIDI channels used
    std::vector<std::string> warnings;
};

// Perform sequence `sound_index` and build its MIDI, SoundFont and SFZ files. Throw FormatError if the sequence cannot
// be read.
Conversion ConvertSequence(const SoundArchive& archive, uint32_t sound_index, const PerformOptions& options = {});

// Make a sequence name safe for file and directory names. Replace characters forbidden by Windows and trailing
// dots/spaces with '_'. Append '_' to Windows device names such as CON or NUL. Used for the MIDI/SoundFont base name
// and the SFZ directory.
std::string FileName(const std::string& sequence_name);

} // namespace citrusf2
