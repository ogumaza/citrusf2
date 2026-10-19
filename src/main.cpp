// SPDX-License-Identifier: MIT

// citrusf2: convert 3DS sound archive sequences (.bcsar) to MIDI, SoundFont and SFZ.
//
//     citrusf2 [options] <archive.bcsar> [<archive.bcsar> ...]
//
// Output goes beside each archive. Windows accepts files dropped on the executable; macOS uses Citrusf2.app.

#include "ntdlib/sar/formats.h"
#include "ntdlib/sequence_export/archive_converter.h"
#include "ntdlib/sequence_export/consoles.h"

int main(int argc, char** argv)
{
    ntdlib::sequence_export::ArchiveConverter<ntdlib::sar::SoundArchive> converter;
    converter.program = "citrusf2";
    converter.version_line = "citrusf2 " CITRUSF2_VERSION;
    converter.description =
        "citrusf2: convert 3DS sound archive sequences (.bcsar) to MIDI, SoundFont 2 and\n"
        "SFZ.\n";
    converter.extension = ".bcsar";
    converter.settings = ntdlib::sequence_export::CtrSettings();

    return ntdlib::sequence_export::RunArchiveConverter(argc, argv, converter);
}
