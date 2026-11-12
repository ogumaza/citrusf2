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
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include "ntdlib/ctr/snd/starting_tracks.h"
#include "ntdlib/sar/formats.h"
#include "ntdlib/sequence_export/archive_converter.h"
#include "ntdlib/sequence_export/arrangements.h"
#include "ntdlib/sequence_export/consoles.h"
#include "ntdlib/sequence_export/convert.h"
#include "ntdlib/sequence_export/frame.h"
#include "ntdlib/sequence_export/performance.h"
#include "ntdlib/sequence_export/settings.h"
#include "ntdlib/sound_pieces.h"
#include "xsflib/sha1.h"

#include "profiles.h"

namespace frame = ntdlib::sequence_export;

using ntdlib::ctr::snd::SequenceArrangement;
using ntdlib::sar::SoundArchive;

namespace
{

// What the hooks find in the archive that the frame has open. The SHA-1 of the archive file picks the starts of the
// sequences (profiles.h). The pieces of several sequences come from the labels (ntdlib/sound_pieces.h). Mighty Gunvolt
// Burst plays each piece of its music as five sequences. citrusf2 converts each piece to one set of files
// (ConvertTogether in ntdlib).
struct CurrentArchive
{
    std::string sha1;
    std::vector<ntdlib::SoundPiece> pieces;
};

// Whether a group file (.bcgrp) lies beside the archive at `input`. Some games keep sequences in group files outside
// the archive. Mighty Gunvolt Burst keeps all of them there.
bool HasGroupFilesBeside(const std::filesystem::path& input)
{
    const std::filesystem::path folder = input.has_parent_path() ? input.parent_path() : std::filesystem::path(".");
    std::error_code error;
    for (std::filesystem::directory_iterator entry(folder, error);
         !error && entry != std::filesystem::directory_iterator(); entry.increment(error))
    {
        if (entry->path().extension() == ".bcgrp")
        {
            return true;
        }
    }

    return false;
}

// Reads the sound archive at `input`, named `name`, with `settings`, and the group files beside it. Sets `current` to
// what it finds. Returns nothing if the file can't be read. The frame then reports the failure. Another build of an
// archive with a profile gets a note.
std::optional<SoundArchive> ReadArchive(const std::filesystem::path& input, const std::string& name,
                                        const frame::Settings& settings, CurrentArchive& current)
{
    current = {};
    std::optional<std::vector<uint8_t>> bytes = frame::ReadFile(input);
    if (!bytes)
    {
        return std::nullopt;
    }

    current.sha1 = xsflib::Sha1::Hex(xsflib::Sha1::Of(*bytes));
    if (const char* game = citrusf2::OtherBuildOf(current.sha1, *bytes))
    {
        std::fprintf(stderr,
                     "note: %s looks like the sound archive of %s, but not the build that citrusf2 knows. A sequence "
                     "that citrusf2 has no start for in this build plays every track\n",
                     name.c_str(), game);
    }

    SoundArchive archive = frame::LoadArchive<SoundArchive>(std::move(*bytes), name, settings);
    if (HasGroupFilesBeside(input))
    {
        frame::AddGroupFiles(archive, input, name);
    }

    current.pieces = ntdlib::FindSoundPieces(archive, ntdlib::kMightyGunvoltBurstChannels);

    return archive;
}

// The other states that the game sets for each sound of `archive` as it plays (citrusf2::ArrangementsOf).
frame::ArrangementsOf ArrangementsOf(const CurrentArchive& current, const SoundArchive& archive)
{
    return [sha1 = current.sha1, &archive](std::size_t sound)
    {
        const ntdlib::sar::SoundInfo& info = archive.Sounds()[sound];
        std::vector<frame::Arrangement> arrangements;
        for (const SequenceArrangement& arrangement :
             citrusf2::ArrangementsOf(sha1, info.name, info.banks.empty() ? ntdlib::sar::kNoItem : info.banks.front()))
        {
            arrangements.push_back({std::string(arrangement.suffix), [arrangement](frame::PerformOptions& options)
                                    { citrusf2::SetUpArrangement(arrangement, options); }});
        }

        return arrangements;
    };
}

// Converts entry `index` of the frame's list of `archive` with `perform` and `settings`, as the game starts it. A sound
// uses the start in ntdlib's list (StartingSequenceTracks). An arrangement uses the start that the arrangement sets. An
// arrangement of the first sequence of a piece plays that sequence alone.
frame::Conversion ConvertEntry(const CurrentArchive& current, const SoundArchive& archive, std::size_t index,
                               const frame::PerformOptions& perform, frame::Settings settings,
                               const std::string& program)
{
    const frame::ArrangedSound at = frame::ArrangedSoundAt(index, archive.Sounds().size());
    frame::PerformOptions options = perform;
    if (at.arrangement)
    {
        frame::SetUpArrangement(at, ArrangementsOf(current, archive), options);
    }
    else
    {
        citrusf2::SetUpStart(ntdlib::ctr::snd::StartingSequenceTracks(current.sha1, archive.Sounds()[at.sound].name),
                             options, settings);
        if (const ntdlib::SoundPiece* piece = ntdlib::PieceStartingAt(current.pieces, at.sound))
        {
            return frame::ConvertTogether(archive, piece->sounds, piece->name, options, settings, program);
        }
    }

    return frame::ConvertSequence(archive, static_cast<uint32_t>(at.sound), options, settings, program);
}

// Fills in `converter`: the help, the 3DS settings and the --reverb option.
void SetUpConverter(frame::ArchiveConverter<SoundArchive>& converter)
{
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
}

// The frame converter of `converter`, with the citrusf2 hooks. The hooks keep what they find in the open archive in
// `current`. `converter` and `current` have to outlive the result.
frame::Converter<frame::PerformOptions, SoundArchive, frame::Conversion> MakeConverter(
    const frame::ArchiveConverter<SoundArchive>& converter, CurrentArchive& current)
{
    frame::Converter<frame::PerformOptions, SoundArchive, frame::Conversion> archive_converter =
        frame::MakeArchiveConverter(converter);
    archive_converter.open = [&converter, &current](const std::filesystem::path& input, const std::string& name)
    {
        return ReadArchive(input, name, converter.settings, current);
    };
    archive_converter.sequences = [&current](const SoundArchive& archive)
    {
        return frame::WithArrangements(frame::ArchiveSequences(archive, current.pieces), archive.Sounds().size(),
                                       ArrangementsOf(current, archive));
    };
    archive_converter.convert =
        [&converter, &current](SoundArchive& archive, std::size_t index, const frame::PerformOptions& perform)
    {
        return ConvertEntry(current, archive, index, perform, converter.settings, converter.program);
    };

    return archive_converter;
}

} // namespace

int main(int argc, char** argv)
{
    frame::ArchiveConverter<SoundArchive> converter;
    SetUpConverter(converter);
    CurrentArchive current;
    const frame::Converter<frame::PerformOptions, SoundArchive, frame::Conversion> archive_converter =
        MakeConverter(converter, current);
    const auto run = [&archive_converter](const std::vector<std::string>& args)
    {
        return frame::Run(args, archive_converter);
    };

    return frame::RunMain(argc, argv, run);
}
