// SPDX-License-Identifier: MIT

// The profiles of games whose code sets up some sounds as it plays them. ntdlib's list of the 3DS games' starts
// (ntdlib/ctr/snd/starting_tracks.h) finds a profile by the SHA-1 of the sound archive. A profile then sets up those
// sounds as the game does. Any other archive plays every sound as its sequence alone gives it.

#pragma once

#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

#include "ntdlib/ctr/snd/starting_tracks.h"
#include "ntdlib/sequence_export/performance.h"
#include "ntdlib/sequence_export/settings.h"

namespace citrusf2
{

// The game of a profile whose sound archive `archive` is another build of, or nullptr. Such an archive holds a sound
// that only the profile's archive holds, but its SHA-1, `archive_sha1`, differs, and it gets no profile. Only
// StreetPass Mii Plaza, Ultimate Angler and Flower Town are checked. The archive of the Plaza's system title gets a
// note too. Its profile covers only some sounds.
const char* OtherBuildOf(std::string_view archive_sha1, std::span<const uint8_t> archive);

// Sets up `perform` and `settings` as a game starts a sequence with `start`: the tracks that sound and their volumes,
// the variables that the game sets as the sound starts or plays, the random generator's state, the tempo ratio, the
// pitch, the volume, the banks and the sends on buses A and B. A start that gives the generator's state at the
// sequence's first tick takes no steps of the generator before the sound starts (Settings::random_steps_before_start).
// The conversion has no model of the game's effect on the buses (SequenceStart::game_effect).
void SetUpStart(const ntdlib::ctr::snd::SequenceStart& start, ntdlib::sequence_export::PerformOptions& perform,
                ntdlib::sequence_export::Settings& settings);

// Sets up `perform` as SetUpStart does for the start of `arrangement`. Throws std::logic_error if the start gives the
// random generator's state. An arrangement can't change Settings::random_steps_before_start, and no arrangement in
// ntdlib's lists gives one.
void SetUpArrangement(const ntdlib::ctr::snd::SequenceArrangement& arrangement,
                      ntdlib::sequence_export::PerformOptions& perform);

// The other states that a game's code gives the sound named `sound` as the game goes on, in the order that the game
// plays them, for the sound archive whose SHA-1 is `archive_sha1`. citrusf2 converts the sound once more for each
// state, after the sound itself. The file's name is the sound's name followed by the state's suffix. `first_bank` is
// the item ID in the sound's bank slot 0, or 0xFFFFFFFF for none. Some games give a sound other banks by that bank. A
// state that differs from the sound's start only in the sends and the effect on the buses stays out. The conversion
// has no model of the game's effects, and it writes the sends only with --reverb.
std::vector<ntdlib::ctr::snd::SequenceArrangement> ArrangementsOf(std::string_view archive_sha1, std::string_view sound,
                                                                  uint32_t first_bank);

} // namespace citrusf2
