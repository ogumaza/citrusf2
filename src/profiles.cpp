// SPDX-License-Identifier: MIT

// The profiles of games (see profiles.h).

#include "profiles.h"

#include <array>
#include <cstdint>
#include <span>
#include <stdexcept>
#include <string_view>
#include <vector>

#include "ntdlib/ctr/snd/starting_tracks.h"
#include "ntdlib/sequence_export/performance.h"
#include "ntdlib/sequence_export/settings.h"

namespace citrusf2
{

namespace
{

using ntdlib::ctr::snd::SequenceArrangement;
using ntdlib::ctr::snd::SequenceStart;
using ntdlib::ctr::snd::SequenceVariable;

// Whether `state` differs from `start` only in the sends and the effect on the buses.
bool DiffersOnlyInEffects(const SequenceStart& state, const SequenceStart& start)
{
    SequenceStart with_effects = start;
    with_effects.fx_send = state.fx_send;
    with_effects.game_effect = state.game_effect;

    return state != start && state == with_effects;
}

} // namespace

const char* OtherBuildOf(std::string_view archive_sha1, std::span<const uint8_t> archive)
{
    // A sound that only each archive holds, as a name of the archive's string table, between two zero bytes, and the
    // SHA-1 of the archive with the profile.
    struct Build
    {
        std::string_view label;
        std::string_view sha1;
        const char* game;
    };

    constexpr std::array<Build, 3> kBuilds = {{
        {std::string_view("\0SEQ_BGM_OP_INTRO\0", 18), "a98dd3c22da8e4bed8327e4fc98196e102ce53ea",
         "Ultimate Angler (add-on content v0.2.0, USA)"},
        {std::string_view("\0BGM_FLOWER_TOWN\0", 17), "331b572b09819830436fe8a09673b22fc6f7bdae",
         "Flower Town (add-on content v0.2.0, USA)"},
        {std::string_view("\0BGM_PEACE_MAIN\0", 16), "33b514ee80f6a6df3034bc8774235e40dfa52d1c",
         "StreetPass Mii Plaza (update v14.6.0)"},
    }};
    const std::string_view bytes(reinterpret_cast<const char*>(archive.data()), archive.size());
    for (const Build& build : kBuilds)
    {
        if (archive_sha1 != build.sha1 && bytes.find(build.label) != std::string_view::npos)
        {
            return build.game;
        }
    }

    return nullptr;
}

void SetUpStart(const SequenceStart& start, ntdlib::sequence_export::PerformOptions& perform,
                ntdlib::sequence_export::Settings& settings)
{
    perform.tracks = static_cast<uint16_t>(perform.tracks & ~(start.muted | start.silenced));
    perform.track_volumes = start.track_volumes;
    for (const SequenceVariable& variable : start.variables)
    {
        if (variable.index == SequenceVariable::kNone)
        {
            continue;
        }

        if (variable.frame == 0)
        {
            perform.variables[variable.index] = variable.value;
        }
        else
        {
            perform.changes.push_back({.frame = variable.frame, .index = variable.index, .value = variable.value});
        }
    }

    if (start.seed)
    {
        perform.seed = *start.seed;
        settings.random_steps_before_start = 0;
    }

    perform.tempo_ratio = start.tempo_ratio;
    perform.pitch = start.pitch;
    perform.volume = start.volume;
    perform.banks = start.banks;
    perform.fx_sends[0] = start.fx_send[0];
    perform.fx_sends[1] = start.fx_send[1];
}

void SetUpArrangement(const SequenceArrangement& arrangement, ntdlib::sequence_export::PerformOptions& perform)
{
    if (arrangement.start.seed)
    {
        throw std::logic_error("an arrangement can't give the random generator's state");
    }

    // SetUpStart changes the settings only for a start that gives the generator's state.
    ntdlib::sequence_export::Settings settings;
    SetUpStart(arrangement.start, perform, settings);
}

std::vector<SequenceArrangement> ArrangementsOf(std::string_view archive_sha1, std::string_view sound,
                                                uint32_t first_bank)
{
    // A state of the banks takes the rest of the sound's start.
    const SequenceStart start = ntdlib::ctr::snd::StartingSequenceTracks(archive_sha1, sound);
    std::vector<SequenceArrangement> arrangements;
    for (const SequenceArrangement& arrangement : ntdlib::ctr::snd::SequenceArrangements(archive_sha1, sound))
    {
        if (!DiffersOnlyInEffects(arrangement.start, start))
        {
            arrangements.push_back(arrangement);
        }
    }

    for (const SequenceArrangement& banks : ntdlib::ctr::snd::BankArrangements(archive_sha1, first_bank))
    {
        SequenceArrangement arrangement = {.suffix = banks.suffix, .start = start};
        arrangement.start.banks = banks.start.banks;
        arrangements.push_back(arrangement);
    }

    return arrangements;
}

} // namespace citrusf2
