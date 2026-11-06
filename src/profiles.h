// SPDX-License-Identifier: MIT

// The profiles of games whose code sets up some sounds as it plays them. citrusf2 picks a profile by the SHA-1 of the
// sound archive. A profile then sets up those sounds as the game does. Any other archive plays every sound as its
// sequence alone gives it.

#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>

#include "ntdlib/sequence_export/performance.h"

namespace citrusf2
{

// The games with a profile.
enum class Profile
{
    kNone,
    kMarioPartyIslandTour, // Mario Party: Island Tour (USA)
    kOcarinaOfTime3d,      // The Legend of Zelda: Ocarina of Time 3D (USA)
    kWarioWareGold,        // WarioWare Gold
    kNewLeaf,              // Animal Crossing: New Leaf (USA)
    kTomodachiLife,        // Tomodachi Life (USA)
    kUltimateAngler,       // StreetPass Mii Plaza: Ultimate Angler (USA)
    kFlowerTown,           // StreetPass Mii Plaza: Flower Town (USA)
    kMiiPlaza,             // StreetPass Mii Plaza, update v14.6.0
    kNinjaLauncher,        // StreetPass Mii Plaza: Ninja Launcher (USA)
    kMiiForce,             // StreetPass Mii Plaza: Mii Force (USA)
    // StreetPass Mii Plaza's European system title, without the update. The profile covers only the seven pieces that
    // follow a new Mii's jingle.
    kMiiPlazaSystemTitle,
    kSystemSettings,     // the European System Settings, and a second System Settings title (0004001000022F00)
    kActivityLog,        // the European Activity Log
    kMiiMaker,           // the European Mii Maker
    kSuperMario3dLand,   // Super Mario 3D Land (USA)
    kMarioKart7,         // Mario Kart 7 (USA, revision 1)
    kHappyHomeDesigner,  // Animal Crossing: Happy Home Designer (USA, the first release and revision 1)
    kMarioPartyStarRush, // Mario Party: Star Rush (USA)
};

// The profile of the sound archive in `archive`.
Profile ProfileOf(std::span<const uint8_t> archive);

// The game of a profile whose sound archive `archive` is another build of, or nullptr. Such an archive holds a sound
// that only the profile's archive holds, but its SHA-1 differs, and it gets no profile. Only StreetPass Mii Plaza,
// Ultimate Angler and Flower Town are checked. The archive of the Plaza's system title gets a note too. Its profile
// covers only some sounds.
const char* OtherBuildOf(std::span<const uint8_t> archive);

// Sets up the performance of the sound named `sound` as the game of `profile` plays it: the tracks that sound, the
// variables that the game sets as the sound starts or plays, the random generator's state for an effect that the
// game plays only on some draws, and the volume that the game sets through the sound's handle.
void SetUpSound(Profile profile, std::string_view sound, ntdlib::sequence_export::PerformOptions& perform);

// Another state that a game's code gives a sound as the game goes on: another set of tracks, another value of a
// variable, another tempo and pitch, other banks, or other volumes. citrusf2 converts the sound once more for each
// state, after the sound itself. The file's name is the sound's name followed by `suffix`.
struct Arrangement
{
    std::string_view suffix;
    uint16_t tracks = 0xffff; // ntdlib::sequence_export::PerformOptions::tracks
    int variable = -1;        // a variable that the state sets, or -1 for none
    int16_t value = -1;       // the variable's value

    // The sound frame at whose start the variable takes the value. At 0 the state sets it before the sequence starts.
    // A later frame comes after the sequence's first tick. That tick can set the variable too.
    uint32_t frame = 0;

    float tempo_ratio = 1.0f;                                    // ntdlib::sequence_export::PerformOptions::tempo_ratio
    float pitch = 1.0f;                                          // ntdlib::sequence_export::PerformOptions::pitch
    std::optional<std::array<uint32_t, 4>> banks = std::nullopt; // ntdlib::sequence_export::PerformOptions::banks
    float volume = 1.0f;                                         // ntdlib::sequence_export::PerformOptions::volume

    // ntdlib::sequence_export::PerformOptions::track_volumes
    std::array<float, ntdlib::sequence_export::kSequenceTracks> track_volumes = {
        1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f};
};

// The other states of the sound named `sound` in the game of `profile`, in the order that the game plays them.
// SetUpSound gives the state that the sound starts with. `first_bank` is the item ID in the sound's bank slot 0, or
// 0xFFFFFFFF for none. Some games give a sound other banks by that bank.
std::span<const Arrangement> ArrangementsOf(Profile profile, std::string_view sound, uint32_t first_bank);

} // namespace citrusf2
