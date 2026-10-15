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
  --var N=V      set variable N (0-15 player, 16-31 global) to V before each
                 sequence starts; repeat for more variables
  --voices N     drop the notes that the game drops for lack of a voice,
                 with N voices (the game has 24 unless it sets another
                 count). The result only approximates the game.
  --voices off   keep every note (default)
  --list         list sequences without converting
  --quiet        print errors only
  --version      show the version
```

Some sequences play nothing until the game sets one of their variables. `--var` sets one before
each sequence starts. As in the game, player and global variables start at -1. Use `--only` to
choose the sequences it applies to.

By default, citrusf2 keeps every note. `--voices N` drops notes as the game does when all N voices
are in use. A new note then takes the voice of the note with the lowest priority. That note stops.
A new note of lower priority than every sounding note is dropped. nw::snd has 24 voices unless a
game sets another count. Pokémon X's code sets 23. The result only approximates the game.
citrusf2 doesn't model the voices that the console's DSP drops.

Files use the sequence names, with adjustments for Windows: forbidden characters and trailing
dots or spaces become `_`, and reserved device names such as `CON` get an added `_`. If names
differ only in ASCII letter case, later sequences get a sound-index suffix to avoid collisions
on Windows and macOS. The same applies to a sequence named `samples`. That sequence's files
would collide with the SFZ sample folder. The default output folder is
`<archive name>_citrusf2`, and its name follows the same rules. Use `--output` to choose another
location.

The conversion follows what the games' sound code does.

## Downloads

The Releases page has citrusf2 for 64-bit Windows. On macOS on Apple Silicon, build it from
source (see [Building](#building)).

## Drag and drop

- **Windows:** drop one or more `.bcsar` files onto `citrusf2.exe`. The console shows the results
  and waits for Enter before closing.
- **macOS:** drop archives onto `Citrusf2.app`, or open it to choose files. The app shows the
  results and offers to open the output folders. It wraps the command-line program because
  Finder cannot drop files onto a bare executable. The app is included in the
  [build](#building).

citrusf2 reports an unreadable archive and goes on with the others. If two inputs would use the
same output folder, the second is skipped to protect the first input's files. This can happen
when names differ only in extension, or only in case on a case-insensitive filesystem. Windows
and macOS usually have one.

## The conversion

### Sequence playback

CSEQ is bytecode, with variables, conditions, random arguments, calls, jumps, loops and tracks
that start other tracks. citrusf2 executes it and records the resulting notes and controllers.
The performer follows Pokémon X's nw::snd player, including:

- All commands and their variable, comparison, random and `if` forms.
- Note lengths, damper, key groups, tie/mono, `notewait off` and waits for sounding notes to
  finish. Note ends are calculated frame by frame. The DSP starts a voice one frame after
  note-on and reports wave completion one frame after the wave ends. One-shot duration follows
  the pitch from the note's sweep, track bend and pitch LFO, recalculated each frame. Envelopes
  start at -90.4 dB and first advance at frame end; notes released or stopped before then are
  inaudible and produce no MIDI note.
- Tick scheduling in thousandths of CPU cycles, in single precision. The game's rounding
  occasionally places a tick in an adjacent sound frame.
- The game's random generator, including its once-per-frame advance. Random choices therefore
  depend on timing. In the game they also depend on what the sound system did before. citrusf2
  starts the generator 17 steps after its seed.
- Main-loop detection and MIDI loop markers. The main loop belongs to the lowest-numbered
  track that can still advance. A pass qualifies only if earlier tracks make no contribution.
  Tracks that wait forever, on held notes or for a negative number of ticks, do not block this
  choice. A pass that changes nothing,
  such as one that only polls or counts a variable, qualifies only while no other track
  changes the sound or plays on. Setting a variable that another track reads counts as a
  change. A track that has sounded nothing and changed nothing for 10 seconds doesn't count as
  playing on. Backward jumps to unvisited commands and passes without waits, such as random
  retries or variable polling, are not loops. Inside a subroutine, a jump back isn't a loop when
  its pass played no note that can be heard and ran a conditional jump, call, return or end that
  could leave the loop. A tie or mono legato on a note below hearing (see below) plays nothing
  that can be heard. A held note that sounds until the game stops it keeps such a loop. A loop
  that a track runs inside a pass of another loop, such as a countdown of waits between notes,
  is an inner loop. Its passes don't qualify until the track has played no pass of another loop
  for 10 seconds. A main loop in which no note starts isn't a loop either once every earlier
  note has ended or is fading out. The sequence then ends with its last sound.
  The markers go on the
  first pass that a player looping on them repeats as the game plays the next: the same notes at
  the same times, with the same settings and tempo. The first pass often differs. Setup before the
  loop can land in its first tick, the game can keep a value from pass to pass, and later passes
  may only tie again a note that the first strikes. When no pass repeats as the game but the next
  pass would, the performance plays that pass too. Otherwise the markers stay on the first pass. A
  loop that draws random numbers keeps its first pass too. At the loop's start, the MIDI file
  sends each controller, pitch, preset and tempo that the game gives back there when the loop
  ends with another value. Without it, a player that loops on the markers would keep the loop
  end's value.

Short loops, such as repeating sound effects, run until the MIDI is at least 10 seconds long.
Indefinitely held sounds are capped at 10 seconds after their last change. A note of 9,600 ticks
or more that its track waits out counts as held. Games use such notes, minutes long at a
typical tempo, for sounds they stop themselves. That also
includes a main loop longer than 10 seconds that ends with more than 10 seconds of no change
and sounds only endless notes held with tie or mono. The game's next pass usually continues
those notes. A sequence that finishes naturally ends with its last sound, including release.
A note counts as below hearing once its envelope has decayed 40 dB. A note without a length,
such as a tie, ends where its envelope has decayed 60 dB. If the sustain level stays above that,
the note ends at 40 dB down. In the game, the channel goes on in silence.
Performances stop after 15 minutes; one that has been silent for more than 10 seconds by then
ends with its last sound too. Tempo or timebase 0 stops sequence timing while notes keep
playing; those notes finish normally, or use the same 10-second limit if held indefinitely.

### MIDI

Each sequence track gets a MIDI track and channel of its own. Channel 10, reserved for drums in
General MIDI, is allocated last. SoundFont synths use bank 128 for that channel. citrusf2 copies
the channel's presets there.

MIDI output preserves tempo changes and uses at least 7680 ticks per quarter note, with an
integer number of MIDI ticks per sequence tick. The game processes ticks in 4.89 ms sound
frames, and the DSP applies their changes at the next frame boundary. citrusf2 places events at
the start of the frame containing each tick, rounded to the nearest MIDI tick. This shifts the
whole sequence one frame earlier while preserving event spacing; at 120 BPM, rounding error is
at most 1/30 ms. Events never cross a loop marker when moved, and the loop keeps its contents. A
sweep or portamento given a portamento time moves on in every frame, between ticks too. The
sweep's pitch bends go at the start of each frame. A sweep goes on after its track closes, while
the note's release sounds, and after the sequence's last tick, at its tempo.

nw::snd applies track settings to notes at frame end. A note therefore starts with its tick's
final settings, and MIDI controller changes are written before notes at the same tick.

| Game setting | MIDI output |
|---|---|
| Track volume, up to 127 | CC7 |
| Track volume above 127 × second volume × main volume × sound volume, plus legato velocity changes without region volume | CC11; as in nw::snd, CC7 and CC11 both act squared. Values are scaled for sequences exceeding MIDI's range (see [Differences from the game](#differences-from-the-game)). |
| Track pan | CC10, mapped to the SoundFont pan law |
| Pitch bend, sweep, portamento and legato key changes | Pitch bend, with RPN 0 range set per channel |
| LFO depth | CC1 = depth / 2, mapped to vibrato or tremolo in the SoundFont |
| Effect sends A and B | Reported, and CC91 and CC93 stay at 0 (see "Routing" under [Differences from the game](#differences-from-the-game)) |
| Immediate track silence | CC120 |
| Filters that change during sounding notes | CC102, mapped to SoundFont cutoff by a modulator |
| Track low-pass (0xd8), for SFZ | CC103 = 64 minus the argument; values <= 6 mean off |
| Biquad filter (0xb4/0xb5), for SFZ | CC104 = filter value; 0 means off |
| Instrument and track envelope, LFO, initial pan and filter settings | Bank select (CC0) and program change |

A MIDI channel can hold only one note per key. Repeated strikes that overlap, common in drums,
get extra channels called layers, with matching controllers, up to the 16-channel limit.

A MIDI channel has one set of controllers for all its notes. Each of the game's notes keeps its
settings. So while notes that the game plays differently from the track's later notes still
sound, the track goes on to another channel if one is left. Those notes are release tails
that the track has detached, and notes still sounding when a note starts that sweeps,
starts at a raised velocity or requires a level of its own. A note whose pitch LFO changes while
it sounds goes on a channel of its own too. The channel's pitch bends follow the LFO (see "LFO
settings" under [Differences from the game](#differences-from-the-game)). Like a layer, each
channel gets a MIDI track named after the sequence track.

### SoundFont

Each distinct combination of bank instrument and track settings becomes a preset. Instrument
regions become zones with:

- Key and velocity ranges resolved by nw::snd's first-match rules.
- The region's sample, root key, tuning, linear volume and pan.
- Envelope parameters after track overrides, timed in 5 ms steps per 4.89 ms sound frame.
  nw::snd's attack is exponential in decibels; the linear SoundFont attack is fitted to it at
  0.55 of its duration. A note that the game releases during its attack gets a longer attack
  in a preset of its own. That attack reaches the game's level when the note is released, and
  the release starts from that level. SoundFont decays and releases fall 100 dB in 101.6 s at
  the slowest, and the game's slowest take up to six times as long. Such a decay gets a longer
  hold, up to SoundFont's limit of 18 s: half the time by which the game's decay reaches its
  sustain level later. The note is then as far above the game's level at the end of the hold as
  below it when its decay reaches the sustain level. Such a release plays at SoundFont's
  slowest. The SFZ files keep the game's times.
- Key groups mapped to exclusive classes.
- Loops repeated as needed to meet SoundFont padding rules: 8 samples before, at least 32
  within, and 8 after. This preserves playback.
- Continued playback for regions that ignore note-off. Their MIDI notes end when the game
  would release them, with release settings that also preserve decay after a repeated strike.

citrusf2 reads CSAR 2.x archives and embedded CSEQ, CBNK, CWAR and CWAV files. Supported wave
encodings are PCM8, PCM16, DSP-ADPCM and IMA-ADPCM. Banks and wave archives stored only in CGRP
groups are also supported. Groups can have wave archives of their own. Copies of a bank in
different groups can then reference different files. citrusf2 uses the first group's copy; the
copies play the same waves.

## SFZ files

SoundFont has one two-pole low-pass filter per voice. It cannot reproduce the game's high-pass
or band-pass presets and only approximates the one-pole track low-pass. SFZ supports all of
these. For it, citrusf2 also writes:

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
  including changes during notes. An inactive low-pass stays at sfizz's maximum 20 kHz cutoff.
  That cutoff attenuates 10 kHz by about 0.5 dB and 16 kHz by 1 dB. SFZ has no biquad bypass
  equivalent to the game's value 0 or absent preset. Instead, CC104 selects filtered or
  unfiltered regions at note-on, and that choice lasts for the note. A note takes the choice
  that its sound frame ends with. The game's notes do the same. Each track uses its first biquad
  type. Later types are omitted and reported.
- **Envelopes.** Decays and releases in sfizz fall linearly in dB, like those in the game, by
  78.17 dB over their given time. They use the game's time for that fall. Attack uses the
  SoundFont fit described above.
- **LFO.** SFZ uses a sine, matching the game, and supports pan LFO.
- **Pan.** A curve maps CC10 to `(value - 64) / 64`. That's the SoundFont convention that the
  MIDI output uses.
- **Level.** SFZ region `volume` restores any gain lost when scaling MIDI CC11. Playback then
  keeps the game's level.
- **Controllers.** CC7, CC10 and CC11 act at once, without sfizz's default 10 ms smoothing. A
  note whose track sets its volume or pan in the tick it starts then begins at that level and
  pan.
- **Loops.** sfizz crossfades the loop end with data before the loop start over at least 1 ms.
  Samples include 4 ms of repeated loop data before the start, and the crossfade can't be heard.
- **Interpolation.** A region that selects linear interpolation on the DSP gets sfizz's linear
  one (`sample_quality=1`). One that selects none plays the nearest sample (`sample_quality=0`).
  The DSP does the same. Other regions use sfizz's default, a cubic interpolation. Cubic
  interpolation is the closest sfizz has to the DSP's polyphase filter.

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
  MIDI pitch bend affects the whole channel. When a note sweeps beside another note still
  sounding, the sweeping note goes on another channel (see "MIDI").
- **Legato.** Tie/mono normally continue the existing note, with pitch bend for key changes
  and CC11 for velocity changes. A note that legatos take above its velocity starts at the
  highest velocity they reach if the instrument plays the same region at that velocity. CC11
  lowers the note until then. Otherwise CC11 goes above 127 (see "Volume
  limits"). CC11 would lower any other note on the channel too. Beside another note's release,
  such a note goes on another channel (see "MIDI"). When no channel is left, a rising legato
  requires a new MIDI note. A changed envelope or LFO preset always requires one. The new note
  restarts the sample and envelope. The game would continue them.
- **Detached notes.** Toggling tie/mono, muting with release or restarting a track detaches its
  sounding notes. They finish releasing with their old settings. In MIDI they keep those on a
  channel of their own (see "MIDI"). When no channel is left, they stay on the track's channel
  and receive later changes. Changes affecting a detached note above -40 dB are reported.
- **Combined pan.** nw::snd adds region pan, track initial pan and track pan before applying
  its pan law. Synths receive track pan separately through CC10 and combine it with zone pan
  using the synth's pan law. For example, a region at +0.38 on a hard-left track is 6.3 dB left in
  the game but 14 dB left in FluidSynth.
- **Pan curves and modes.** citrusf2 always uses the default curve and dual mode. Other curves
  and balance mode, which pans stereo channels together, are reported.
- **LFO settings.** Notes keep the speed, range, type and delay their track has at the end of
  their note-on's tick. That's when the game first gives them to the note. The game updates
  these during playback. A note whose pitch LFO changes while it sounds goes on another channel
  (see "MIDI"). Its preset there has no LFO. The channel's pitch bend adds the game's LFO value
  in each sound frame. When no channel is left, the note keeps its note-on settings. citrusf2
  reports that. A note whose volume or pan LFO changes is reported too. Depth changes work
  through CC1.
- **Deep tremolo.** The game's volume LFO can lower a note as far as silence but raise it by
  no more than 6 dB. The SoundFont and SFZ LFOs swing as far up as down. A volume LFO deeper
  than 6 dB is kept to 6 dB and reported. Such an LFO then dips less than in the game.
- **SoundFont filters.** The two-pole low-pass closely matches the biquad low-pass. For the
  one-pole track filter, the two-pole one passes slightly more just above cutoff and less in the
  top octave. Its slope is twice as steep. Cutoffs are fitted by ntdlib's
  `tools/sequence_export/filter_tables.py`. High-pass and band-pass are omitted; two active
  low-passes use the lower cutoff. SFZ has all these filters.
- **Routing.** Surround pan and main send are omitted. Pan LFO is omitted from SoundFont but
  included in SFZ. The game's code determines whether effect buses A and B have an effect.
  Pokémon X runs none, and a send to them reaches nothing there. citrusf2 reports the sends and
  keeps CC91 and CC93 at 0. A synth's default sends then add no reverb or chorus either.
- **Volume limits.** Archive sound volume, track volume, second/main volume and region volume
  can reach 255. MIDI CC7 holds track volume up to 127. CC11 holds the rest, also up to 127. If
  necessary, all CC11 values are scaled equally. The tracks keep their balance, and the
  sequence's level falls. SoundFont also cannot amplify a sample: if any used region
  exceeds volume 127, every zone is attenuated equally to preserve region balance. citrusf2
  reports the reduction. SFZ restores the MIDI gain and keeps each region's original volume.
- **DSP output.** The DSP resamples each voice through one of three filters. The voice's rate
  decides which: the pitch times the wave's sample rate, over the output's 32,728 Hz. At a rate
  of 1 or below, the filter loses about as much as the synths' interpolation. Above 1, and more
  above 4/3, it cuts more of the highs. SoundFont zones and SFZ regions above a rate of 1
  therefore have a low-pass whose cutoff tracks the key. ntdlib's
  `tools/sequence_export/resampling_filters.py` fits the cutoffs to the DSP's filters. The
  low-pass follows the rate at the zone's key, without bend, sweep or LFO. In the game, the
  filter changes whenever a bend, sweep or LFO moves the rate across 1 or 4/3. A SoundFont
  preset whose filter changes during notes has no such low-pass. citrusf2 reports its notes above
  a rate of 1. In the SFZ files, a track that uses both the low-pass and the biquad has no
  filter left for it. Notes there that start with the biquad on get no resampling low-pass.
  Output limiting differs too. Region requests for linear or no interpolation are reported. The
  SFZ files follow them, and SoundFont synths use their usual interpolation.
- **Voice limits.** The game has 24 voices and drops notes when they run out. Synths usually have
  more. By default, citrusf2 keeps every note. `--voices N` drops notes as the game does. With
  `--voices`, if a track starts over 24 notes in one tick, citrusf2 keeps the first 24. The game
  keeps the newest. Without it, the limit is 1024 notes. After 24 legatos in a tick, the MIDI note
  continues with its existing preset and no gain above its starting level. Legatos that would
  otherwise require a new note are reported. The game's DSP has 24 voices too. A stereo note takes
  two of them. When they run out, the game drops more notes. citrusf2 keeps those notes with or
  without `--voices`.

SoundFont output follows the specification and targets FluidSynth. Player differences remain:

- **Level.** FluidSynth runs the SoundFont low-pass filter on every note. That includes notes
  whose filter is open. The filter divides the level by the square root of its Q. FluidSynth
  converts the default resonance of 0 dB to a Q of 0.707. Every note therefore plays 1.5 dB
  louder than the SoundFont specifies. Multiplying FluidSynth's gain by 0.84 gives the specified
  level.
- **Pan.** Pans are mapped from nw::snd's square-root law to FluidSynth's sin/cos law.
  TinySoundFont uses a square-root law, and its pans come out slightly narrower.
- **Envelopes.** Times follow the specification's 100 dB range. FluidSynth uses 96 dB. Decays
  and releases then fall 4% slower in decibels, and sustain levels are 4% shallower: -11.9 dB
  plays as -11.4 dB. FluidSynth also slows any release faster than 15.6 ms (-7200 timecents) to
  that time.
- **Tremolo.** In FluidSynth, a volume LFO can't raise a note above full level. On a note that
  sustains at full level, a tremolo only dips. The game's tremolo swings as far up as down.
- **Attenuation.** Like E-mu hardware, FluidSynth applies 0.4 of a zone's initial attenuation.
  The specification applies all of it. A region at volume 64 therefore plays 2.4 dB below one
  at 127 in FluidSynth, rather than 6 dB. Attenuation for regions above 127 is reduced the same
  way.
- **Low-pass level.** In FluidSynth, a note through the SoundFont's version of the biquad
  low-pass plays 0.85 dB below an unfiltered one. The game's filter lowers it by 0.5 dB.
- **Pitch.** FluidSynth rounds pitch down to whole cents. Fractional bends can be almost a
  cent flat. A long note's loop can then drift by up to 35 ms per minute against the game.
- **Key groups.** The game releases interrupted group notes at rate 126. They fade out in under
  40 ms. FluidSynth uses -200 timecents for exclusive-class cuts, about 0.9 s over the full
  envelope range.
- **Modulators.** TinySoundFont doesn't support SoundFont modulators. CC1 LFO depth and CC102
  filter changes have no effect there.
- **Tempo changes.** FluidSynth rounds its position to the nearest MIDI tick at each change
  and restarts its clock there. This can advance playback by half a tick per change: 1/30 ms
  at 120 BPM with 7680 ticks per quarter note. TinySoundFont rounds tempo-change times down to
  milliseconds. Its playback runs ahead by about half a millisecond per change on average.
- **Note timing.** FluidSynth applies MIDI events at the start of each 64-sample render block.
  Notes can start up to 64 samples late, about 1.5 ms at 44.1 kHz.
- **Short notes.** FluidSynth holds every note for at least 10 ms (`synth.min-note-length`).
  Shorter notes therefore sound longer than in the game.

## Building

Requires C++20 and CMake 3.20 or later, with no other build dependencies. citrusf2 supports
macOS on Apple Silicon and Windows x64 with clang-cl. CI builds and tests both.

- **Windows:** install Visual Studio 2022 with "Desktop development with C++", including CMake.
  Add the "C++ Clang tools for Windows" component. Use Developer PowerShell or Developer Command
  Prompt.
- **macOS:** install Xcode command line tools (`xcode-select --install`) and CMake
  (`brew install cmake` or the installer from cmake.org).

citrusf2 builds on three libraries: seqlib, xsflib and ntdlib. The repository holds them as Git
submodules in `externals/`. Clone it with its submodules:

```sh
git clone --recursive <repository URL>
```

In a clone made without `--recursive`, run `git submodule update --init` first.

From the repository root:

```sh
cmake -S . -B build
cmake --build build --config Release
ctest --test-dir build -C Release
```

On Windows, the first command is `cmake -S . -B build -A x64 -T ClangCL`. It selects clang-cl
and 64-bit output.

The executable is `build/citrusf2`, or `build/Release/citrusf2.exe` with Visual Studio. Xcode and
other multi-configuration generators also use `build/Release`. The Windows build links the
runtime statically. The executable then runs without runtime DLLs. CI uploads the Windows
executable as an artifact. macOS builds also produce `build/Citrusf2.app`. The app contains a
copy of the program, and you can move it elsewhere.

C++ formatting uses clang-format with the repository's `.clang-format`.

## Tests and tools

`ctest` runs the checks that need no game data: `citrusf2 --version` and the range of `--var`.
The unit tests of the conversion are in seqlib and ntdlib. ntdlib runs its converter tests with
citrusf2's settings on synthetic 3DS sound archives. They cover the archive and wave readers,
the sequence player's timing, loops, legatos and limits, the envelope, pitch and filter models,
and the MIDI, SoundFont and SFZ files.

Other checks and tools:

- `-DCITRUSF2_TEST_ARCHIVE=path/to/archive.bcsar` adds a test that converts every sequence in
  that archive. Independent readers check each output file.
- seqlib's `seqlib_check` program checks the structure of SoundFont and MIDI files. Configure with
  `-DSEQLIB_BUILD_TOOLS=ON`, then build the `seqlib_check` target.
- ntdlib's `tools/sequence_export/filter_tables.py ctr <code.bin>` derives the SoundFont and SFZ
  filter tables from a locally supplied Pokémon X `code.bin`. Requires Python 3 and numpy.

The archive test and the filter tables require game files supplied separately. Neither citrusf2
nor ntdlib distributes them. Normal conversion requires only the input `.bcsar` archive.

## Licence

citrusf2 is under the [MIT](LICENSE) licence. The libraries it builds on, seqlib, xsflib and
ntdlib, are under the MIT licence too. `THIRD-PARTY-NOTICES.md` gives their licence texts. Game
data is not covered by this licence.
