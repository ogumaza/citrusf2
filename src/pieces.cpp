// SPDX-License-Identifier: MIT

// Music that a game plays as several sequences together (see pieces.h).

#include "pieces.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "ntdlib/sar/formats.h"
#include "ntdlib/sequence_export/archive_converter.h"
#include "ntdlib/sequence_export/frame.h"

namespace citrusf2
{

namespace
{

// The ends of the labels of a piece's sequences, in the order in which the game starts them.
constexpr std::array<std::string_view, 5> kSuffixes = {"_DPCM", "_NOISE", "_P1", "_P2", "_TRI"};

} // namespace

std::vector<Piece> FindPieces(const ntdlib::sar::SoundArchive& archive)
{
    // The sequences by label. A later sequence with an earlier one's label doesn't replace it.
    const std::vector<ntdlib::sar::SoundInfo>& sounds = archive.Sounds();
    std::map<std::string, uint32_t, std::less<>> sequences;
    for (uint32_t i = 0; i < sounds.size(); i++)
    {
        if (sounds[i].type == ntdlib::sar::SoundType::kSequence)
        {
            sequences.emplace(sounds[i].name, i);
        }
    }

    // Each piece starts at its first sequence. Every one of its labels has to be there.
    std::vector<Piece> pieces;
    for (uint32_t i = 0; i < sounds.size(); i++)
    {
        const std::string& label = sounds[i].name;
        if (sounds[i].type != ntdlib::sar::SoundType::kSequence || label.size() <= kSuffixes[0].size() ||
            !label.ends_with(kSuffixes[0]))
        {
            continue;
        }

        Piece piece{label.substr(0, label.size() - kSuffixes[0].size()), {}};
        for (const std::string_view suffix : kSuffixes)
        {
            const auto found = sequences.find(piece.name + std::string(suffix));
            if (found == sequences.end())
            {
                break;
            }

            piece.sounds.push_back(found->second);
        }

        if (piece.sounds.size() == kSuffixes.size() && piece.sounds.front() == i)
        {
            pieces.push_back(std::move(piece));
        }
    }

    return pieces;
}

std::optional<Piece> PieceAt(const std::vector<Piece>& pieces, std::size_t index)
{
    for (const Piece& piece : pieces)
    {
        if (piece.sounds.front() == index)
        {
            return piece;
        }
    }

    return std::nullopt;
}

std::vector<ntdlib::sequence_export::Sequence> ArchiveSequences(const ntdlib::sar::SoundArchive& archive)
{
    // The sequences of the pieces after their first.
    const std::vector<Piece> pieces = FindPieces(archive);
    std::set<std::size_t> later;
    for (const Piece& piece : pieces)
    {
        later.insert(piece.sounds.begin() + 1, piece.sounds.end());
    }

    std::vector<ntdlib::sequence_export::Sequence> sequences;
    for (ntdlib::sequence_export::Sequence& sequence : ntdlib::sequence_export::ArchiveSequences(archive))
    {
        if (later.contains(sequence.index))
        {
            continue;
        }

        if (const std::optional<Piece> piece = PieceAt(pieces, sequence.index))
        {
            sequence.name = piece->name;
            for (const uint32_t sound : piece->sounds)
            {
                sequence.labels.push_back(archive.Sounds()[sound].name);
            }
        }

        sequences.push_back(std::move(sequence));
    }

    return sequences;
}

} // namespace citrusf2
