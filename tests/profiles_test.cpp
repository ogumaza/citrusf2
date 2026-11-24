// SPDX-License-Identifier: MIT

// Tests the games' reverbs on aux bus A (citrusf2::ReverbOnBusA): which sounds write their send A as CC91. Tests the
// start of a state with a low-pass too (citrusf2::SetUpArrangement).

#include "profiles.h"

#include <algorithm>
#include <cstdint>
#include <optional>
#include <vector>

#include "ntdlib/ctr/snd/starting_tracks.h"
#include "ntdlib/sequence_export/consoles.h"
#include "ntdlib/sequence_export/filter.h"
#include "ntdlib/sequence_export/performance.h"
#include "ntdlib/sequence_export/settings.h"
#include "xsflib/testing/check.h"

namespace
{

using citrusf2::ReverbOnBusA;
using ntdlib::ctr::snd::SequenceArrangement;

constexpr uint8_t kArchiveEffects = ntdlib::ctr::snd::SequenceStart::kArchiveEffects;

// The SHA-1s of four sound archives: Flower Town's, Ocarina of Time 3D's, New Leaf's and Island Tour's.
constexpr const char* kFlowerTown = "331b572b09819830436fe8a09673b22fc6f7bdae";
constexpr const char* kOcarinaOfTime3D = "c3fdb98e65e6143d76e6d971cfaaac058455300d";
constexpr const char* kNewLeaf = "54f0fbc801c85ea2035219923aa4cd955e6f4cc7";
constexpr const char* kIslandTour = "e9d5d40aa7b034360905ecb171a5825b37688c6c";

// The SHA-1 of the Plaza's sound archive in the update v14.6.0.
constexpr const char* kPlaza = "33b514ee80f6a6df3034bc8774235e40dfa52d1c";

// A game with a reverb on bus A for the whole archive writes every sound's send.
void TestArchiveReverb()
{
    XSFLIB_CHECK(ReverbOnBusA(kOcarinaOfTime3D, "NA_BGM_FIELD", kArchiveEffects) == std::optional<bool>(true));
    XSFLIB_CHECK(ReverbOnBusA(kFlowerTown, "BGM_FLOWER_TOWN", kArchiveEffects) == std::optional<bool>(true));
}

// Only the Plaza's sound test plays Flower Town's SOUNDLIST_ sounds. It plays them dry.
void TestSoundTest()
{
    XSFLIB_CHECK(ReverbOnBusA(kFlowerTown, "SOUNDLIST_TITLE", kArchiveEffects) == std::optional<bool>(false));
    XSFLIB_CHECK(ReverbOnBusA(kFlowerTown, "SOUNDLIST_FLOWER_MEISTER", kArchiveEffects) == std::optional<bool>(false));
}

// New Leaf has no reverb for the archive. Its club's effect 0 is a reverb.
void TestGameEffects()
{
    XSFLIB_CHECK(ReverbOnBusA(kNewLeaf, "SEQ_BGM_IND_MUSEUM", kArchiveEffects) == std::optional<bool>(false));
    XSFLIB_CHECK(ReverbOnBusA(kNewLeaf, "SEQ_BGM_DJ_HS_HANIWA", 0) == std::optional<bool>(true));
    XSFLIB_CHECK(ReverbOnBusA(kNewLeaf, "SEQ_BGM_DJ_HS_HANIWA", 1) == std::optional<bool>(false));
}

// A game that isn't known to put a reverb on bus A gets no answer. --reverb decides for it.
void TestUnknownGames()
{
    XSFLIB_CHECK(!ReverbOnBusA(kIslandTour, "SEQ_BGM_MENU", kArchiveEffects));
    XSFLIB_CHECK(!ReverbOnBusA("", "SOUNDLIST_TITLE", kArchiveEffects));
}

// The archives of ntdlib's table of the games' effects. Each game with a reverb on bus A answers for the archive's
// effects: 16 with the reverb, and New Leaf and Happy Home Designer's two releases without it. The games without a
// reverb on bus A get no answer. ntdlib's table cites the addresses in each game's code.
void TestTableArchives()
{
    const char* const kReverb[] = {
        "1dd919adf21565f9d9399907f49d610068685cda", // Pokémon X
        "6b471abb246fcaa2cf13ffbf16345e5d0c02e7e1", // WarioWare Gold
        kOcarinaOfTime3D,
        kPlaza,
        "c2550ce04d4c8c44e92c2d72682038616fbec658", // the Plaza's system title
        "a98dd3c22da8e4bed8327e4fc98196e102ce53ea", // Ultimate Angler
        kFlowerTown,
        "132e22e118ed9cb162fa2117d636d433920328a4", // Monster Manor
        "c6a2a213c9780f9cbcd886d507ba72beb858b4a1", // Feed Mii
        "ddb93158672258c7d8ea2b5edb8662bda34234e6", // Ninja Launcher
        "ff5231c919ad81331321e6e29e24d84bbf2127cc", // System Settings
        "fd187b38953c162695779f26c66140859a63e74b", // the second System Settings
        "0cc99a6732a7dbc0d6e8887449233ab25396672c", // Mii Maker
        "ea55099f2b92479898b4eda7956d645021dac2d3", // the Activity Log
        "32528650ee18385fdab7701dce1aae01bbd71e79", // Nintendo eShop
        "cad032a7a698c87af8e73aa78e6865afe77a67fb", // Swapnote
    };
    const char* const kDry[] = {
        kNewLeaf,
        "8c07814daf28befb015cc97dd2c66699254035be", // Happy Home Designer, revision 1
        "30c98d66881296eda5e676964918e7003b403c75", // Happy Home Designer, the other USA release
    };
    const char* const kNoReverb[] = {
        "7967baa712b4badc354a88e550847e1713195c74", // Warrior's Way
        "e566c28135170a936f1f6e4538a15d745f3b2a9e", // Mii Force
        "1a7cdf18ce5dace373d4eac78733083987c9a964", // Slot Car Rivals
        "e640022555aa9fce1c4516a7398f960b56614477", // Market Crashers
        "1a3bd13bd513803e50475dfd3b2ca427d1b68bc0", // Battleground Z
        "d14849826e1df1dd543938dc9313afdbb9d82147", // Mii Trek
        "03cdddad0cb59b6f90dd7cc6727873701f4300b9", // Luigi's Mansion
        "bfb2742fc5e8f2869c9ce781710ba393f675fbc1", // Super Mario 3D Land
        "35b72405134e662e160c950c62509cb8a84b3e47", // Majora's Mask 3D
        kIslandTour,
        "90fa0978d0f4e3af259a48b48141d3e58ab5e66e", // Star Rush
        "5dae357850ed6f88cecd09ac6d6c402ace27566a", // Star Rush's second archive
    };

    for (const char* sha1 : kReverb)
    {
        XSFLIB_CHECK(ReverbOnBusA(sha1, "", kArchiveEffects) == std::optional<bool>(true));
    }
    for (const char* sha1 : kDry)
    {
        XSFLIB_CHECK(ReverbOnBusA(sha1, "", kArchiveEffects) == std::optional<bool>(false));
    }
    for (const char* sha1 : kNoReverb)
    {
        XSFLIB_CHECK(!ReverbOnBusA(sha1, "", kArchiveEffects));
    }
}

// Behind a wall of fire or poison, Find Mii II starts BGM_DEN2_Battle01 at a volume of 0.45 and a low-pass of -0.4. The
// state's start copies both. The tracks of the piece have no low-pass of their own. Each track then takes step 13 of
// the low-pass, 1600 Hz, and the SoundFont's fitted cutoff for it, with no resonance.
void TestWallLowPass()
{
    const std::vector<SequenceArrangement> arrangements =
        citrusf2::ArrangementsOf(kPlaza, "BGM_DEN2_Battle01", ntdlib::ctr::snd::kNoBank);
    const auto wall = std::ranges::find(arrangements, "_wall", &SequenceArrangement::suffix);
    XSFLIB_CHECK(wall != arrangements.end());
    if (wall == arrangements.end())
    {
        return;
    }

    ntdlib::sequence_export::PerformOptions perform;
    citrusf2::SetUpArrangement(*wall, perform);
    const ntdlib::sequence_export::Settings settings = ntdlib::sequence_export::CtrSettings();
    const int argument = ntdlib::sequence_export::LowPassArgument(0.0f, perform.lpf, settings);
    const ntdlib::sequence_export::SoundFontFilter filter =
        ToSoundFont(TrackFilters(argument, 0, 0, settings), settings);

    XSFLIB_CHECK(perform.volume == 0.45f && perform.lpf == -0.4f);
    XSFLIB_CHECK(filter.low_pass_hz == 1600);
    XSFLIB_CHECK(filter.cutoff == settings.filters->low_pass_cutoff[13] && filter.resonance == 0);
}

} // namespace

int main()
{
    const auto tests = []
    {
        TestArchiveReverb();
        TestSoundTest();
        TestGameEffects();
        TestUnknownGames();
        TestTableArchives();
        TestWallLowPass();
    };

    return xsflib::testing::RunTest("profiles", tests);
}
