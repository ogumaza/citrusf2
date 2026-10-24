// SPDX-License-Identifier: MIT

// citrusf2: convert 3DS sound archive sequences (.bcsar) to MIDI, SoundFont and SFZ.
//
//     citrusf2 [options] <archive.bcsar> [<archive.bcsar> ...]
//
// Output goes beside each archive. Windows accepts files dropped on the executable; macOS uses Citrusf2.app.

#include <string>

#include "ntdlib/sar/formats.h"
#include "ntdlib/sequence_export/archive_converter.h"
#include "ntdlib/sequence_export/consoles.h"
#include "ntdlib/sequence_export/frame.h"
#include "ntdlib/sequence_export/performance.h"
#include "ntdlib/sequence_export/settings.h"

namespace frame = ntdlib::sequence_export;

int main(int argc, char** argv)
{
    frame::ArchiveConverter<ntdlib::sar::SoundArchive> converter;
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

    return frame::RunArchiveConverter(argc, argv, converter);
}
