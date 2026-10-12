// SPDX-License-Identifier: MIT

// Convert every sequence of a sound archive (.bcsar) with citrusf2's settings, and check each conversion with the
// readers of seqlib. ntdlib's tests hold the conversion's unit tests and run them with these settings.

#include "ntdlib/sequence_export/consoles.h"
#include "ntdlib/testing/sequence_export_check.h"

int main(int argc, char** argv)
{
    return ntdlib::testing::RunArchiveCheck(argc, argv, ntdlib::sequence_export::CtrSettings(), "citrusf2", ".bcsar");
}
