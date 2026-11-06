// SPDX-License-Identifier: MIT

// Music that a game plays as several sequences together. A game can start several sequences in one sound frame and play
// them as one piece. citrusf2 converts such a piece to one MIDI file, one SoundFont and one set of SFZ files (ntdlib's
// ConvertTogether).
//
// Mighty Gunvolt Burst plays each piece of its music as five sequences, one for each channel of the NES's sound chip:
// the DPCM channel, the noise, the two pulse channels and the triangle. Their labels are one name followed by "_DPCM",
// "_NOISE", "_P1", "_P2" and "_TRI". The game's table at 0x49f000 gives each of its 27 pieces' five sounds in that
// order. The function at 0x2d1b24 starts a piece. It calls 0x2cd2b0 for each channel in turn, in one sound frame.

#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "ntdlib/sar/formats.h"
#include "ntdlib/sequence_export/frame.h"

namespace citrusf2
{

// A piece that a game plays as several sequences: its name, and its sequences' sounds, in the order in which the game
// starts them.
struct Piece
{
    std::string name;
    std::vector<uint32_t> sounds;
};

// The pieces of `archive`, in the order of their first sounds. A piece's sequences have the labels of Mighty Gunvolt
// Burst's music: one name followed by "_DPCM", "_NOISE", "_P1", "_P2" and "_TRI". The piece takes that name. Of two
// sequences with one label, the first counts.
std::vector<Piece> FindPieces(const ntdlib::sar::SoundArchive& archive);

// The piece of `pieces` whose first sound is `index`, or none.
std::optional<Piece> PieceAt(const std::vector<Piece>& pieces, std::size_t index);

// The sequences of `archive` for the frame, in the order of its sounds. A piece takes the place of its first sequence,
// with the piece's name and its sequences' labels. Its other sequences leave the list.
std::vector<ntdlib::sequence_export::Sequence> ArchiveSequences(const ntdlib::sar::SoundArchive& archive);

} // namespace citrusf2
