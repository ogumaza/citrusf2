// SPDX-License-Identifier: MIT

// citrusf2: convert 3DS sound archive sequences (.bcsar) to MIDI, SoundFont and SFZ.
//
//     citrusf2 [options] <archive.bcsar> [<archive.bcsar> ...]
//
// Output goes beside each archive. Windows accepts files dropped on the executable; macOS uses Citrusf2.app.

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <system_error>
#include <vector>

#include "ntdlib/sar/formats.h"
#include "ntdlib/sequence_export/archive_converter.h"
#include "ntdlib/sequence_export/consoles.h"
#include "ntdlib/sequence_export/convert.h"
#include "ntdlib/sequence_export/frame.h"
#include "ntdlib/sequence_export/performance.h"
#include "ntdlib/sequence_export/settings.h"

#include "pieces.h"
#include "profiles.h"

namespace frame = ntdlib::sequence_export;

using ntdlib::sar::SoundArchive;

namespace
{

// The profile of the archive at `input`, or none when the file can't be read. The frame then reports the failure.
// Another build of an archive with a profile gets a note.
citrusf2::Profile ProfileOfFile(const std::filesystem::path& input, const std::string& name)
{
    const std::optional<std::vector<uint8_t>> bytes = frame::ReadFile(input);
    if (!bytes)
    {
        return citrusf2::Profile::kNone;
    }

    if (const char* game = citrusf2::OtherBuildOf(*bytes))
    {
        std::fprintf(stderr,
                     "note: %s looks like the sound archive of %s, but not the build that citrusf2 knows. A sequence "
                     "that citrusf2 has no start for in this build plays every track\n",
                     name.c_str(), game);
    }

    return citrusf2::ProfileOf(*bytes);
}

// Whether a group file (.bcgrp) lies beside the archive at `input`. Some games keep sequences in group files outside
// the archive. Mighty Gunvolt Burst keeps all of them there.
bool HasGroupFilesBeside(const std::filesystem::path& input)
{
    const std::filesystem::path folder = input.has_parent_path() ? input.parent_path() : std::filesystem::path(".");
    std::error_code error;
    for (const auto& entry : std::filesystem::directory_iterator(folder, error))
    {
        if (entry.path().extension() == ".bcgrp")
        {
            return true;
        }
    }

    return false;
}

} // namespace

int main(int argc, char** argv)
{
    frame::ArchiveConverter<SoundArchive> converter;
    converter.program = "citrusf2";
    converter.version_line = "citrusf2 " CITRUSF2_VERSION;
    converter.description =
        "citrusf2: convert 3DS sound archive sequences (.bcsar) to MIDI, SoundFont 2 and\n"
        "SFZ.\n";
    converter.extension = ".bcsar";
    converter.settings = frame::CtrSettings();

    // A game's code puts the effects on the aux buses. Some games put a reverb on bus A. --reverb writes send A as
    // CC91, the synth's reverb send. The search for the main loop leaves CC91 out. The loops then stay those of a
    // conversion without --reverb.
    converter.extra_options =
        "  --reverb       write effect send A as CC91 (reverb), for games that put a\n"
        "                 reverb on bus A\n";
    converter.option =
        [&converter](const std::string& option, frame::OptionReader& reader, frame::PerformOptions& perform)
    {
        if (option == "--reverb")
        {
            converter.settings.sends = frame::Sends::kReverb;
            converter.settings.reverb_moves_loops = false;
            return true;
        }

        return frame::VarOption(option, reader, perform);
    };

    // An archive's profile sets up the sounds that the game's code sets up as it plays them (profiles.h). The profile
    // comes from the archive's bytes, before the frame reads the archive.
    citrusf2::Profile profile = citrusf2::Profile::kNone;
    frame::Converter<frame::PerformOptions, SoundArchive, frame::Conversion> archive_converter =
        frame::MakeArchiveConverter(converter);
    const auto open_archive = archive_converter.open;
    archive_converter.open = [open_archive, &profile](const std::filesystem::path& input, const std::string& name)
    {
        profile = ProfileOfFile(input, name);
        std::optional<SoundArchive> archive = open_archive(input, name);
        if (archive && HasGroupFilesBeside(input))
        {
            frame::AddGroupFiles(*archive, input, name);
        }

        return archive;
    };

    // A sound's arrangements follow the sound (citrusf2::ArrangementsOf). Arrangement k of sound s gets the index
    // (k + 1) * n + s. n is the archive's sound count. A piece of several sequences takes the place of its first
    // (pieces.h).
    const auto arrangements_of = [&profile](const SoundArchive& archive, std::size_t sound)
    {
        const std::vector<uint32_t>& banks = archive.Sounds()[sound].banks;
        return citrusf2::ArrangementsOf(profile, archive.Sounds()[sound].name,
                                        banks.empty() ? ntdlib::sar::kNoItem : banks.front());
    };

    archive_converter.sequences = [&arrangements_of](const SoundArchive& archive)
    {
        const std::size_t sounds = archive.Sounds().size();
        std::vector<frame::Sequence> sequences;
        for (const frame::Sequence& sequence : citrusf2::ArchiveSequences(archive))
        {
            sequences.push_back(sequence);
            const std::span<const citrusf2::Arrangement> arrangements = arrangements_of(archive, sequence.index);
            for (std::size_t k = 0; k < arrangements.size(); k++)
            {
                sequences.push_back({.index = (k + 1) * sounds + sequence.index,
                                     .name = sequence.name + std::string(arrangements[k].suffix)});
            }
        }

        return sequences;
    };

    archive_converter.convert = [&converter, &profile, &arrangements_of](SoundArchive& archive, std::size_t index,
                                                                         const frame::PerformOptions& perform)
    {
        const std::size_t sounds = archive.Sounds().size();
        const std::string& name = archive.Sounds()[index % sounds].name;
        frame::PerformOptions options = perform;
        citrusf2::SetUpSound(profile, name, options);

        if (index >= sounds)
        {
            const citrusf2::Arrangement& arrangement = arrangements_of(archive, index % sounds)[index / sounds - 1];
            options.tracks = arrangement.tracks;

            if (arrangement.variable >= 0 && arrangement.frame == 0)
            {
                options.variables[static_cast<uint8_t>(arrangement.variable)] = arrangement.value;
            }
            if (arrangement.variable >= 0 && arrangement.frame > 0)
            {
                options.changes.push_back({.frame = arrangement.frame,
                                           .index = static_cast<uint8_t>(arrangement.variable),
                                           .value = arrangement.value});
            }

            options.tempo_ratio = arrangement.tempo_ratio;
            options.pitch = arrangement.pitch;
            options.banks = arrangement.banks;
            options.volume = arrangement.volume;
            options.track_volumes = arrangement.track_volumes;
        }
        else if (const std::optional<citrusf2::Piece> piece = citrusf2::PieceAt(citrusf2::FindPieces(archive), index))
        {
            return frame::ConvertTogether(archive, piece->sounds, piece->name, options, converter.settings,
                                          converter.program);
        }

        return frame::ConvertSequence(archive, static_cast<uint32_t>(index % sounds), options, converter.settings,
                                      converter.program);
    };

    const auto run = [&archive_converter](const std::vector<std::string>& args)
    {
        return frame::Run(args, archive_converter);
    };

    return frame::RunMain(argc, argv, run);
}
