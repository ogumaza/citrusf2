// SPDX-License-Identifier: MIT

// The profiles of games (see profiles.h).

#include "profiles.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>

#include "ntdlib/sequence_export/performance.h"
#include "xsflib/sha1.h"

namespace citrusf2
{

namespace
{

using ntdlib::sequence_export::PerformOptions;
using ntdlib::sequence_export::VariableChange;

// The sound archives of Mario Party: Island Tour (USA), The Legend of Zelda: Ocarina of Time 3D (USA) and WarioWare
// Gold.
constexpr std::string_view kMarioPartyIslandTourSha1 = "e9d5d40aa7b034360905ecb171a5825b37688c6c";
constexpr std::string_view kOcarinaOfTime3dSha1 = "c3fdb98e65e6143d76e6d971cfaaac058455300d";
constexpr std::string_view kWarioWareGoldSha1 = "6b471abb246fcaa2cf13ffbf16345e5d0c02e7e1";

// The sound archives of Animal Crossing: New Leaf (USA) and of Tomodachi Life (USA).
constexpr std::string_view kNewLeafSha1 = "54f0fbc801c85ea2035219923aa4cd955e6f4cc7";
constexpr std::string_view kTomodachiLifeSha1 = "200b87d7d1152f5035b29a092f2e20e726695264";

// The sound archives of Animal Crossing: Happy Home Designer (USA), of the first release and of revision 1. They hold
// the same sequences for the tune player and the TV as New Leaf.
constexpr std::string_view kHappyHomeDesignerOldSha1 = "30c98d66881296eda5e676964918e7003b403c75";
constexpr std::string_view kHappyHomeDesignerSha1 = "8c07814daf28befb015cc97dd2c66699254035be";

// The sound archives of Ultimate Angler and Flower Town (USA), two add-on games of StreetPass Mii Plaza. Their code
// is in the Plaza's program (version 14.6.0).
constexpr std::string_view kUltimateAnglerSha1 = "a98dd3c22da8e4bed8327e4fc98196e102ce53ea";
constexpr std::string_view kFlowerTownSha1 = "331b572b09819830436fe8a09673b22fc6f7bdae";

// The sound archive of Mii Force (USA), another add-on game of the Plaza.
constexpr std::string_view kMiiForceSha1 = "e566c28135170a936f1f6e4538a15d745f3b2a9e";

// The sound archive of StreetPass Mii Plaza's update v14.6.0. Older builds of the Plaza hold another archive.
constexpr std::string_view kMiiPlazaSha1 = "33b514ee80f6a6df3034bc8774235e40dfa52d1c";

// The sound archive of StreetPass Mii Plaza's system title. The system title plays without the update. The European
// system title of system update 11.17.0-50E holds the archive. The archive's seven From_New_Mii sequences match those
// of v14.6.0, command for command. The system title's code plays them by ID (0x2d5180).
constexpr std::string_view kMiiPlazaSystemTitleSha1 = "c2550ce04d4c8c44e92c2d72682038616fbec658";

// The sound archives of four European system titles of the 3DS: System Settings (0004001000022000), a second System
// Settings title (0004001000022F00), the Activity Log (0004001000022200) and Mii Maker (0004001000022700).
constexpr std::string_view kSystemSettingsSha1 = "ff5231c919ad81331321e6e29e24d84bbf2127cc";
constexpr std::string_view kSystemSettings2Sha1 = "fd187b38953c162695779f26c66140859a63e74b";
constexpr std::string_view kActivityLogSha1 = "ea55099f2b92479898b4eda7956d645021dac2d3";
constexpr std::string_view kMiiMakerSha1 = "0cc99a6732a7dbc0d6e8887449233ab25396672c";

// The sound archive of Mario Party: Star Rush (USA) with minigame 708's music.
constexpr std::string_view kMarioPartyStarRushSha1 = "90fa0978d0f4e3af259a48b48141d3e58ab5e66e";

// The sound archive of Ninja Launcher (USA), an add-on game of StreetPass Mii Plaza. Its code is in the Plaza's
// program (version 14.6.0).
constexpr std::string_view kNinjaLauncherSha1 = "ddb93158672258c7d8ea2b5edb8662bda34234e6";

// The sound archive of Super Mario 3D Land (USA, revision 1).
constexpr std::string_view kSuperMario3dLandSha1 = "bfb2742fc5e8f2869c9ce781710ba393f675fbc1";

// The sound archive of Mario Kart 7 (USA, revision 1).
constexpr std::string_view kMarioKart7Sha1 = "cfd8fb5750069922dc631708d883fcc7374f8fa5";

// Island Tour's menu music. The piece plays in four layers: tracks 0 to 3, 0 to 6, 0 to 9, or all 16. The table of
// layers is rom:/ts_sound_ctrl_track.bin (loaded at code.bin 0x115bfc), rows 25 to 28. The menu's flow starts the
// piece with one layer's tracks (0x158d60, through 0x39bb04) and mutes the rest (0x18dd80). Each menu screen then
// fades to its layer over a second (0x272d00, 0x2ffccc). The main menu plays tracks 0 to 3 (0x16336c). Game select
// and party select play 0 to 6, character select 0 to 9, and the rule and minigame screens all 16. The profile plays
// the piece as the main menu does, where the piece starts. Each other layer is an arrangement, in the order of the
// table's rows (0x3d5274, read at 0x272d00).
constexpr std::string_view kIslandTourMenu = "SEQ_BGM_MENU";
constexpr uint16_t kIslandTourMenuTracks = 0x000f;
constexpr std::array<Arrangement, 3> kIslandTourMenuLayers = {{
    {.suffix = "_game_select", .tracks = 0x007f},
    {.suffix = "_character_select", .tracks = 0x03ff},
    {.suffix = "_minigame_select", .tracks = 0xffff},
}};

// Seven of Island Tour's minigame and boss pieces set their tempo from player variable 0. A track moves a copy of the
// variable toward it every 2 ticks, clamps the copy and scales it (TEMPO_CONTROL_START). The game writes the variable
// as the piece plays. Each minigame's code is in a separate module (rom:/dll/mod_mg*.cro.p), and the addresses below
// are offsets in its file. Azahar runs that patched the game into each minigame give the values.
// mg3801 starts SEQ_BGM_MNG_MG3801 (0xf4c0) with row 10 of rom:/ts_sound_ctrl_track.bin (0xf4d4) and writes the
// player's time left out of 100 seconds (0xbe18). The piece plays at 145 BPM down to 50 seconds, 165 at 20 and 215 at
// 0. At 20 seconds the game moves to row 11, and back to row 10 at 40 (0xbe9c and 0xbe60).
// mg3802 starts SEQ_BGM_MNG_MG3802 (0x9c40) and writes the time left, 99 as the piece starts (0x614c). The piece plays
// at 123 BPM down to 30 seconds, 160 at 20 and 230 at 0.
// mg3704 starts SEQ_BGM_BOSS_04_A (0x13dc) and writes 20 at once (0x13ec). Each round then writes its timer (0xfd4 and
// 0x2f5c). The timer starts at 20 seconds. The piece plays at 122 BPM at 20 seconds and 170 at 0. A round that starts
// with the value at 0xea4 at 50 or less starts _B, _B2 or _B3 (0xf00, 0xf30 and 0xf48). Those play at 148 BPM at 20
// seconds and 170 at 0. Runs patched that value to 50 and the choice at 0xed0 to 0, 1 and 2. They started each piece,
// and each wrote 20.
// mg3419 starts SEQ_BGM_MNG_012 (0x1c44) with row 7 (0x1c54) and writes the ball's speed times 96 (0x1e6c, 0xe4d0). The
// piece plays at 60 BPM with the ball at rest. With the tilt patched to its maximum, the speed stays at 1 and the
// variable at 96, or 165 BPM. Rows 8 and 9 come with the byte at 0x1dec at 2 and 3 (0x1e10 and 0x1e2c). A run patched
// that byte reached both.
// The profile starts each piece at the value of its first seconds, or of full speed. The arrangements give the rest.
// The game's value falls to 0 as the time runs out, and a conversion holds one value: "_hurry" has 10 seconds left.
// SEQ_BGM_MNG_MG3801_HANDE stays as it is. The game starts it at a volume of 0 (0x5930) and writes no variable on it.
constexpr int kIslandTourTempoVariable = 0;
constexpr int16_t kTenSecondsLeft = 10;
constexpr int16_t kFullSpeed = 96;
constexpr std::array<Arrangement, 1> kTenSecondsLeftState = {{
    {.suffix = "_hurry", .variable = kIslandTourTempoVariable, .value = kTenSecondsLeft},
}};
constexpr std::array<Arrangement, 1> kMg3801TenSecondsLeft = {{
    {.suffix = "_hurry", .tracks = 0x9ab5, .variable = kIslandTourTempoVariable, .value = kTenSecondsLeft},
}};
constexpr std::array<Arrangement, 2> kMng012Layers = {{
    {.suffix = "_second", .tracks = 0x41f0, .variable = kIslandTourTempoVariable, .value = kFullSpeed},
    {.suffix = "_third", .tracks = 0xfe00, .variable = kIslandTourTempoVariable, .value = kFullSpeed},
}};

struct TempoPiece
{
    std::string_view sound;
    uint16_t tracks;
    int16_t value;
    std::span<const Arrangement> arrangements;
};

constexpr std::array<TempoPiece, 7> kIslandTourTempoPieces = {{
    {"SEQ_BGM_MNG_012", 0x400f, kFullSpeed, kMng012Layers},
    {"SEQ_BGM_MNG_MG3801", 0x674b, 100, kMg3801TenSecondsLeft},
    {"SEQ_BGM_MNG_MG3802", 0xffff, 99, kTenSecondsLeftState},
    {"SEQ_BGM_BOSS_04_A", 0xffff, 20, kTenSecondsLeftState},
    {"SEQ_BGM_BOSS_04_B", 0xffff, 20, kTenSecondsLeftState},
    {"SEQ_BGM_BOSS_04_B2", 0xffff, 20, kTenSecondsLeftState},
    {"SEQ_BGM_BOSS_04_B3", 0xffff, 20, kTenSecondsLeftState},
}};

// Lon Lon Ranch's music. Malon sings its melody on tracks 0 and 1. Track 13 plays the same melody on an instrument.
// While the music plays, the code of Malon's actors (code.bin 0x1d9d54, 0x1da004 and 0x1da290) calls 0x33d364 with her
// distance from Link. That function gives tracks 0 and 1 a volume that falls from 1 at 200 units to 0 at 2,000 units.
// Track 13 gets 1 minus that volume (0x33d408 and 0x33d418, through the track volume setter at 0x3560d8). Near Malon,
// she sings and track 13 is silent. Far from her, track 13 plays the melody alone. The game raises track 13 only as her
// voice fades. The profile therefore plays the piece as it sounds near Malon.
constexpr std::string_view kLonLonRanch = "NA_BGM_RONRON";
constexpr uint16_t kLonLonRanchTracks = 0xdfff;

// The jingles between the microgames of WarioWare Gold's Tricky challenge, where Lulu plays tricks on the microgames.
// Each holds two arrangements: tracks 0 to 11, and tracks 14 and 15. Tracks 12 and 13 belong to both. The challenge
// listens to the game's sound starts (vtable 0x83b724, hook 0x361d58) and acts on these three only. Through 0x361cc4,
// a flag silences one arrangement with SetTrackSilence and lets the other play, with fades of 10 frames. Flag 0 plays
// tracks 12 to 15. Flag 1 plays tracks 0 to 13. The flag starts at 0 (0x36ac68). The request of the next trick sets it
// to 1 (0x362664). The end of Lulu's cut-in that shows the trick sets it to 0 (0x36a9c0). It stays 0 while the trick's
// microgames play, until the next trick's request. The profile plays the jingles as they sound then. The game fades
// tracks 0 to 11 out over a jingle's first 10 frames. The profile silences them from the start.
constexpr std::array<std::string_view, 3> kTrickyJingles = {"SEQ_M_TSUNAGI_TRICKY_NEXT1", "SEQ_M_TSUNAGI_TRICKY_NG1",
                                                            "SEQ_M_TSUNAGI_TRICKY_OK1"};
constexpr uint16_t kTrickyJingleTracks = 0xf000;

// New Leaf's museum. The game plays STRM_BGM_IND_MUSEUM and SEQ_BGM_IND_MUSEUM together. The sequence's 13 tracks are
// a base of tracks 0 to 3 and three layers: tracks 4 to 6, 7 to 9 and 10 to 12. Each hall of the museum has a state,
// and a table gives each state the sequence's tracks that play (code.bin 0x8ba11c): tracks 0 to 3 and 10 to 12 (state
// 2), none (3 and 4), tracks 0 to 3 and 7 to 9 (5), tracks 0 to 6 (6). Moving between halls passes through state 1,
// tracks 0 to 3. The music's start (0x1e1330) gives scenes 0x49 and 0x55, and every scene its table doesn't list,
// state 2. It mutes the other tracks before they sound (0x1e2590, with no fade). No state plays all 13 tracks. The
// profile plays the piece in state 2.
constexpr std::string_view kMuseum = "SEQ_BGM_IND_MUSEUM";
constexpr uint16_t kMuseumTracks = 0x1c0f;

// New Leaf's café job piece. The BGM manager gives SEQ_BGM_IND_CAFEARBEIT mode 10 (0x1e1810). The piece's start in
// that mode sets track 7's volume to 0 and mutes the track (0x1e151c to 0x1e1560). At an event of the job, the café
// unmutes track 7 and fades it in over 40 frames (0x1e2e50, from 0x420404). The profile plays the start. The state
// after the fade is an arrangement.
constexpr std::string_view kCafe = "SEQ_BGM_IND_CAFEARBEIT";
constexpr uint16_t kCafeTracks = 0xff7f;
constexpr std::array<Arrangement, 1> kCafeTrack7 = {{
    {.suffix = "_track_7"},
}};

// New Leaf's DJ K.K. plays each song in one of two styles, SEQ_BGM_DJnn_HS_* and SEQ_BGM_DJnn_FC_*. Between songs he
// plays an interlude. Each song waits at its start until player variable 0 is 0 or 1. At 0 it plays an intro first.
// At -1 it plays nothing. Once the song's sound is prepared, DJ K.K.'s player (0x28b8e8) writes 1 to it, or 0 for a
// number with bit 7 set (0x28babc to 0x28baf4). The playlist gives the birthday song and the event song such numbers
// (0x28ae04 to 0x28ae1c). The interludes wait for 0 the same way. The player writes 0 to them (0x28b9b8 to 0x28b9c8).
constexpr std::string_view kDjPrefix = "SEQ_BGM_DJ";
constexpr std::array<std::string_view, 8> kDjAtZero = {
    "SEQ_BGM_DJ91_HS_BIRTHDAYSONG", "SEQ_BGM_DJ91_FC_BIRTHDAYSONG", "SEQ_BGM_DJ95_HS_EVENT",  "SEQ_BGM_DJ95_FC_EVENT",
    "SEQ_BGM_DJ_HS_HANIWA",         "SEQ_BGM_DJ_FC_HANIWA",         "SEQ_BGM_DJ_HS_MURAMELO", "SEQ_BGM_DJ_FC_MURAMELO"};
constexpr uint8_t kDjStart = 0;

// SEQ_BGM_DJ_HS_MURAMELO and SEQ_BGM_DJ_FC_MURAMELO play the town's tune four times on track 15. Before each step, the
// track copies the next of its variables 0 to 15 into player variable 4. A value of 10000 mutes the step. A value below
// -1000 picks a random note. Any other value transposes note 60. No other track reads a track variable. DJ K.K.'s
// player (0x28b08c) writes the 16 values to track 15 (SetTrackVariable, 0x52eacc) from the tune that the save holds. A
// note gets its transpose in the major scale (0x411284) plus the key of the song in the player's slot (the table at
// 0x8bb51c). A rest or a hold gets 10000. A random note gets the key less 10000. A new town takes one of four default
// tunes at random (0x7b7f40, from the table at 0x8bbce2). The profile gives the first of them in key 0, the commonest
// key of DJ K.K.'s 94 songs (24 of them). A Unicorn run of 0x28b08c gives the values. At -1, every step plays B.
constexpr std::array<std::string_view, 2> kDjTuneInterludes = {"SEQ_BGM_DJ_HS_MURAMELO", "SEQ_BGM_DJ_FC_MURAMELO"};
constexpr uint8_t kDjTuneTrack = 15;
constexpr std::array<int16_t, 16> kFirstDefaultTune = {12, 10000, 7, 4,      5, 5,     5,     10000,
                                                       7,  7,     5, -10000, 0, 10000, 10000, 10000};

// The game plays DJ K.K.'s sequences only in the club CLUB444_DJKK (scene 0x46). There the scene change (0x1b3070)
// starts the effects manager's effect 0, nn::snd's FxReverb, on bus A. The level setter (0x1b2e28) gives the sound
// player of his music (PLAYER_BGM) a send of 0.4 on bus A, from the table at 0x8bb488. BasicSound's update adds the
// player's send to the sound's (0x52b7f8 to 0x52b818). Each track adds the result to the track's send on the bus
// (0x533f84 to 0x533fb0). With --reverb, CC91 takes the sum.
constexpr float kClubSend = 0.4f;

// The tune player of New Leaf (0x411330) and Happy Home Designer plays a tune of 16 steps one note at a time. Each
// note starts the player's SE_MELODY_ sound again (New Leaf 0x411534, Happy Home Designer 0x2364a8). The player writes
// the note to variable 14 and the note's transpose to variable 12. The transpose comes from a table of ten scales
// (New Leaf 0x8bbd4c, Happy Home Designer 0x639f24), plus an offset. Each sound gives its scale and its offset. At -1
// a sound plays no note. The profile gives each sound the tune's note 3, the C, and the transpose of its scale and
// offset. SE_MELODY_TRAIN_ANNOUNCE plays the train's fixed tune (0x8bbd22, loaded at 0x217bb8). The profile gives
// it the tune's first note, 9. The game stops each note after a length that the sound gives (0x410cf4). citrusf2
// holds it for the length that the sequence gives it.
constexpr std::array<std::string_view, 3> kTuneSoundFamilies = {"SE_MELODY_NPCNRM_", "SE_MELODY_NPCSP_",
                                                                "SE_MELODY_DOOR_"};
constexpr std::array<std::string_view, 5> kTuneSounds = {"SE_MELODY_BOARD_EDIT", "SE_MELODY_LETTER",
                                                         "SE_MELODY_PLAYER_GHOST", "SE_MELODY_HUMMING",
                                                         "SE_MELODY_TRAIN_ANNOUNCE"};
constexpr std::string_view kTrainAnnouncement = "SE_MELODY_TRAIN_ANNOUNCE";
constexpr uint8_t kTuneNote = 14;
constexpr uint8_t kTuneTranspose = 12;
constexpr int16_t kNoteC = 3;
constexpr int16_t kTrainTuneFirstNote = 9;

// A sound of the tune player whose note doesn't play at the sequence's note 60: the transpose that the sound's scale
// and offset give the note.
struct TuneTranspose
{
    std::string_view sound;
    int16_t transpose;
};

constexpr std::array<TuneTranspose, 186> kTuneTransposes = {{
    {"SE_MELODY_HUMMING", 12},       {"SE_MELODY_TRAIN_ANNOUNCE", 11}, {"SE_MELODY_NPCNRM_BEA05", -1},
    {"SE_MELODY_NPCNRM_BEA06", -7},  {"SE_MELODY_NPCNRM_BEA07", -3},   {"SE_MELODY_NPCNRM_BEA10", -3},
    {"SE_MELODY_NPCNRM_BEA12", -3},  {"SE_MELODY_NPCNRM_BRD00", -12},  {"SE_MELODY_NPCNRM_BRD01", -3},
    {"SE_MELODY_NPCNRM_BRD02", -12}, {"SE_MELODY_NPCNRM_BRD04", -12},  {"SE_MELODY_NPCNRM_BRD15", -17},
    {"SE_MELODY_NPCNRM_BRD16", -9},  {"SE_MELODY_NPCNRM_BRD17", -12},  {"SE_MELODY_NPCNRM_BRD18", -10},
    {"SE_MELODY_NPCNRM_BUL00", 3},   {"SE_MELODY_NPCNRM_BUL08", 2},    {"SE_MELODY_NPCNRM_CAT00", -2},
    {"SE_MELODY_NPCNRM_CAT01", -3},  {"SE_MELODY_NPCNRM_CAT05", -5},   {"SE_MELODY_NPCNRM_CAT06", -2},
    {"SE_MELODY_NPCNRM_CAT08", -2},  {"SE_MELODY_NPCNRM_CAT09", -5},   {"SE_MELODY_NPCNRM_CAT10", -5},
    {"SE_MELODY_NPCNRM_CAT11", -5},  {"SE_MELODY_NPCNRM_CAT12", -3},   {"SE_MELODY_NPCNRM_CAT13", -5},
    {"SE_MELODY_NPCNRM_CAT14", -4},  {"SE_MELODY_NPCNRM_CAT17", -3},   {"SE_MELODY_NPCNRM_CAT18", -4},
    {"SE_MELODY_NPCNRM_CAT20", 4},   {"SE_MELODY_NPCNRM_CAT21", -5},   {"SE_MELODY_NPCNRM_CBR00", 2},
    {"SE_MELODY_NPCNRM_CBR02", 1},   {"SE_MELODY_NPCNRM_CBR06", -2},   {"SE_MELODY_NPCNRM_CBR10", 1},
    {"SE_MELODY_NPCNRM_CBR16", -5},  {"SE_MELODY_NPCNRM_CHN12", -5},   {"SE_MELODY_NPCNRM_CRD01", 1},
    {"SE_MELODY_NPCNRM_CRD04", -4},  {"SE_MELODY_NPCNRM_CRD07", 7},    {"SE_MELODY_NPCNRM_CRD08", -7},
    {"SE_MELODY_NPCNRM_DER01", -5},  {"SE_MELODY_NPCNRM_DER02", -10},  {"SE_MELODY_NPCNRM_DER03", -7},
    {"SE_MELODY_NPCNRM_DER05", -7},  {"SE_MELODY_NPCNRM_DER07", -5},   {"SE_MELODY_NPCNRM_DER08", 3},
    {"SE_MELODY_NPCNRM_DER09", -7},  {"SE_MELODY_NPCNRM_DOG01", -5},   {"SE_MELODY_NPCNRM_DOG06", -2},
    {"SE_MELODY_NPCNRM_DOG07", -2},  {"SE_MELODY_NPCNRM_DOG08", -2},   {"SE_MELODY_NPCNRM_DOG15", -3},
    {"SE_MELODY_NPCNRM_DOG16", -7},  {"SE_MELODY_NPCNRM_DOG18", -2},   {"SE_MELODY_NPCNRM_DUK01", -3},
    {"SE_MELODY_NPCNRM_DUK08", -2},  {"SE_MELODY_NPCNRM_DUK15", 2},    {"SE_MELODY_NPCNRM_DUK16", 4},
    {"SE_MELODY_NPCNRM_ELP10", 2},   {"SE_MELODY_NPCNRM_FLG01", -12},  {"SE_MELODY_NPCNRM_FLG02", -12},
    {"SE_MELODY_NPCNRM_FLG03", -12}, {"SE_MELODY_NPCNRM_FLG05", -12},  {"SE_MELODY_NPCNRM_FLG06", -2},
    {"SE_MELODY_NPCNRM_FLG07", -12}, {"SE_MELODY_NPCNRM_FLG10", -12},  {"SE_MELODY_NPCNRM_FLG12", -12},
    {"SE_MELODY_NPCNRM_FLG13", -1},  {"SE_MELODY_NPCNRM_FLG16", 7},    {"SE_MELODY_NPCNRM_FLG17", -7},
    {"SE_MELODY_NPCNRM_GOA04", -7},  {"SE_MELODY_NPCNRM_GOA06", -2},   {"SE_MELODY_NPCNRM_GOA07", -12},
    {"SE_MELODY_NPCNRM_GOR00", -7},  {"SE_MELODY_NPCNRM_GOR01", -12},  {"SE_MELODY_NPCNRM_GOR02", -7},
    {"SE_MELODY_NPCNRM_GOR08", -10}, {"SE_MELODY_NPCNRM_GOR09", -3},   {"SE_MELODY_NPCNRM_GOR10", -7},
    {"SE_MELODY_NPCNRM_HAM01", 5},   {"SE_MELODY_NPCNRM_HAM03", -5},   {"SE_MELODY_NPCNRM_HAM04", 5},
    {"SE_MELODY_NPCNRM_HAM07", -2},  {"SE_MELODY_NPCNRM_HIP09", 5},    {"SE_MELODY_NPCNRM_HRS05", -3},
    {"SE_MELODY_NPCNRM_HRS11", -3},  {"SE_MELODY_NPCNRM_HRS12", -3},   {"SE_MELODY_NPCNRM_HRS13", -5},
    {"SE_MELODY_NPCNRM_KAL02", -2},  {"SE_MELODY_NPCNRM_KAL05", -8},   {"SE_MELODY_NPCNRM_KAL09", -3},
    {"SE_MELODY_NPCNRM_KAL10", -2},  {"SE_MELODY_NPCNRM_KGR00", -5},   {"SE_MELODY_NPCNRM_KGR01", -6},
    {"SE_MELODY_NPCNRM_KGR06", -6},  {"SE_MELODY_NPCNRM_KGR08", -7},   {"SE_MELODY_NPCNRM_KGR09", -7},
    {"SE_MELODY_NPCNRM_LON01", 5},   {"SE_MELODY_NPCNRM_LON06", 3},    {"SE_MELODY_NPCNRM_LON07", -3},
    {"SE_MELODY_NPCNRM_LON08", -5},  {"SE_MELODY_NPCNRM_MNK07", 3},    {"SE_MELODY_NPCNRM_MNK08", 5},
    {"SE_MELODY_NPCNRM_MUS01", -2},  {"SE_MELODY_NPCNRM_MUS03", 5},    {"SE_MELODY_NPCNRM_MUS09", -1},
    {"SE_MELODY_NPCNRM_MUS15", -1},  {"SE_MELODY_NPCNRM_MUS16", 7},    {"SE_MELODY_NPCNRM_MUS17", 7},
    {"SE_MELODY_NPCNRM_OST01", -1},  {"SE_MELODY_NPCNRM_OST07", -8},   {"SE_MELODY_NPCNRM_OST08", 3},
    {"SE_MELODY_NPCNRM_PBR05", -2},  {"SE_MELODY_NPCNRM_PBR06", -2},   {"SE_MELODY_NPCNRM_PBR07", -4},
    {"SE_MELODY_NPCNRM_PBR08", -5},  {"SE_MELODY_NPCNRM_PBR09", 7},    {"SE_MELODY_NPCNRM_PGN02", -2},
    {"SE_MELODY_NPCNRM_PGN06", 5},   {"SE_MELODY_NPCNRM_PGN10", -2},   {"SE_MELODY_NPCNRM_PGN11", -10},
    {"SE_MELODY_NPCNRM_PGN12", 3},   {"SE_MELODY_NPCNRM_PGN13", 3},    {"SE_MELODY_NPCNRM_PGN14", 5},
    {"SE_MELODY_NPCNRM_PIG03", -2},  {"SE_MELODY_NPCNRM_PIG05", -2},   {"SE_MELODY_NPCNRM_PIG08", 3},
    {"SE_MELODY_NPCNRM_PIG13", 2},   {"SE_MELODY_NPCNRM_PIG14", 5},    {"SE_MELODY_NPCNRM_PIG15", 5},
    {"SE_MELODY_NPCNRM_RBT03", -2},  {"SE_MELODY_NPCNRM_RBT06", -7},   {"SE_MELODY_NPCNRM_RBT09", -7},
    {"SE_MELODY_NPCNRM_RBT13", -7},  {"SE_MELODY_NPCNRM_RBT15", 12},   {"SE_MELODY_NPCNRM_RBT16", -7},
    {"SE_MELODY_NPCNRM_RBT18", 5},   {"SE_MELODY_NPCNRM_RBT19", 5},    {"SE_MELODY_NPCNRM_RHN01", 2},
    {"SE_MELODY_NPCNRM_RHN08", 5},   {"SE_MELODY_NPCNRM_SHP01", -2},   {"SE_MELODY_NPCNRM_SHP09", 6},
    {"SE_MELODY_NPCNRM_SHP10", 5},   {"SE_MELODY_NPCNRM_SHP11", 2},    {"SE_MELODY_NPCNRM_SQU00", -1},
    {"SE_MELODY_NPCNRM_SQU04", -5},  {"SE_MELODY_NPCNRM_SQU05", -5},   {"SE_MELODY_NPCNRM_SQU06", -3},
    {"SE_MELODY_NPCNRM_SQU07", -3},  {"SE_MELODY_NPCNRM_SQU11", -2},   {"SE_MELODY_NPCNRM_SQU15", -3},
    {"SE_MELODY_NPCNRM_SQU16", -5},  {"SE_MELODY_NPCNRM_SQU17", -2},   {"SE_MELODY_NPCNRM_SQU18", -2},
    {"SE_MELODY_NPCNRM_TIG05", 7},   {"SE_MELODY_NPCNRM_TIG06", 5},    {"SE_MELODY_NPCNRM_WOL00", 3},
    {"SE_MELODY_NPCNRM_WOL01", 5},   {"SE_MELODY_NPCNRM_WOL02", 5},    {"SE_MELODY_NPCNRM_WOL06", 5},
    {"SE_MELODY_NPCNRM_WOL10", 5},   {"SE_MELODY_NPCSP_ALW", 5},       {"SE_MELODY_NPCSP_BLN", -5},
    {"SE_MELODY_NPCSP_HGH", 2},      {"SE_MELODY_NPCSP_HGS", 1},       {"SE_MELODY_NPCSP_MKA", -2},
    {"SE_MELODY_NPCSP_PKN", 2},      {"SE_MELODY_NPCSP_PLA", -2},      {"SE_MELODY_NPCSP_SEG", -5},
    {"SE_MELODY_NPCSP_SEO", -5},     {"SE_MELODY_NPCSP_SLO", 3},       {"SE_MELODY_NPCSP_SZA0", 12},
    {"SE_MELODY_NPCSP_SZO", -12},    {"SE_MELODY_NPCSP_TAP", 3},       {"SE_MELODY_NPCSP_TTL", -12},
    {"SE_MELODY_NPCSP_WRL", -2},     {"SE_MELODY_DOOR_RECYCLE", -3},   {"SE_MELODY_DOOR_CAFE", 5},
    {"SE_MELODY_DOOR_SHOP", -5},     {"SE_MELODY_DOOR_GARDEN", 24},    {"SE_MELODY_DOOR_SHOESHOP", -7},
    {"SE_MELODY_DOOR_CLUB", 5},      {"SE_MELODY_DOOR_CASE", -12},     {"SE_MELODY_DOOR_ACCESHOP", 5},
}};

// Channel 12's show on the TV of New Leaf and Happy Home Designer. The show's routine (New Leaf 0x2d4690, Happy Home
// Designer 0x1e3e74) rewrites its effect's variable 0 every frame with the ticks left in the part, 3.2 a frame.
// SE_TV_12_RUN loops while the count is 80 or more. The profile gives the count at the run's start, 928. The game
// stops the run as the count falls below 80. citrusf2 plays it on.
constexpr std::string_view kTvRun = "SE_TV_12_RUN";
constexpr uint8_t kTvCount = 0;
constexpr int16_t kTvRunStart = 928;

// Tomodachi Life's music in a Mii's apartment. The game plays BGM_MANSION_AND_ROOM, or BGM_AKICHI_NIGHT when a flag of
// the room is set (0x7f84fc). Each piece holds several arrangements. A chooser (0x7a7260) gives the tracks that play by
// the Mii's state, and the player (0x7a4ecc) mutes the rest as the piece starts. In BGM_MANSION_AND_ROOM, each state
// plays three tracks or one: tracks 0 to 2, tracks 3, 4 and 10, tracks 0, 6 and 12, tracks 5, 9 and 13, tracks 0, 8 and
// 14, or track 15. In BGM_AKICHI_NIGHT, tracks 4 to 7 and tracks 8 to 11 play the same parts. A state plays one of the
// two sets, and tracks 0 to 3 never play. When none of the chooser's tests of the Mii's state holds, it gives tracks 0
// to 2 of BGM_MANSION_AND_ROOM (0x7a7678) and tracks 4 to 7 of BGM_AKICHI_NIGHT (0x7a7314). The profile plays the
// pieces so.
constexpr std::string_view kApartment = "BGM_MANSION_AND_ROOM";
constexpr uint16_t kApartmentTracks = 0x0007;
constexpr std::string_view kApartmentAtNight = "BGM_AKICHI_NIGHT";
constexpr uint16_t kApartmentAtNightTracks = 0x00f0;

// The chooser's other sets of tracks, in the order that it checks them. Tracks 3, 4 and 10 play while the byte at
// +0x1a1 of the object at 0xacb624 is set (0x7a74b4). Tracks 0, 6 and 12 play in a state of kind 2 of the Mii
// (0x7a7518). Tracks 5, 9 and 13 play in a state of kind 6 with the Mii's cold or stomach ache (0x7a7558 to 0x7a75e8).
// Track 15 plays alone while 0x7e29c4 reports a state of the Mii (0x7a7614). Tracks 0, 8 and 14 play while the byte
// at +0x2bc of an object of the Mii is 1 (0x7a766c). BGM_AKICHI_NIGHT plays tracks 8 to 11 while 0x7db8ac reports a
// state of the room (0x7a72e8).
constexpr std::array<Arrangement, 5> kApartmentStates = {{
    {.suffix = "_tracks_3_4_10", .tracks = 0x0418},
    {.suffix = "_tracks_0_6_12", .tracks = 0x1041},
    {.suffix = "_tracks_5_9_13", .tracks = 0x2220},
    {.suffix = "_track_15", .tracks = 0x8000},
    {.suffix = "_tracks_0_8_14", .tracks = 0x4101},
}};
constexpr std::array<Arrangement, 1> kNightApartmentStates = {{
    {.suffix = "_tracks_8_to_11", .tracks = 0x0f00},
}};

// Tomodachi Life's SE_MII_PLAY_GUITAR_S, _M and _L play a Mii's guitar in the chord of the apartment's music. They
// pick their notes by global variable 10. No code writes it. BGM_MANSION_AND_ROOM writes the chord to it as it plays,
// 0 to 4, and 0 first (the sequence's offset 0x5f). At -1 the guitar plays nothing. The profile gives the music's
// first chord. This rests on a reading of the music's bytecode.
constexpr std::array<std::string_view, 3> kGuitars = {"SE_MII_PLAY_GUITAR_S", "SE_MII_PLAY_GUITAR_M",
                                                      "SE_MII_PLAY_GUITAR_L"};
constexpr uint8_t kChord = 26;
constexpr int16_t kFirstChord = 0;

// Ultimate Angler's main music. Tracks 14 and 15 of a piece hold one part, mixed once for the speakers and once for
// headphones. Each time a piece starts on the main music player (the game's code, 0xd52678), the game reads whether
// headphones are plugged in (0xd52c74, through 0xd3c284). It reads the flag again as it runs, and acts when the flag
// changes (0xd524ec, then 0xd51f64). With the speakers, the game mutes track 15 and lets track 14 play. With
// headphones, it mutes track 14 and lets track 15 play. The profile plays the pieces as they sound on the speakers.
// It silences track 15 of every SEQ_BGM_ piece but SEQ_BGM_SPOT_00_A, which the game starts on a second player
// (0xd52718). In this archive, those are the pieces whose tracks 14 and 15 both hold notes.
constexpr std::string_view kFishingMusicPrefix = "SEQ_BGM_";
constexpr std::string_view kFishingSecondPlayerMusic = "SEQ_BGM_SPOT_00_A";
constexpr uint16_t kFishingSpeakerTracks = 0x7fff;

// SEQ_BGM_CROSS holds two arrangements: tracks 0 to 9 with tracks 14 and 15, and tracks 8 to 13. The game plays one
// at a time (0xd50c70). Through 0xd3beb0 it fades the tracks of a mask in and the other tracks out. Bit 15 of that
// mask stands for track 0 and bit 0 for track 15. The piece starts in the first arrangement, with no fade (0xd52b3c,
// mask 0xffc3). Later calls move it between the two arrangements, with fades of 60 frames (masks 0xfc and 0xffc3). The
// profile plays the first, with the speakers' track 14.
constexpr std::string_view kCross = "SEQ_BGM_CROSS";
constexpr uint16_t kCrossTracks = 0x43ff;

// Ultimate Angler's four reeling pieces. Each holds a layer that plays only at tension level 2 and up (the game's code,
// 0xd508d8): track 5 of SEQ_BGM_REELING_NORMAL, track 8 of _BIG, track 6 of _KAIGYO and track 12 of _LASTBOSS. Below
// level 2, the layer plays at a volume of 0. Each piece starts so (0xd5274c). _KAIGYO and _LASTBOSS also mute the layer
// as they start (0xd52884 and 0xd52acc). The first update of the tension unmutes it, at a volume of 0 (0xd50a44). The
// profile plays each piece as it starts, with the speakers' track 14. The arrangement _tension_2 adds the layer.
struct ReelingPiece
{
    std::string_view sound;
    uint16_t tracks;
};

constexpr std::array<ReelingPiece, 4> kReelingPieces = {{
    {"SEQ_BGM_REELING_NORMAL", 0x7fdf},
    {"SEQ_BGM_REELING_BIG", 0x7eff},
    {"SEQ_BGM_REELING_KAIGYO", 0x7fbf},
    {"SEQ_BGM_REELING_LASTBOSS", 0x6fff},
}};
constexpr std::array<Arrangement, 1> kReelingAtTension2 = {{
    {.suffix = "_tension_2", .tracks = kFishingSpeakerTracks},
}};

// Ultimate Angler's ending. The ending's cutscene moves SEQ_BGM_ED2_DEMO through five sets of tracks (the game's code,
// 0xd5195c, through 0xd3beb0). Each change fades the set's tracks in and the other tracks out over 150 frames. The
// cutscene's states run in the order 4, 5, 2, 6, 3, 7, 8, 9, 10, 11, 12. The piece starts with tracks 0 and 1, with no
// fade, at the entry of state 4 (0x7dbfc8). Tracks 0 to 3 follow at the entry of state 8 (0x7dc218). In state 9, tracks
// 0 to 3 with 5 and 6 follow once the dialog reaches its second line (0x7de3d8). Tracks 0 to 8 follow at frame 98 of
// state 10's camera animation (0x7dccf0). Tracks 7 to 10 follow at frame 262 of state 12's camera animation (0x7dced4).
// A line of the dialog closes once its text has typed out. A held button types it faster. The times of the changes
// therefore depend on the player. The profile plays the piece with tracks 0 and 1, as it starts. Each later set is an
// arrangement, named by its step in the game's order: SEQ_BGM_ED2_DEMO_step_1 to SEQ_BGM_ED2_DEMO_step_4.
constexpr std::string_view kEnding = "SEQ_BGM_ED2_DEMO";
constexpr uint16_t kEndingTracks = 0x0003;
constexpr std::array<Arrangement, 4> kEndingSteps = {{
    {.suffix = "_step_1", .tracks = 0x000f},
    {.suffix = "_step_2", .tracks = 0x006f},
    {.suffix = "_step_3", .tracks = 0x01ff},
    {.suffix = "_step_4", .tracks = 0x0780},
}};

// Ultimate Angler's home ambience, SE_ENV_HOME_CROSS. A table (the game's code, 0xee4df8) gives one of eight groups for
// each of 10 rows of 7 columns. 0xd51cac plays the tracks of a row and column's group and mutes the other tracks, with
// no fade. Groups 0 and 7 play tracks 0 and 1. Groups 1 to 5 play tracks 2 to 4, 5 to 7, 8 to 10, 11 and 12, and 13 to
// 15. Group 6 plays track 0. The home scene starts the ambience in row 9's column 2, group 7 (0x750804 and 0x75081c).
// Seven more calls give row 9 a constant column (0x750e30, for one). Five of them play tracks 0 and 1, and two play
// track 0. The tour map's six calls take rows 0 to 8 from its selection (0x765b2c, for one). The profile plays tracks 0
// and 1, as the ambience starts. Each other group is an arrangement, named by its tracks.
constexpr std::string_view kHomeAmbience = "SE_ENV_HOME_CROSS";
constexpr uint16_t kHomeAmbienceTracks = 0x0003;
constexpr std::array<Arrangement, 6> kHomeAmbienceGroups = {{
    {.suffix = "_tracks_2_to_4", .tracks = 0x001c},
    {.suffix = "_tracks_5_to_7", .tracks = 0x00e0},
    {.suffix = "_tracks_8_to_10", .tracks = 0x0700},
    {.suffix = "_tracks_11_and_12", .tracks = 0x1800},
    {.suffix = "_tracks_13_to_15", .tracks = 0xe000},
    {.suffix = "_track_0", .tracks = 0x0001},
}};

// Flower Town's atelier music. A table (the game's code, 0xede978) gives each state of the atelier the tracks
// that play, and a fader (0x7167b4) fades the other tracks out. Four states play tracks 0 to 5, two of them at half
// volume. One plays tracks 6 to 8. Two play tracks 9 to 11 and 13 to 15. The atelier's code sets the state "Normal",
// tracks 0 to 5 at full volume, in seven places (0x6c3a38, 0x6c4714, 0x6c689c, 0x6c6c94, 0x6c8a4c, 0x6c8e24 and
// 0x6c9140), more than any other state. The profile plays the piece in that state.
constexpr std::string_view kAtelier = "BGM_FLOWER_ATELIER";
constexpr uint16_t kAtelierTracks = 0x003f;

// StreetPass Mii Plaza's SEQ_USAGI. The Plaza starts it and sets its state "NORMAL" at once (the Plaza's code,
// 0x2650a8 and 0x2650c4). A table gives each of five states the tracks that play (0x10a26a8). Each frame, a fader
// moves each track's volume toward its state, and mutes a track whose volume is 0 (0x2a9820). The profile plays the
// piece in NORMAL. Each other state is an arrangement, in the table's order. The Plaza sets "SILENCE" at 0x2649e0.
constexpr std::string_view kUsagi = "SEQ_USAGI";
constexpr uint16_t kUsagiTracks = 0x0efd;
constexpr std::array<Arrangement, 4> kUsagiStates = {{
    {.suffix = "_intro", .tracks = 0x0afd},
    {.suffix = "_silence", .tracks = 0x0e00},
    {.suffix = "_tohoho", .tracks = 0x0f01},
    {.suffix = "_happy", .tracks = 0xfeff},
}};

// StreetPass Mii Plaza's seven pieces that follow its new Mii jingle. Each copies global variable 16 as it starts, and
// waits until the variable changes. The jingle, BGM_NEW_Mii or BGM_NEW_Mii_Long, counts its beats in that variable.
// The Plaza's tables of music list the seven pieces with the jingles (0xed9928 and 0xefd01c). In the game each piece
// starts on the jingle's next beat. The profile changes the variable at the second sound frame. The piece then starts
// at once, as the game plays it. The Plaza's system title plays the same seven pieces the same way.
constexpr std::array<std::string_view, 7> kFromNewMii = {
    "BGM_MAIN_Mii_Only_One_From_New_Mii", "BGM_MAIN_Mii_MIN_From_New_Mii", "BGM_MAIN_Mii_MID_From_New_Mii",
    "BGM_MAIN_Mii_MAX_From_New_Mii",      "BGM_MAIN_Mii_No5_From_New_Mii", "BGM_MAIN_Mii_No6_From_New_Mii",
    "BGM_MAIN_Mii_No7_From_New_Mii"};
constexpr VariableChange kNewMiiBeat = {.frame = 1, .index = 16, .value = 1};

// StreetPass Mii Plaza's quick mode, an option that its save keeps (bit 0x400). The Plaza's music manager plays a piece
// at a speed (the Plaza's code, 0x3a16e4 and 0x3a1dec). A speed v gives the sound a pitch of v and a tempo ratio of
// 1 + 0.65 × (v − 1). In quick mode, the manager plays its pieces at a speed of 1.25 (0x3b6aa4): a pitch of 1.25 and a
// tempo ratio of 1.1625. The gate plays BGM_NEW_Mii_Long at the same speed (0x1b719c). The manager's table lists 16
// pieces (0xefd01c). The profile plays each piece at a speed of 1, and the arrangement _quick at the speed of quick
// mode. The seven pieces after the jingle keep their change of global 16 in that state.
constexpr std::array<std::string_view, 16> kQuickModePieces = {
    "BGM_NEW_Mii_Long",
    "BGM_MAIN_Mii_Only_One",
    "BGM_MAIN_Mii_MIN",
    "BGM_MAIN_Mii_MID",
    "BGM_MAIN_Mii_MAX",
    "BGM_MAIN_Mii_No5",
    "BGM_MAIN_Mii_No6",
    "BGM_MAIN_Mii_No7",
    "BGM_MAIN_Mii_Only_One_From_New_Mii",
    "BGM_MAIN_Mii_MIN_From_New_Mii",
    "BGM_MAIN_Mii_MID_From_New_Mii",
    "BGM_MAIN_Mii_MAX_From_New_Mii",
    "BGM_MAIN_Mii_No5_From_New_Mii",
    "BGM_MAIN_Mii_No6_From_New_Mii",
    "BGM_MAIN_Mii_No7_From_New_Mii",
    "SEQ_VIPROOM",
};
constexpr std::array<Arrangement, 1> kQuickMode = {{
    {.suffix = "_quick", .tempo_ratio = 1.1625f, .pitch = 1.25f},
}};

// Ninja Launcher's battle music. The battle's update counts the battle's time down (the Plaza's code, 0x856f2c). Once
// fewer than 10 seconds are left, it sets the current piece's tempo ratio to 1.3, and before that to 1 (0x857810). The
// profile plays each piece as the battle starts. The arrangement _hurry plays it at the tempo of the last 10 seconds.
constexpr std::array<std::string_view, 8> kBattlePieces = {
    "SEQ_BGM_BATTLE1", "SEQ_BGM_BATTLE2", "SEQ_BGM_BATTLE3",     "SEQ_BGM_BATTLE4",
    "SEQ_BGM_BATTLE5", "SEQ_BGM_BATTLE6", "SEQ_BGM_BATTLE_BOSS", "SEQ_BGM_BATTLE_LBOSS",
};
constexpr std::array<Arrangement, 1> kHurryUp = {{
    {.suffix = "_hurry", .tempo_ratio = 1.3f},
}};

// Mii Force's SD_BGM_D01 tests player variable 0 every two beats, and its loop never ends by itself. At a value of 1,
// each track jumps to the piece's ending. The ending takes the key of the part that the piece has reached: one key from
// the 2nd test to the 9th, the other from the 10th to the first test of the next pass. Four of the game's demo scripts
// play the piece, and one changes the variable. The game-over demo sets it to 1 once the boss's last line has gone
// (demo1001.txt, command SetSndSeqGlobalVal, through 0x917c40, 0x91bb6c and 0x80ed88). The time depends on the player's
// buttons. The profile plays the piece as a loop. The arrangement _ending changes the variable just before the 17th
// test: one pass, then the ending. _ending_early changes it just before the 9th, for the other key.
constexpr std::string_view kGameOverPiece = "SD_BGM_D01";
constexpr std::array<Arrangement, 2> kGameOverEndings = {{
    {.suffix = "_ending", .variable = 0, .value = 1, .frame = 4400},
    {.suffix = "_ending_early", .variable = 0, .value = 1, .frame = 2200},
}};

// Ultimate Angler's motor sounds set global variable 31 to 0 (SE_ANIM_CRUISER_MOTOR, for one).
// SE_ANIM_CRUISER_JUMP_IN_MY_MII adds 1 to it as each Mii jumps into the cruiser. The two jump-in sounds take a pitch
// and a volume by its value, 0 to 9. The profile plays the first jump-in, at 0. At the default of -1, the sounds would
// play a pitch that the game never plays. Each later value is an arrangement, for the second jump-in to the tenth.
constexpr std::array<std::string_view, 2> kJumpIns = {"SE_ANIM_CRUISER_JUMP_IN", "SE_ANIM_CRUISER_JUMP_IN_MY_MII"};
constexpr uint8_t kJumpInCount = 31;
constexpr std::array<Arrangement, 9> kLaterJumpIns = {{
    {.suffix = "_jump_2", .variable = kJumpInCount, .value = 1},
    {.suffix = "_jump_3", .variable = kJumpInCount, .value = 2},
    {.suffix = "_jump_4", .variable = kJumpInCount, .value = 3},
    {.suffix = "_jump_5", .variable = kJumpInCount, .value = 4},
    {.suffix = "_jump_6", .variable = kJumpInCount, .value = 5},
    {.suffix = "_jump_7", .variable = kJumpInCount, .value = 6},
    {.suffix = "_jump_8", .variable = kJumpInCount, .value = 7},
    {.suffix = "_jump_9", .variable = kJumpInCount, .value = 8},
    {.suffix = "_jump_10", .variable = kJumpInCount, .value = 9},
}};

// Ultimate Angler's SE_FEVER_PULL_2 bends one track by player variable 0, with a bend range of 50 semitones: 39 cents
// for each step. The game starts the sound with the fever's pull count in the variable, then adds 1 to the count, up
// to 20 (the game's code, 0xd51420). The count goes back to 0 as a fever starts (0xd51804). The profile plays the first
// pull, at 0. At the default of -1, the sound would play 39 cents flat. Each later count is an arrangement, for the
// second pull to the 21st and later ones.
constexpr std::string_view kPull = "SE_FEVER_PULL_2";
constexpr uint8_t kPullCount = 0;
constexpr std::array<Arrangement, 20> kLaterPulls = {{
    {.suffix = "_pull_2", .variable = kPullCount, .value = 1},
    {.suffix = "_pull_3", .variable = kPullCount, .value = 2},
    {.suffix = "_pull_4", .variable = kPullCount, .value = 3},
    {.suffix = "_pull_5", .variable = kPullCount, .value = 4},
    {.suffix = "_pull_6", .variable = kPullCount, .value = 5},
    {.suffix = "_pull_7", .variable = kPullCount, .value = 6},
    {.suffix = "_pull_8", .variable = kPullCount, .value = 7},
    {.suffix = "_pull_9", .variable = kPullCount, .value = 8},
    {.suffix = "_pull_10", .variable = kPullCount, .value = 9},
    {.suffix = "_pull_11", .variable = kPullCount, .value = 10},
    {.suffix = "_pull_12", .variable = kPullCount, .value = 11},
    {.suffix = "_pull_13", .variable = kPullCount, .value = 12},
    {.suffix = "_pull_14", .variable = kPullCount, .value = 13},
    {.suffix = "_pull_15", .variable = kPullCount, .value = 14},
    {.suffix = "_pull_16", .variable = kPullCount, .value = 15},
    {.suffix = "_pull_17", .variable = kPullCount, .value = 16},
    {.suffix = "_pull_18", .variable = kPullCount, .value = 17},
    {.suffix = "_pull_19", .variable = kPullCount, .value = 18},
    {.suffix = "_pull_20", .variable = kPullCount, .value = 19},
    {.suffix = "_pull_21", .variable = kPullCount, .value = 20},
}};

// Ultimate Angler's four crowd effects take the crowd's state in player variable 0 (0xd51f1c, 0xd521ac, 0xd517c0 and
// 0xd51470). 0xd50e00 builds the state from the counts of three kinds of Mii (0x75be18). Bits 0 and 1 give kind A's
// level: 1 Mii, 2 to 6, or 7 to 10. Bits 2 and 3 give kind B's level. With no Mii of kind B, bit 4 is set, and bits 2
// and 3 give kind C's level. Tracks 0 and 2 play kind A's voices, and tracks 1 and 3 kind B's or kind C's. The profile
// plays the fullest crowd, 0x1f, which plays as the default of -1 does. Each other state that 0xd50e00 can give is an
// arrangement, 26 in all. The state with no Mii of any kind is left out. It plays nothing.
constexpr std::array<std::string_view, 4> kCrowdVoices = {"SE_MIIS_VOICE", "SE_MIIS_VOICE_HARD", "SE_MIIS_VOICE_START",
                                                          "SE_MIIS_VOICE_START_FEVER"};
constexpr uint8_t kCrowdState = 0;
constexpr int16_t kFullestCrowd = 0x1f;
constexpr std::array<Arrangement, 26> kOtherCrowds = {{
    {.suffix = "_crowd_a0_b1", .variable = kCrowdState, .value = 0x04},
    {.suffix = "_crowd_a0_b2", .variable = kCrowdState, .value = 0x08},
    {.suffix = "_crowd_a0_b3", .variable = kCrowdState, .value = 0x0c},
    {.suffix = "_crowd_a0_c1", .variable = kCrowdState, .value = 0x14},
    {.suffix = "_crowd_a0_c2", .variable = kCrowdState, .value = 0x18},
    {.suffix = "_crowd_a0_c3", .variable = kCrowdState, .value = 0x1c},
    {.suffix = "_crowd_a1", .variable = kCrowdState, .value = 0x01},
    {.suffix = "_crowd_a1_b1", .variable = kCrowdState, .value = 0x05},
    {.suffix = "_crowd_a1_b2", .variable = kCrowdState, .value = 0x09},
    {.suffix = "_crowd_a1_b3", .variable = kCrowdState, .value = 0x0d},
    {.suffix = "_crowd_a1_c1", .variable = kCrowdState, .value = 0x15},
    {.suffix = "_crowd_a1_c2", .variable = kCrowdState, .value = 0x19},
    {.suffix = "_crowd_a1_c3", .variable = kCrowdState, .value = 0x1d},
    {.suffix = "_crowd_a2", .variable = kCrowdState, .value = 0x02},
    {.suffix = "_crowd_a2_b1", .variable = kCrowdState, .value = 0x06},
    {.suffix = "_crowd_a2_b2", .variable = kCrowdState, .value = 0x0a},
    {.suffix = "_crowd_a2_b3", .variable = kCrowdState, .value = 0x0e},
    {.suffix = "_crowd_a2_c1", .variable = kCrowdState, .value = 0x16},
    {.suffix = "_crowd_a2_c2", .variable = kCrowdState, .value = 0x1a},
    {.suffix = "_crowd_a2_c3", .variable = kCrowdState, .value = 0x1e},
    {.suffix = "_crowd_a3", .variable = kCrowdState, .value = 0x03},
    {.suffix = "_crowd_a3_b1", .variable = kCrowdState, .value = 0x07},
    {.suffix = "_crowd_a3_b2", .variable = kCrowdState, .value = 0x0b},
    {.suffix = "_crowd_a3_b3", .variable = kCrowdState, .value = 0x0f},
    {.suffix = "_crowd_a3_c1", .variable = kCrowdState, .value = 0x17},
    {.suffix = "_crowd_a3_c2", .variable = kCrowdState, .value = 0x1b},
}};

// Ultimate Angler's SE_CATCHED_092 and SE_CATCHED_120 draw a random number as they start. They play their sound on one
// draw in five and one in three, a quieter one on some of the other draws, and nothing on the rest. The profile gives
// each a state of the generator that plays the sound: the states 11 and 2 steps after nw::snd's first state. The
// generator takes 17 more steps before the sequence's first tick (Settings::random_steps_before_start).
struct SeededSound
{
    std::string_view sound;
    uint32_t seed;
};

constexpr std::array<SeededSound, 2> kSeededSounds = {{
    {"SE_CATCHED_092", 0x207db289},
    {"SE_CATCHED_120", 0xcd305e6a},
}};

// Flower Town's SE_COLOR_WHEEL_LIGHT sets global variable 19 to 0. SE_COLOR_WHEEL_LIGHT_2 then plays its first note at
// 0 and its second at 1, and adds 1 each time. At any other value it plays nothing. The profile plays the first note,
// and the arrangement _second plays the second.
constexpr std::string_view kSecondLight = "SE_COLOR_WHEEL_LIGHT_2";
constexpr uint8_t kLightCount = 19;
constexpr std::array<Arrangement, 1> kSecondLightNote = {{
    {.suffix = "_second", .variable = kLightCount, .value = 1},
}};

// The system titles' music. The titles link a sound library that sets the level of each of a piece's 16 tracks.
// System Settings, the second System Settings title and the Activity Log take a mask from the high halfword of a
// sound's user parameter (System Settings' code.bin 0x13f62c, through 0x141728). Bit 15 - N keeps track N (0x113490
// gives track group N the mask 1 << N). As a piece starts, the player mutes each other track (0x131564, through
// 0x1384e4 and the builder of the mute command at 0x137e30). No parameter, or a mask of 0, keeps every track. A later
// piece of the same sequence fades the tracks to its mask instead of starting again (0x13138c). The three pieces of
// System Settings' one sequence play tracks 0 to 6, 7 to 15 and 3. The Activity Log's code is at 0x1cccb0. Its three
// pieces play tracks 0 to 9 of 14, 0 to 11, and 0 to 9 with 12 and 13. Each piece's file plays its tracks.
struct TrackedPiece
{
    std::string_view sound;
    uint16_t tracks;
};

constexpr std::array<TrackedPiece, 3> kSystemSettingsPieces = {{
    {"BGM_CTR_MSET", 0x007f},
    {"BGM_CTR_MSET_NET", 0xff80},
    {"BGM_CTR_MSET_FIRST", 0x0008},
}};

constexpr std::array<TrackedPiece, 3> kActivityLogPieces = {{
    {"BGM_CTR_PLOG_MAIN", 0x03ff},
    {"BGM_CTR_PLOG_MAIN_CLOCK", 0x0fff},
    {"BGM_CTR_PLOG_MAIN_STEP", 0x33ff},
}};

// Mii Maker sets the levels of MEDIT_BGM_FACE's tracks from its code (code.bin 0x19e97c, through SetTrackVolume at
// 0x1eff90). Its first screen starts the piece and sets tracks 8 to 15 to a volume of 0 at once (0x27d0fc). Two other
// routines give the same tracks (0x27d038, and 0x1c4718 with the sound's volume at 0.7). The profile plays the first
// screen's tracks. Two more routines give the later mixes. 0x27d470 sets the sound's volume to 0.7 (0x19ead0), tracks
// 8 and 9 to 0.8 and tracks 3, 4 and 7 to 0. 0x27d9e4 also sets track 10 to 0.75. Tracks 10 to 15 keep the first
// screen's volume of 0 through 0x27d470. The arrangements play the two mixes, named by the tracks that they add. A
// Unicorn run of each routine gives the values. Which screens run them wasn't mapped.
constexpr std::string_view kMiiMakerFace = "MEDIT_BGM_FACE";
constexpr std::array<TrackedPiece, 1> kMiiMakerPieces = {{
    {kMiiMakerFace, 0x00ff},
}};
using TrackVolumes = std::array<float, ntdlib::sequence_export::kSequenceTracks>;
constexpr TrackVolumes kMiiMakerTracks8And9 = {1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f,
                                               0.8f, 0.8f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f};
constexpr TrackVolumes kMiiMakerTracks8To10 = {1.0f, 1.0f, 1.0f,  1.0f, 1.0f, 1.0f, 1.0f, 1.0f,
                                               0.8f, 0.8f, 0.75f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f};
constexpr std::array<Arrangement, 2> kMiiMakerMixes = {{
    {.suffix = "_tracks_8_9", .tracks = 0x0367, .volume = 0.7f, .track_volumes = kMiiMakerTracks8And9},
    {.suffix = "_tracks_8_to_10", .tracks = 0x0767, .volume = 0.7f, .track_volumes = kMiiMakerTracks8To10},
}};

// Super Mario 3D Land plays a melody as the player collects the notes of a trail. The melody's sequence waits at each
// beat, on every track, until track variable 0 (32 in the commands) changes, unless track variable 2 (34) is 1. The
// melody's object (vtable 0x3d6090) starts the sound with variable 0 at 0 (0x1beea0). Each note that the player
// collects adds one to a count of beats to play (0x1ea67c). An update sets variable 0 to 1 for each beat that the count
// holds (0x1bef24). A note collected while the melody plays between beats starts the sound again at that note's beat,
// with variable 2 at 1 (0x1ea6f8). Variable 2 goes back to 0 once the melody reaches the beat (0x1bef80). Each write
// goes to the player and to the variable of all 16 tracks (0x1e8e88). The profile sets variable 2 to 1. Each melody
// then plays as written. The game plays it that way when the player collects each note in time. The game also sets a
// tempo ratio from 0.4 to 2 with each note (0x1710e8 to 0x171114). The profile leaves the ratio at 1.
constexpr std::array<std::string_view, 7> kNoteMelodies = {
    "SS_NOTE_OBJ_CHIKA",   "SS_NOTE_OBJ_SEA", "SS_NOTE_OBJ_MWORLD", "SS_NOTE_OBJ_M3INTRO",
    "SS_NOTE_OBJ_MWORLD2", "SsNoteObjTest",   "SsNoteObjTestLong",
};
constexpr uint8_t kEveryNoteInTime = 34;

// Super Mario 3D Land plays each piece of SoundData/BgmTable.szs at its entry's Volume times the line's volume. The
// table's reader (0x1d7ec4) stores Volume at +4 of each 48-byte entry. The BGM player's constructor (0x250a98) sets
// the line's volume to 1. Once a piece starts, 0x1e9130 passes Volume times the line's volume to the sound's SetVolume
// (0x24c880, called at 0x1e91a4). These are the table's sequenced pieces with a Volume other than 1.
struct SoundVolume
{
    std::string_view sound;
    float volume;
};

constexpr std::array<SoundVolume, 9> kBgmTableVolumes = {{
    {"SS_NOTE_OBJ_CHIKA", 0.55f},
    {"SS_NOTE_OBJ_SEA", 0.6f},
    {"SS_NOTE_OBJ_MWORLD", 0.55f},
    {"SS_NOTE_OBJ_M3INTRO", 0.55f},
    {"SS_NOTE_OBJ_MWORLD2", 0.55f},
    {"SsNoteObjTest", 0.55f},
    {"SsNoteObjTestLong", 0.7f},
    {"SS_KINO_GIFT_SILVER", 0.5f},
    {"SS_KINO_GIFT_GOLD", 0.4f},
}};

// Super Mario 3D Land gives Luigi his voice by starting Mario's voice sequences with other banks. For Luigi, 0x25dcc0
// points the player's sound objects at the table at 0x41e270, whose static constructor (0x211b5c) builds a StartInfo
// with flag 0x10 for each of four banks. Its method 0x3448c8 looks up the sequence's bank in slot 0 in the list at
// 0x3a88cc. It starts the sound with that bank's StartInfo, whose SeqSoundInfo gives all four slots. Bank 0
// (BANK_PLAYER_VOICE_MR) takes bank 1 (BANK_PLAYER_VOICE_LU), and bank 12 (BANK_PLAYER_VOICE_DEMO) takes bank 13
// (BANK_PLAYER_VOICE_DEMO_LU). Bank 23 takes bank 24. The archive's SePvDLu* sequences already list bank 24. Bank 22
// keeps its bank.
constexpr uint32_t kNoBank = 0xffffffff;
constexpr std::array<Arrangement, 1> kLuigiVoice = {{
    {.suffix = "_Luigi", .banks = std::array<uint32_t, 4>{0x03000001, kNoBank, kNoBank, kNoBank}},
}};
constexpr std::array<Arrangement, 1> kLuigiCutsceneVoice = {{
    {.suffix = "_Luigi", .banks = std::array<uint32_t, 4>{0x0300000d, kNoBank, kNoBank, kNoBank}},
}};

// Super Mario 3D Land also starts some effects with a player variable. Most of them transpose their notes by it. The
// profile gives each the value of its first play. The source of ntdlib's ctr::snd::StartingSequenceTracks cites the
// code for each. Each of the game's 36 red coin sets holds 5 coins. The last coin therefore plays at 4.
struct EffectStart
{
    std::string_view sound;
    uint8_t variable;
    int16_t value;
};

constexpr std::array<EffectStart, 9> kEffectStarts = {{
    {"SeSyRedCoin", 0, 0},
    {"SeSyRedCoinStarterComplete", 0, 4},
    {"SeSyGetCollectCoin", 0, 0},
    {"SeSyDmWldInt05MotionCounter", 10, 0},
    {"SeSyBeatEnemySequentially", 0, 0},
    {"SeBmBunbunFireDamage", 0, 0},
    {"SeBmPunpunFireDamage", 0, 0},
    {"SeSyNoteBlockJumpNormalNote", 0, 0},
    {"SeSyNoteBlockJumpHighNote", 0, 0},
}};

// Mario Kart 7 plays a beat sequence beside the BGM of each race course. The beat follows the stream at a tempo ratio
// of the stream's BPM over the beat's 120. The source of ntdlib's ctr::snd::StartingSequenceTracks cites the code.
// Music Park's stream plays at 139 BPM, and at 166 on the final lap. Killer Ship's plays at 148 and 178, and Waluigi
// Pinball's at 135 and 166. The other 29 courses play SEQ_SYNC_BEAT_120, at their streams' BPMs from 112 to 207.
// Track 15 of each beat plays a drum loop that the game sounds only while the player leads the race. It fades in as
// the lead over second place grows. The profile plays it at full level.
constexpr std::string_view kMusicParkBeat = "SEQ_SYNC_BEAT_MUSIC_PARK";
constexpr std::string_view kKillerShipBeat = "SEQ_SYNC_BEAT_KILLER_SHIP";
constexpr std::string_view kWaluigiPinballBeat = "SEQ_SYNC_BEAT_WALUIGI_PINBALL";
constexpr float kMusicParkTempo = 139.0f / 120.0f;
constexpr float kMusicParkFinalLapTempo = 166.0f / 120.0f;
constexpr float kKillerShipTempo = 148.0f / 120.0f;
constexpr float kWaluigiPinballTempo = 135.0f / 120.0f;

// Music Park's beat holds an instrument's part on each of tracks 0 to 3. The game mutes all four as the beat starts.
// Track 15 plays on. Each of the course's instrument roads unmutes one of tracks 0 to 2. An area of the course fades
// track 3 in. On the final lap, the game sets track variable 0 to 1 on tracks 0 to 2. Tracks 0 and 2 then play a
// semitone higher. No other track reads the variable. The arrangements set variable 32, track variable 0 of every
// track. Track 3 stays muted on the final lap. The profile plays the start. The arrangements play the other states.
constexpr uint16_t kMusicParkTracks = 0xfff0;
constexpr uint8_t kFinalLapTranspose = 32;
constexpr std::array<Arrangement, 8> kMusicParkStates = {{
    {.suffix = "_track_0", .tracks = 0xfff1, .tempo_ratio = kMusicParkTempo},
    {.suffix = "_track_1", .tracks = 0xfff2, .tempo_ratio = kMusicParkTempo},
    {.suffix = "_track_2", .tracks = 0xfff4, .tempo_ratio = kMusicParkTempo},
    {.suffix = "_track_3", .tracks = 0xfff8, .tempo_ratio = kMusicParkTempo},
    {.suffix = "_final_lap",
     .tracks = 0xfff0,
     .variable = kFinalLapTranspose,
     .value = 1,
     .tempo_ratio = kMusicParkFinalLapTempo},
    {.suffix = "_final_lap_track_0",
     .tracks = 0xfff1,
     .variable = kFinalLapTranspose,
     .value = 1,
     .tempo_ratio = kMusicParkFinalLapTempo},
    {.suffix = "_final_lap_track_1",
     .tracks = 0xfff2,
     .variable = kFinalLapTranspose,
     .value = 1,
     .tempo_ratio = kMusicParkFinalLapTempo},
    {.suffix = "_final_lap_track_2",
     .tracks = 0xfff4,
     .variable = kFinalLapTranspose,
     .value = 1,
     .tempo_ratio = kMusicParkFinalLapTempo},
}};
constexpr std::array<Arrangement, 1> kKillerShipFinalLap = {{
    {.suffix = "_final_lap", .tempo_ratio = 178.0f / 120.0f},
}};
constexpr std::array<Arrangement, 1> kWaluigiPinballFinalLap = {{
    {.suffix = "_final_lap", .tempo_ratio = 166.0f / 120.0f},
}};

// SEQ_SYNC_BEAT_120 starts at Toad Circuit's 140 BPM, the first course's. The arrangements give Toad Circuit's final
// lap, then each other course and its final lap, named by the stream's label without STRM_CN_ or STRM_CR_ and _N. The
// BPMs come from the archive's 3D information, as the game reads them.
constexpr std::string_view kCourseBeat = "SEQ_SYNC_BEAT_120";
constexpr float kToadCircuitTempo = 140.0f / 120.0f;
constexpr std::array<Arrangement, 57> kCourseBeatStates = {{
    {.suffix = "_toad_circuit_final_lap", .tempo_ratio = 168.0f / 120.0f},
    {.suffix = "_glide_lake", .tempo_ratio = 140.0f / 120.0f},
    {.suffix = "_glide_lake_final_lap", .tempo_ratio = 155.0f / 120.0f},
    {.suffix = "_marine_road", .tempo_ratio = 130.0f / 120.0f},
    {.suffix = "_marine_road_final_lap", .tempo_ratio = 156.0f / 120.0f},
    {.suffix = "_sand_town", .tempo_ratio = 112.0f / 120.0f},
    {.suffix = "_sand_town_final_lap", .tempo_ratio = 136.0f / 120.0f},
    {.suffix = "_wufu_island1", .tempo_ratio = 125.0f / 120.0f},
    {.suffix = "_wufu_island1_final_lap", .tempo_ratio = 150.0f / 120.0f},
    {.suffix = "_mario_circuit", .tempo_ratio = 140.0f / 120.0f},
    {.suffix = "_mario_circuit_final_lap", .tempo_ratio = 168.0f / 120.0f},
    {.suffix = "_rally_cource", .tempo_ratio = 173.0f / 120.0f},
    {.suffix = "_rally_cource_final_lap", .tempo_ratio = 207.0f / 120.0f},
    {.suffix = "_underground", .tempo_ratio = 139.0f / 120.0f},
    {.suffix = "_underground_final_lap", .tempo_ratio = 153.0f / 120.0f},
    {.suffix = "_advanced_circuit", .tempo_ratio = 142.0f / 120.0f},
    {.suffix = "_advanced_circuit_final_lap", .tempo_ratio = 168.0f / 120.0f},
    {.suffix = "_wufu_island2", .tempo_ratio = 125.0f / 120.0f},
    {.suffix = "_wufu_island2_final_lap", .tempo_ratio = 150.0f / 120.0f},
    {.suffix = "_dk_jungle", .tempo_ratio = 137.0f / 120.0f},
    {.suffix = "_dk_jungle_final_lap", .tempo_ratio = 153.0f / 120.0f},
    {.suffix = "_ice_slider", .tempo_ratio = 145.0f / 120.0f},
    {.suffix = "_ice_slider_final_lap", .tempo_ratio = 166.0f / 120.0f},
    {.suffix = "_koopa_castle", .tempo_ratio = 136.0f / 120.0f},
    {.suffix = "_koopa_castle_final_lap", .tempo_ratio = 163.0f / 120.0f},
    {.suffix = "_rainbow_road", .tempo_ratio = 136.0f / 120.0f},
    {.suffix = "_rainbow_road_final_lap", .tempo_ratio = 168.0f / 120.0f},
    {.suffix = "_wario_ship", .tempo_ratio = 152.0f / 120.0f},
    {.suffix = "_wario_ship_final_lap", .tempo_ratio = 174.0f / 120.0f},
    {.suffix = "_n64_luigi_circuit", .tempo_ratio = 140.0f / 120.0f},
    {.suffix = "_n64_luigi_circuit_final_lap", .tempo_ratio = 168.0f / 120.0f},
    {.suffix = "_agb_koopa_castle", .tempo_ratio = 170.0f / 120.0f},
    {.suffix = "_agb_koopa_castle_final_lap", .tempo_ratio = 190.0f / 120.0f},
    {.suffix = "_wii_kinoko_canyon", .tempo_ratio = 133.0f / 120.0f},
    {.suffix = "_wii_kinoko_canyon_final_lap", .tempo_ratio = 150.0f / 120.0f},
    {.suffix = "_ds_luigi_mansion", .tempo_ratio = 122.0f / 120.0f},
    {.suffix = "_ds_luigi_mansion_final_lap", .tempo_ratio = 155.0f / 120.0f},
    {.suffix = "_n64_nokonoko_beach", .tempo_ratio = 118.0f / 120.0f},
    {.suffix = "_n64_nokonoko_beach_final_lap", .tempo_ratio = 141.0f / 120.0f},
    {.suffix = "_sfc_mario_circuit2", .tempo_ratio = 133.0f / 120.0f},
    {.suffix = "_sfc_mario_circuit2_final_lap", .tempo_ratio = 160.0f / 120.0f},
    {.suffix = "_wii_coconuts_mall", .tempo_ratio = 132.0f / 120.0f},
    {.suffix = "_wii_coconuts_mall_final_lap", .tempo_ratio = 158.0f / 120.0f},
    {.suffix = "_n64_karakara_desert", .tempo_ratio = 128.0f / 120.0f},
    {.suffix = "_n64_karakara_desert_final_lap", .tempo_ratio = 154.0f / 120.0f},
    {.suffix = "_ds_dk_snow_mountain", .tempo_ratio = 119.0f / 120.0f},
    {.suffix = "_ds_dk_snow_mountain_final_lap", .tempo_ratio = 153.0f / 120.0f},
    {.suffix = "_gc_daisy_cruiser", .tempo_ratio = 125.0f / 120.0f},
    {.suffix = "_gc_daisy_cruiser_final_lap", .tempo_ratio = 155.0f / 120.0f},
    {.suffix = "_wii_maple_tree_house", .tempo_ratio = 148.0f / 120.0f},
    {.suffix = "_wii_maple_tree_house_final_lap", .tempo_ratio = 200.0f / 120.0f},
    {.suffix = "_wii_nokonoko_cape", .tempo_ratio = 160.0f / 120.0f},
    {.suffix = "_wii_nokonoko_cape_final_lap", .tempo_ratio = 192.0f / 120.0f},
    {.suffix = "_gc_dinodino_jungle", .tempo_ratio = 137.0f / 120.0f},
    {.suffix = "_gc_dinodino_jungle_final_lap", .tempo_ratio = 157.0f / 120.0f},
    {.suffix = "_sfc_rainbow_road", .tempo_ratio = 145.0f / 120.0f},
    {.suffix = "_sfc_rainbow_road_final_lap", .tempo_ratio = 174.0f / 120.0f},
}};

// Mario Kart 7 starts some effects with player variable 0. The player's kart starts the falls into water and lava and
// the glider's air current with 1. The glider's flutter always takes 1. The rise in rank takes 7 less the new rank.
// The profile gives 0, the rise to 7th place. Each driver's horn takes the kart's body, 0 to 16, as its program. At -1
// the horn plays nothing. The profile gives body 0.
constexpr std::array<EffectStart, 22> kKartEffectStarts = {{
    {"SE_KT_FALL_WATER", 0, 1},     {"SE_KT_FALL_LAVA", 0, 1},       {"SE_KT_GLIDER_AIR_CURRENT", 0, 1},
    {"SE_KT_GLIDER_FLUTTER", 0, 1}, {"SE_RC_RANK_UP", 0, 0},         {"SE_KT_HORN_KOOPA", 0, 0},
    {"SE_KT_HORN_DAISY", 0, 0},     {"SE_KT_HORN_DONKEY", 0, 0},     {"SE_KT_HORN_HONEY_QUEEN", 0, 0},
    {"SE_KT_HORN_NOKONOKO", 0, 0},  {"SE_KT_HORN_JUGEM", 0, 0},      {"SE_KT_HORN_LUIGI", 0, 0},
    {"SE_KT_HORN_MARIO", 0, 0},     {"SE_KT_HORN_METALMARIO", 0, 0}, {"SE_KT_HORN_MII", 0, 0},
    {"SE_KT_HORN_PEACH", 0, 0},     {"SE_KT_HORN_ROSETTA", 0, 0},    {"SE_KT_HORN_HEIHO", 0, 0},
    {"SE_KT_HORN_KINOPIO", 0, 0},   {"SE_KT_HORN_WARIO", 0, 0},      {"SE_KT_HORN_HANACHAN", 0, 0},
    {"SE_KT_HORN_YOSHI", 0, 0},
}};

// Sets up the sounds of the tune player and the TV. New Leaf and Happy Home Designer share them.
void SetUpTuneAndTv(std::string_view sound, PerformOptions& perform)
{
    const auto in_family = [sound](std::string_view family)
    {
        return sound.starts_with(family);
    };
    if (std::ranges::find(kTuneSounds, sound) != kTuneSounds.end() ||
        std::ranges::any_of(kTuneSoundFamilies, in_family))
    {
        const auto found = std::ranges::find(kTuneTransposes, sound, &TuneTranspose::sound);
        perform.variables[kTuneNote] = sound == kTrainAnnouncement ? kTrainTuneFirstNote : kNoteC;
        perform.variables[kTuneTranspose] = found == kTuneTransposes.end() ? 0 : found->transpose;
    }

    if (sound == kTvRun)
    {
        perform.variables[kTvCount] = kTvRunStart;
    }
}

// Mario Party: Star Rush plays the music of minigame 708 from one sequence. The minigame's code is in a separate
// module, romfs/dll/mod_mgn708.cro.p. The offsets below are offsets in that file. The module writes the music's
// variables through the sound's handle (nd::SysAudio::SoundHandle::writeVariable). Variable 10 picks the pattern that
// each pass of the loop plays: 0 MGN708_A_OTEHON, 1 _A_RHYTHM_LOOP, 2 _A_KAITOU and 3 _A_INTERVAL_LOOP, and the same
// four for the B set. Variable 9 at 1 moves the A set on to the B set. At 2 it ends the music. SQ_BGM_MGN708_A_FIRST,
// _A_INTERVAL_START, _B_FIRST and _B_INTERVAL_START set both variables as they start. SQ_BGM_MGN708 sets neither,
// and it ends at once unless variable 9 is 0. The module can start SQ_BGM_MGN708 or _A_FIRST as the minigame starts
// (0x6314). An Azahar run of the USA game, with the module at 0x924000, shows the start: the minigame plays
// SQ_BGM_MGN708 and writes 0 to variable 9 right after it (0x73cc). The profile plays that start.
constexpr std::string_view kMinigame708 = "SQ_BGM_MGN708";
constexpr std::string_view kMinigame708B = "SQ_BGM_MGN708_B_FIRST";
constexpr int kMinigame708Set = 9;
constexpr int kMinigame708Pattern = 10;

// The other patterns of minigame 708's two sets. The module writes variable 10 as the minigame goes on: 0 at 0x1fcc,
// 1 at 0x343c, 2 at 0x15d8, 0x1654 and 0x1d84, and 3 at 0x6cc4 and 0x7224. In the Azahar run, the minigame plays the
// A set's four patterns on SQ_BGM_MGN708. It leaves variable 10 unwritten for the first 12 seconds. It then writes
// 3 (0x6cc4), 0 (0x1fcc, from 15 s), 1 (0x343c, 42 s), 2 (0x1d84, 50 s), 1, 3 (0x7224, 66 s) and 0 again. The A set's
// patterns follow SQ_BGM_MGN708 in that order: 3, 0, 1 and 2. SQ_BGM_MGN708 sets neither variable. Each pattern takes
// variable 10 before the first tick, and the profile keeps variable 9 at 0.
constexpr std::array<Arrangement, 4> kMinigame708APatterns = {{
    {.suffix = "_interval_loop", .variable = kMinigame708Pattern, .value = 3},
    {.suffix = "_otehon", .variable = kMinigame708Pattern, .value = 0},
    {.suffix = "_rhythm_loop", .variable = kMinigame708Pattern, .value = 1},
    {.suffix = "_kaitou", .variable = kMinigame708Pattern, .value = 2},
}};

// SQ_BGM_MGN708_B_FIRST starts with pattern 3 of the B set. Each other pattern takes the variable at frame 1. The
// sound's start sets the variable at the first tick. The loop's first pass comes after frame 1 and plays the pattern.
// The module moves the music on to the B set with variable 9 at 1 (0x135c). A patched Azahar run set r2 to 1 at the
// module's check at 0x6bd8 in the second round. The module then wrote variable 9 = 1 for 7 seconds. Each later round
// wrote variable 10 = 3, 0, 1, 2 and 1, as in the A set, and the B set's patterns played at those times.
constexpr std::array<Arrangement, 3> kMinigame708BPatterns = {{
    {.suffix = "_otehon", .variable = kMinigame708Pattern, .value = 0, .frame = 1},
    {.suffix = "_rhythm_loop", .variable = kMinigame708Pattern, .value = 1, .frame = 1},
    {.suffix = "_kaitou", .variable = kMinigame708Pattern, .value = 2, .frame = 1},
}};

} // namespace

Profile ProfileOf(std::span<const uint8_t> archive)
{
    const std::string sha1 = xsflib::Sha1::Hex(xsflib::Sha1::Of(archive));
    if (sha1 == kMarioPartyIslandTourSha1)
    {
        return Profile::kMarioPartyIslandTour;
    }
    if (sha1 == kOcarinaOfTime3dSha1)
    {
        return Profile::kOcarinaOfTime3d;
    }
    if (sha1 == kWarioWareGoldSha1)
    {
        return Profile::kWarioWareGold;
    }
    if (sha1 == kNewLeafSha1)
    {
        return Profile::kNewLeaf;
    }
    if (sha1 == kTomodachiLifeSha1)
    {
        return Profile::kTomodachiLife;
    }
    if (sha1 == kUltimateAnglerSha1)
    {
        return Profile::kUltimateAngler;
    }
    if (sha1 == kFlowerTownSha1)
    {
        return Profile::kFlowerTown;
    }
    if (sha1 == kMiiPlazaSha1)
    {
        return Profile::kMiiPlaza;
    }
    if (sha1 == kMiiForceSha1)
    {
        return Profile::kMiiForce;
    }
    if (sha1 == kNinjaLauncherSha1)
    {
        return Profile::kNinjaLauncher;
    }
    if (sha1 == kMiiPlazaSystemTitleSha1)
    {
        return Profile::kMiiPlazaSystemTitle;
    }
    if (sha1 == kSystemSettingsSha1 || sha1 == kSystemSettings2Sha1)
    {
        return Profile::kSystemSettings;
    }
    if (sha1 == kActivityLogSha1)
    {
        return Profile::kActivityLog;
    }
    if (sha1 == kMiiMakerSha1)
    {
        return Profile::kMiiMaker;
    }
    if (sha1 == kSuperMario3dLandSha1)
    {
        return Profile::kSuperMario3dLand;
    }
    if (sha1 == kMarioKart7Sha1)
    {
        return Profile::kMarioKart7;
    }
    if (sha1 == kHappyHomeDesignerSha1 || sha1 == kHappyHomeDesignerOldSha1)
    {
        return Profile::kHappyHomeDesigner;
    }
    if (sha1 == kMarioPartyStarRushSha1)
    {
        return Profile::kMarioPartyStarRush;
    }

    return Profile::kNone;
}

const char* OtherBuildOf(std::span<const uint8_t> archive)
{
    if (const Profile profile = ProfileOf(archive);
        profile != Profile::kNone && profile != Profile::kMiiPlazaSystemTitle)
    {
        return nullptr;
    }

    // A sound that only each archive holds, as a name of the archive's string table, between two zero bytes.
    struct Build
    {
        std::string_view label;
        const char* game;
    };

    constexpr std::array<Build, 3> kBuilds = {{
        {std::string_view("\0SEQ_BGM_OP_INTRO\0", 18), "Ultimate Angler (add-on content v0.2.0, USA)"},
        {std::string_view("\0BGM_FLOWER_TOWN\0", 17), "Flower Town (add-on content v0.2.0, USA)"},
        {std::string_view("\0BGM_PEACE_MAIN\0", 16), "StreetPass Mii Plaza (update v14.6.0)"},
    }};
    const std::string_view bytes(reinterpret_cast<const char*>(archive.data()), archive.size());
    for (const Build& build : kBuilds)
    {
        if (bytes.find(build.label) != std::string_view::npos)
        {
            return build.game;
        }
    }

    return nullptr;
}

void SetUpSound(Profile profile, std::string_view sound, PerformOptions& perform)
{
    if (profile == Profile::kMarioPartyIslandTour && sound == kIslandTourMenu)
    {
        perform.tracks = kIslandTourMenuTracks;
    }

    if (profile == Profile::kMarioPartyIslandTour)
    {
        for (const TempoPiece& piece : kIslandTourTempoPieces)
        {
            if (sound == piece.sound)
            {
                perform.tracks = piece.tracks;
                perform.variables[kIslandTourTempoVariable] = piece.value;
            }
        }
    }

    if (profile == Profile::kOcarinaOfTime3d && sound == kLonLonRanch)
    {
        perform.tracks = kLonLonRanchTracks;
    }

    if (profile == Profile::kWarioWareGold)
    {
        for (const std::string_view jingle : kTrickyJingles)
        {
            if (sound == jingle)
            {
                perform.tracks = kTrickyJingleTracks;
            }
        }
    }

    if (profile == Profile::kNewLeaf && sound == kMuseum)
    {
        perform.tracks = kMuseumTracks;
    }

    if (profile == Profile::kNewLeaf)
    {
        if (sound == kCafe)
        {
            perform.tracks = kCafeTracks;
        }

        if (sound.starts_with(kDjPrefix))
        {
            perform.variables[kDjStart] = std::ranges::find(kDjAtZero, sound) == kDjAtZero.end() ? 1 : 0;
            perform.fx_sends[0] = kClubSend;

            if (std::ranges::find(kDjTuneInterludes, sound) != kDjTuneInterludes.end())
            {
                for (std::size_t step = 0; step < kFirstDefaultTune.size(); ++step)
                {
                    perform.track_variables.push_back(
                        {.track = kDjTuneTrack, .index = static_cast<uint8_t>(step), .value = kFirstDefaultTune[step]});
                }
            }
        }
    }

    if (profile == Profile::kNewLeaf || profile == Profile::kHappyHomeDesigner)
    {
        SetUpTuneAndTv(sound, perform);
    }

    if (profile == Profile::kTomodachiLife)
    {
        if (sound == kApartment)
        {
            perform.tracks = kApartmentTracks;
        }
        if (sound == kApartmentAtNight)
        {
            perform.tracks = kApartmentAtNightTracks;
        }
        if (std::ranges::find(kGuitars, sound) != kGuitars.end())
        {
            perform.variables[kChord] = kFirstChord;
        }
    }

    if (profile == Profile::kUltimateAngler)
    {
        if (sound.starts_with(kFishingMusicPrefix) && sound != kFishingSecondPlayerMusic)
        {
            perform.tracks = kFishingSpeakerTracks;
        }
        if (sound == kCross)
        {
            perform.tracks = kCrossTracks;
        }

        for (const ReelingPiece& piece : kReelingPieces)
        {
            if (sound == piece.sound)
            {
                perform.tracks = piece.tracks;
            }
        }

        if (sound == kEnding)
        {
            perform.tracks = kEndingTracks;
        }
        if (sound == kHomeAmbience)
        {
            perform.tracks = kHomeAmbienceTracks;
        }

        for (const std::string_view jump_in : kJumpIns)
        {
            if (sound == jump_in)
            {
                perform.variables[kJumpInCount] = 0;
            }
        }

        if (sound == kPull)
        {
            perform.variables[kPullCount] = 0;
        }

        for (const std::string_view voice : kCrowdVoices)
        {
            if (sound == voice)
            {
                perform.variables[kCrowdState] = kFullestCrowd;
            }
        }

        for (const SeededSound& seeded : kSeededSounds)
        {
            if (sound == seeded.sound)
            {
                perform.seed = seeded.seed;
            }
        }
    }

    if (profile == Profile::kFlowerTown && sound == kAtelier)
    {
        perform.tracks = kAtelierTracks;
    }
    if (profile == Profile::kFlowerTown && sound == kSecondLight)
    {
        perform.variables[kLightCount] = 0;
    }

    if (profile == Profile::kMiiPlaza && sound == kUsagi)
    {
        perform.tracks = kUsagiTracks;
    }

    if (profile == Profile::kMiiPlaza || profile == Profile::kMiiPlazaSystemTitle)
    {
        for (const std::string_view piece : kFromNewMii)
        {
            if (sound == piece)
            {
                perform.changes = {kNewMiiBeat};
            }
        }
    }

    if (profile == Profile::kMarioPartyStarRush && sound == kMinigame708)
    {
        perform.variables[kMinigame708Set] = 0;
    }

    std::span<const TrackedPiece> pieces;
    if (profile == Profile::kSystemSettings)
    {
        pieces = kSystemSettingsPieces;
    }
    if (profile == Profile::kActivityLog)
    {
        pieces = kActivityLogPieces;
    }
    if (profile == Profile::kMiiMaker)
    {
        pieces = kMiiMakerPieces;
    }

    for (const TrackedPiece& piece : pieces)
    {
        if (sound == piece.sound)
        {
            perform.tracks = piece.tracks;
        }
    }

    if (profile == Profile::kSuperMario3dLand)
    {
        for (const std::string_view melody : kNoteMelodies)
        {
            if (sound == melody)
            {
                perform.variables[kEveryNoteInTime] = 1;
            }
        }

        for (const EffectStart& effect : kEffectStarts)
        {
            if (sound == effect.sound)
            {
                perform.variables[effect.variable] = effect.value;
            }
        }

        for (const SoundVolume& piece : kBgmTableVolumes)
        {
            if (sound == piece.sound)
            {
                perform.volume = piece.volume;
            }
        }
    }

    if (profile == Profile::kMarioKart7)
    {
        if (sound == kMusicParkBeat)
        {
            perform.tracks = kMusicParkTracks;
            perform.tempo_ratio = kMusicParkTempo;
        }
        if (sound == kKillerShipBeat)
        {
            perform.tempo_ratio = kKillerShipTempo;
        }
        if (sound == kWaluigiPinballBeat)
        {
            perform.tempo_ratio = kWaluigiPinballTempo;
        }
        if (sound == kCourseBeat)
        {
            perform.tempo_ratio = kToadCircuitTempo;
        }

        for (const EffectStart& effect : kKartEffectStarts)
        {
            if (sound == effect.sound)
            {
                perform.variables[effect.variable] = effect.value;
            }
        }
    }
}

std::span<const Arrangement> ArrangementsOf(Profile profile, std::string_view sound, uint32_t first_bank)
{
    if (profile == Profile::kNewLeaf && sound == kCafe)
    {
        return kCafeTrack7;
    }
    if (profile == Profile::kMiiMaker && sound == kMiiMakerFace)
    {
        return kMiiMakerMixes;
    }
    if (profile == Profile::kTomodachiLife && sound == kApartment)
    {
        return kApartmentStates;
    }
    if (profile == Profile::kTomodachiLife && sound == kApartmentAtNight)
    {
        return kNightApartmentStates;
    }
    if (profile == Profile::kMarioKart7 && sound == kMusicParkBeat)
    {
        return kMusicParkStates;
    }
    if (profile == Profile::kMarioKart7 && sound == kKillerShipBeat)
    {
        return kKillerShipFinalLap;
    }
    if (profile == Profile::kMarioKart7 && sound == kWaluigiPinballBeat)
    {
        return kWaluigiPinballFinalLap;
    }
    if (profile == Profile::kMarioKart7 && sound == kCourseBeat)
    {
        return kCourseBeatStates;
    }
    if (profile == Profile::kUltimateAngler && sound == kEnding)
    {
        return kEndingSteps;
    }
    if (profile == Profile::kUltimateAngler && sound == kHomeAmbience)
    {
        return kHomeAmbienceGroups;
    }
    if (profile == Profile::kMiiPlaza && sound == kUsagi)
    {
        return kUsagiStates;
    }
    if (profile == Profile::kFlowerTown && sound == kSecondLight)
    {
        return kSecondLightNote;
    }
    if (profile == Profile::kMarioPartyIslandTour && sound == kIslandTourMenu)
    {
        return kIslandTourMenuLayers;
    }
    if (profile == Profile::kMarioPartyIslandTour)
    {
        for (const TempoPiece& piece : kIslandTourTempoPieces)
        {
            if (sound == piece.sound)
            {
                return piece.arrangements;
            }
        }
    }
    if (profile == Profile::kMarioPartyStarRush && sound == kMinigame708)
    {
        return kMinigame708APatterns;
    }
    if (profile == Profile::kMarioPartyStarRush && sound == kMinigame708B)
    {
        return kMinigame708BPatterns;
    }
    if (profile == Profile::kMiiForce && sound == kGameOverPiece)
    {
        return kGameOverEndings;
    }

    if (profile == Profile::kMiiPlaza)
    {
        for (const std::string_view piece : kQuickModePieces)
        {
            if (sound == piece)
            {
                return kQuickMode;
            }
        }
    }

    if (profile == Profile::kNinjaLauncher)
    {
        for (const std::string_view piece : kBattlePieces)
        {
            if (sound == piece)
            {
                return kHurryUp;
            }
        }
    }

    if (profile == Profile::kUltimateAngler)
    {
        for (const ReelingPiece& piece : kReelingPieces)
        {
            if (sound == piece.sound)
            {
                return kReelingAtTension2;
            }
        }

        for (const std::string_view jump_in : kJumpIns)
        {
            if (sound == jump_in)
            {
                return kLaterJumpIns;
            }
        }

        if (sound == kPull)
        {
            return kLaterPulls;
        }

        for (const std::string_view voice : kCrowdVoices)
        {
            if (sound == voice)
            {
                return kOtherCrowds;
            }
        }
    }

    if (profile == Profile::kSuperMario3dLand && first_bank == 0x03000000)
    {
        return kLuigiVoice;
    }
    if (profile == Profile::kSuperMario3dLand && first_bank == 0x0300000c)
    {
        return kLuigiCutsceneVoice;
    }

    return {};
}

} // namespace citrusf2
