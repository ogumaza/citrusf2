# citrusf2

citrusf2 converts Nintendo 3DS sound archives (`.bcsar`) to MIDI, SoundFont 2 and SFZ.
Each sequence gets a MIDI file and a SoundFont containing its instruments. SFZ output includes
filters that SoundFont cannot reproduce (see [SFZ files](#sfz-files)).

Drop archives onto `citrusf2.exe` on Windows or `Citrusf2.app` on macOS. Output goes in a folder
beside each archive. From a terminal:

```
citrusf2 [options] <archive.bcsar> [<archive.bcsar> ...]

  --output DIR   write to DIR (one archive only)
  --only REGEX   convert matching sequence names (for example --only BGM)
  --loops N      repeat the main loop N times (default 1)
                 MIDI loop markers are included for players that support them
  --seed N       initial game PRNG state (default 0x12345678)
  --list         list sequences without converting
  --quiet        print errors only
  --version      show the version
```

Files use the sequence names, with adjustments for Windows: forbidden characters and trailing
dots or spaces become `_`, and reserved device names such as `CON` get an added `_`. If names
differ only in ASCII letter case, later sequences get a sound-index suffix to avoid collisions
on Windows and macOS. The same applies to a sequence named `samples`, which would collide with
the SFZ sample folder. The default output folder is `<archive name>_citrusf2`; its name follows
the same rules.
Use `--output` to choose another location.

The conversion is based on the 3SF project's reverse engineering of the games' sound code.

## Downloads

The Releases page has citrusf2 for 64-bit Windows. On macOS and Linux, build it from source
(see [Building](#building)).

## Drag and drop

- **Windows:** drop one or more `.bcsar` files onto `citrusf2.exe`. The console shows the results
  and waits for Enter before closing.
- **macOS:** drop archives onto `Citrusf2.app`, or open it to choose files. The app shows the
  results and offers to open the output folders. It wraps the command-line program because
  Finder cannot drop files onto a bare executable. The app is included in the
  [build](#building).

An unreadable archive reports an error without stopping the others. If two inputs would use the
same output folder, the second is skipped to protect the first's files. This can happen when
names differ only in extension, or only in case on a case-insensitive filesystem, as is usual
on Windows and macOS.

## The conversion

### Sequence playback

CSEQ is bytecode, with variables, conditions, random arguments, calls, jumps, loops and tracks
that start other tracks. citrusf2 executes it and records the resulting notes and controllers.
The performer follows Pokémon X's nw::snd player as reconstructed by 3SF, including:

- All commands and their variable, comparison, random and `if` forms.
- Note lengths, damper, key groups, tie/mono, `notewait off` and waits for sounding notes to
  finish. Note ends are calculated frame by frame. The DSP starts a voice one frame after
  note-on and reports wave completion one frame after the wave ends. One-shot duration follows
  the pitch from the note's sweep, track bend and pitch LFO, recalculated each frame. Envelopes
  start at -90.4 dB and first advance at frame end; notes released or stopped before then are
  inaudible and produce no MIDI note.
- Tick scheduling in thousandths of CPU cycles, using single precision. The game's rounding
  occasionally places a tick in an adjacent sound frame.
- The game's random generator, including its once-per-frame advance. Random choices therefore
  depend on timing.
- Main-loop detection and MIDI loop markers. The main loop belongs to the lowest-numbered
  track that can still advance. A pass qualifies only if earlier tracks make no contribution.
  Tracks waiting forever on held notes do not block this choice. Backward jumps to unvisited
  commands and passes without waits, such as random retries or variable polling, are not loops.

Short loops, such as repeating sound effects, run until the MIDI is at least 10 seconds long.
Indefinitely held sounds are capped at 10 seconds after their last change. That includes a main
loop longer than 10 seconds that ends with more than 10 seconds of no change, while only
endless notes held with tie or mono sound, since the game's next pass usually continues them. A
sequence that finishes naturally ends with its last sound, including release. Performances stop
after 15 minutes; one that has been silent for more than 10 seconds by then ends with its last
sound too. Tempo or timebase 0 stops sequence timing while notes keep playing; those notes
finish normally, or use the same 10-second limit if held indefinitely.

### MIDI

Each sequence track gets its own MIDI track and channel. Channel 10, reserved for drums in
General MIDI, is allocated last. SoundFont synths use bank 128 for that channel, so citrusf2
copies the presets it needs there.

MIDI output preserves tempo changes and uses at least 7680 ticks per quarter note, with an
integer number of MIDI ticks per sequence tick. The game processes ticks in 4.89 ms sound
frames, and the DSP applies their changes at the next frame boundary. citrusf2 places events at
the start of the frame containing each tick, rounded to the nearest MIDI tick. This shifts the
whole sequence one frame earlier while preserving event spacing; at 120 BPM, rounding error is
at most 1/30 ms. Events never cross a loop marker when moved, preserving the loop's contents.

nw::snd applies track settings to notes at frame end. A note therefore starts with its tick's
final settings, and MIDI controller changes are written before notes at the same tick.

| Game setting | MIDI output |
|---|---|
| Track volume, up to 127 | CC7 |
| Track volume above 127 × second volume × main volume × sound volume, plus legato velocity changes without region volume | CC11; CC7 and CC11 both act squared, matching nw::snd. Values are scaled for sequences exceeding MIDI's range (see [Differences from the game](#differences-from-the-game)). |
| Track pan | CC10, mapped to the SoundFont pan law |
| Pitch bend, sweep, portamento and legato key changes | Pitch bend, with RPN 0 range set per channel |
| LFO depth | CC1 = depth / 2, mapped to vibrato or tremolo in the SoundFont |
| Effect sends A and B | CC91 and CC93 (reverb and chorus) |
| Immediate track silence | CC120 |
| Filters that change during sounding notes | CC102, mapped to SoundFont cutoff by a modulator |
| Track low-pass (0xd8), for SFZ | CC103 = 64 minus the argument; values <= 6 mean off |
| Biquad filter (0xb4/0xb5), for SFZ | CC104 = filter value; 0 means off |
| Instrument and track envelope, LFO, initial pan and filter settings | Bank select (CC0) and program change |

A MIDI channel can hold only one note per key. Repeated strikes that overlap, common in drums,
get extra channels called layers, with matching controllers, up to the 16-channel limit.

### SoundFont

Each distinct combination of bank instrument and track settings becomes a preset. Instrument
regions become zones with:

- Key and velocity ranges resolved by nw::snd's first-match rules.
- The region's sample, root key, tuning, linear volume and pan.
- Envelope parameters after track overrides, timed in 5 ms steps per 4.89 ms sound frame.
  nw::snd's attack is exponential in decibels; the linear SoundFont attack is fitted to it at
  0.55 of its duration. SoundFont decays and releases fall 100 dB in 101.6 s at the slowest,
  and the game's slowest take up to six times as long. Such a decay gets a longer hold, up to
  SoundFont's limit of 18 s, so that it reaches its sustain level when the game's does. Such a
  release plays at SoundFont's slowest. The SFZ files keep the game's times.
- Key groups mapped to exclusive classes.
- Loops repeated as needed to meet SoundFont padding rules: 8 samples before, at least 32
  within, and 8 after. This preserves playback.
- Continued playback for regions that ignore note-off. Their MIDI notes end when the game
  would release them, with release settings that also preserve decay after a repeated strike.

citrusf2 reads CSAR 2.x archives and embedded CSEQ, CBNK, CWAR and CWAV files. Supported wave
encodings are PCM8, PCM16, DSP-ADPCM and IMA-ADPCM. Banks and wave archives stored only in CGRP
groups are also supported. Groups can have their own wave archives, so copies of a bank may
reference different files. citrusf2 uses the first group's copy; the copies play the same waves.

## SFZ files

SoundFont has one two-pole low-pass filter per voice. It cannot reproduce the game's high-pass
or band-pass presets and only approximates the one-pole track low-pass. SFZ supports all of
these, so citrusf2 also writes:

- `sfz/<sequence>/Track <n>.sfz`: instruments for MIDI `Track <n>` and its layers, for each
  active sequence track.
- `sfz/samples/`: 16-bit WAV samples shared by all sequences in the archive. Stereo channels
  are separate files, panned left and right.

SFZ players ignore MIDI channels, and each file contains one instrument. Load the MIDI in a DAW
and assign an SFZ player, such as sfizz, to each track with its matching file. Output targets
sfizz 1.2.

Presets use program change, plus bank select (CC0) when a sequence has more than 128 presets.
Regions match the SoundFont, with these differences:

- **Filters.** sfizz's `lpf_1p` matches the track low-pass; `lpf_2p`, `hpf_2p` and `bpf_2p` match
  the biquad presets coefficient for coefficient at the DSP rate. When both filters are active,
  they run in series. CC103/CC104 and SFZ curves map track values to cutoff, resonance and gain,
  including changes during notes. An inactive low-pass stays at sfizz's maximum 20 kHz cutoff,
  attenuating 10 kHz by about 0.5 dB and 16 kHz by 1 dB. SFZ has no biquad bypass equivalent to
  the game's value 0 or absent preset, so CC104 selects filtered or unfiltered regions at
  note-on; that choice lasts for the note. Each track uses its first biquad type. Later types
  are omitted and reported.
- **Envelopes.** sfizz decay and release fall linearly in dB, like the game's, by 78.17 dB over
  their specified duration. They use the game's time for that fall. Attack uses the SoundFont
  fit described above.
- **LFO.** SFZ uses a sine, matching the game, and supports pan LFO.
- **Pan.** A curve maps CC10 to `(value - 64) / 64`, matching the SoundFont convention used by
  the MIDI output.
- **Level.** SFZ region `volume` restores any gain lost when scaling MIDI CC11, so playback
  retains the game's level.
- **Loops.** sfizz crossfades the loop end with data before the loop start over at least 1 ms.
  Samples include 4 ms of repeated loop data before the start, making the crossfade inaudible.

Two sfizz 1.2.3 quirks affect playback:

- **Sample rate.** Loading a file configures its filters for 48 kHz. A subsequent sample-rate
  change corrects them. At other rates, load files before setting the rate. `sfizz_render` and
  `sfizz_jack` set it first; `sfizz_render` defaults to 48 kHz unless told otherwise.
- **Streaming.** During offline rendering, streamed samples can fall silent after their
  preloaded portion. The files request full loading with `hint_ram_based=1`. sfizz prints
  "Unsupported value for hint_stealing: 1" for this; the message is harmless.

## Differences from the game

citrusf2 reports approximations for each sequence. Some behavior cannot be represented exactly
in MIDI, SoundFont or SFZ:

- **Per-note pitch.** Game sweeps, portamento and legato key changes affect individual notes.
  MIDI pitch bend affects the whole channel.
- **Legato.** Tie/mono normally continue the existing note, using pitch bend for key changes.
  A velocity increase or changed envelope/LFO preset requires a new MIDI note, restarting the
  sample and envelope where the game would continue them.
- **Detached notes.** Toggling tie/mono, muting with release or restarting a track detaches its
  sounding notes. They finish releasing with their old settings. In MIDI they remain on the
  track's channel and receive later changes. Changes affecting a detached note above -40 dB
  are reported.
- **Combined pan.** nw::snd adds region pan, track initial pan and track pan before applying
  its pan law. Synths receive track pan separately through CC10 and combine it with zone pan
  using their own law. For example, a region at +0.38 on a hard-left track is 6.3 dB left in
  the game but 14 dB left in FluidSynth.
- **Pan curves and modes.** citrusf2 always uses the default curve and dual mode. Other curves
  and balance mode, which pans stereo channels together, are reported.
- **LFO settings.** Notes retain the speed, range, type and delay from note-on. The game can
  update these during playback. Depth changes work through CC1.
- **SoundFont filters.** The two-pole low-pass closely matches the biquad low-pass. For the
  one-pole track filter, it passes slightly more just above cutoff and less in the top octave,
  falling twice as fast. Cutoffs are fitted by `tools/filter_tables.py`. High-pass and
  band-pass are omitted; two active low-passes use the lower cutoff. SFZ has all these filters.
- **Routing.** Surround pan and main send are omitted. Pan LFO is omitted from SoundFont but
  included in SFZ. Game effects, such as Pokémon X's map-specific reverb, become synth reverb
  and chorus sends.
- **Volume limits.** Archive sound volume, track volume, second/main volume and region volume
  can reach 255. MIDI CC7 carries track volume up to 127; CC11 carries the rest but also stops
  at 127. If necessary, all CC11 values are scaled equally, preserving track balance while
  lowering the sequence's level. SoundFont also cannot amplify a sample: if any used region
  exceeds volume 127, every zone is attenuated equally to preserve region balance. citrusf2
  reports the reduction. SFZ restores the MIDI gain and keeps each region's original volume.
- **DSP output.** Synth interpolation differs from the DSP, mainly above 8 kHz, as does output
  limiting. Region requests for linear or no interpolation are reported; synths use their own
  interpolation.
- **Voice limits.** The game has 24 voices; synths usually have more. If a track starts over
  24 notes in one tick, citrusf2 keeps the first 24, while the game keeps the newest. After
  24 legatos in a tick, the MIDI note continues with its existing preset and no gain above its
  starting level. Legatos that would otherwise need a new note are reported.

SoundFont output follows the specification and targets FluidSynth. Player differences remain:

- **Pan.** Pans are mapped from nw::snd's square-root law to FluidSynth's sin/cos law.
  TinySoundFont uses a square-root law, giving a slightly narrower result.
- **Envelopes.** Times follow the specification's 100 dB range. FluidSynth uses 96 dB, making
  decay 4% slower in decibels.
- **Tremolo.** FluidSynth doesn't let a volume LFO raise a note above full level, so on a note
  that sustains at full level a tremolo only dips. The game's swings as far up as down.
- **Attenuation.** Like E-mu hardware, FluidSynth applies 0.4 of a zone's initial attenuation.
  The specification applies all of it. A region at volume 64 therefore plays 2.4 dB below one
  at 127 in FluidSynth, rather than 6 dB. Attenuation for regions above 127 is reduced the same
  way.
- **Pitch.** FluidSynth rounds pitch down to whole cents. Fractional bends can be almost a
  cent flat, allowing a long note's loop to drift by up to 35 ms per minute against the game.
- **Key groups.** The game releases interrupted group notes at rate 126, fading in under
  40 ms. FluidSynth uses -200 timecents for exclusive-class cuts, about 0.9 s over the full
  envelope range.
- **Modulators.** TinySoundFont lacks SoundFont modulator support, so CC1 LFO depth and CC102
  filter changes have no effect.
- **Tempo changes.** FluidSynth rounds its position to the nearest MIDI tick at each change
  and restarts its clock there. This can advance playback by half a tick per change: 1/30 ms
  at 120 BPM with 7680 ticks per quarter note. TinySoundFont rounds tempo-change times down to
  milliseconds, advancing by about half a millisecond per change on average.
- **Note timing.** FluidSynth applies MIDI events at the start of each 64-sample render block.
  Notes can start up to 64 samples late, about 1.5 ms at 44.1 kHz.

## Building

Requires C++20 and CMake 3.20 or later, with no other build dependencies. CI builds and tests
Windows/MSVC x64, macOS/Apple Clang on Apple Silicon and Intel, and Linux/GCC and Clang x64.

- **Windows:** install Visual Studio 2022 or later with "Desktop development with C++",
  including CMake. Use Developer PowerShell or Developer Command Prompt.
- **macOS:** install Xcode command line tools (`xcode-select --install`) and CMake
  (`brew install cmake` or the installer from cmake.org).
- **Linux:** install GCC or Clang and CMake. On Debian/Ubuntu:
  `sudo apt install build-essential cmake`.

From the repository root:

```sh
cmake -S . -B build
cmake --build build --config Release
ctest --test-dir build -C Release
```

The executable is `build/citrusf2`, or `build/Release/citrusf2.exe` with Visual Studio. Xcode and
other multi-configuration generators also use `build/Release`. MSVC links the runtime
statically, so the executable needs no runtime DLLs. CI uploads the Windows executable as an
artifact. macOS builds also produce `build/Citrusf2.app`, containing its own copy of the program;
you can move the app elsewhere.

C++ formatting uses clang-format with the repository's `.clang-format`.

## Tests and tools

`ctest` runs unit tests without game data. Coverage includes:

- DSP-ADPCM and PCM16 decoding, including nw::snd's two-channel limit.
- Truncated files, missing blocks, oversized wave/region counts, invalid bank/wave references,
  and names containing control characters, line breaks or invalid UTF-8.
- Group file lookup, malformed groups/tables, partial files, fallback to complete copies, and
  groups missing from truncated archives. Archive truncation is distinguished from invalid
  file offsets in otherwise complete archives.
- Sequence start offsets and bank slots, loop detection, held loops, the time limit, tie/mono
  and legato gain scaling, unheard notes, one-shot completion, tick rounding, unusual arguments,
  stopped timing, variable polling, loops without waits, voice limits and approximation reports.
- Envelope conversion, pitch ratios, LFO, pan, SoundFont/SFZ filters and loop unrolling.
- SoundFont/MIDI output checked by independent readers, SFZ/WAV output and file naming.

Other checks and tools:

- `-DCITRUSF2_TEST_ARCHIVE=path/to/archive.bcsar` adds conversion of every sequence in that
  archive, validating each output file with independent readers.
- `tools/check_sf2.py` checks SoundFont structure using Python alone.
- `tools/filter_tables.py` derives the SoundFont and SFZ filter tables in `src/filter.cpp`
  from a locally supplied Pokémon X `code.bin`. Requires Python 3 and numpy.
- `tools/compare_with_3sf.py` compares FluidSynth renders of converted files with 3SF renders
  using locally supplied game archives and DSP firmware. It removes MIDI tempo changes while
  preserving event times to avoid FluidSynth's cumulative rounding. Reports cover timing,
  level, spectrum and stereo balance; `--timeline` adds per-second results. Requires Python 3,
  numpy, FluidSynth, 3SF's
  `3sfrip`/`3sfplay` and DSP firmware such as `dspaudio.cdc`:
  `tools/compare_with_3sf.py --firmware dspaudio.cdc --3sf <3SF's build folder> <citrusf2> <archive.bcsar>`.
  Use `--sfizz <libsfizz>` to render SFZ instead, routing each MIDI track through its sequence
  track's SFZ file.

These development tools require game files supplied separately; neither citrusf2 nor 3SF
distributes those files. Normal conversion needs only the input `.bcsar` archive.

## Licence

[MIT](LICENSE). The archive readers follow the MIT-licensed 3SF project. Game data is not
covered by this licence.
