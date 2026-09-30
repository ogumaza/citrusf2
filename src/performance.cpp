// SPDX-License-Identifier: MIT

// Sequence execution (see performance.h), following Pokemon X's nw::snd: MmlParser::Parse (code.bin 0x49036c),
// CommandProc (0x48fad4), SequenceTrack::ParseNextTick (0x31dbf4), UpdateChannelLength (0x31e1c4), NoteOn (0x31e2a4),
// Close (0x31e240), and SequenceSoundPlayer's tick scheduler (0x31ff50).

#include "performance.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <iterator>
#include <map>

#include "binary.h"
#include "envelope.h"
#include "instruments.h"
#include "pan.h"
#include "pitch.h"
#include "wave.h"

namespace citrusf2
{

namespace
{

constexpr int kParseLimit = 10000;     // commands per track per tick before nw::snd yields
constexpr double kNever = 1e300;       // sentinel time in milliseconds
constexpr double kMaxTailMs = 10000.0; // longest release tail kept after the end

// Repeat short loops, such as sustained sound effects, until the performance reaches this duration. Keep loop markers
// around the first pass.
constexpr double kMinLoopSeconds = 10.0;

// Longest performance, in seconds of music.
constexpr double kMaxSeconds = 900.0;

// Maximum duration after a held sound stops changing. This covers tracks waiting on endless looping notes and sequences
// whose timing has stopped.
constexpr double kHoldSeconds = 10.0;

// nn::snd's 1/32728 multiplier for converting voice pitch to DSP sample rate (code.bin 0x1863b0). The actual DSP clock
// is slightly faster: 268111856 / 8192 Hz.
constexpr float kInvDspRate = 0x1.00501ap-15f;

// DSP output samples per sound frame.
constexpr double kFrameSamples = 160.0;

// DSP voice count (nn::snd). Each nw::snd channel needs one voice, or two for stereo.
constexpr int kVoices = 24;

// nw::snd schedules ticks in thousandths of ARM11 clock cycles. A sound frame contains kFrameMilliCycles
// (SequenceSoundPlayer::UpdateTick, code.bin 0x31ff50; see Performer::NextTick).
constexpr float kArmClock = 268111856.0f;
constexpr uint64_t kFrameMilliCycles = 1310720000;

// True for controller events that update a track's channel settings.
bool IsTrackSetting(EventKind kind)
{
    switch (kind)
    {
    case EventKind::kVolume:
    case EventKind::kExpression:
    case EventKind::kPan:
    case EventKind::kPitch:
    case EventKind::kModulation:
    case EventKind::kReverb:
    case EventKind::kChorus:
    case EventKind::kFilter:
    case EventKind::kSfzLowPass:
    case EventKind::kSfzBiquad:
        return true;
    default:
        return false;
    }
}

// A linear ramp over a number of ticks (nw::snd MoveValue).
class MoveValue
{
public:
    // Set immediately.
    void Init(int v)
    {
        start_ = target_ = v;
        duration_ = counter_ = 0;
    }

    // Advance the ramp by one tick.
    void Update()
    {
        if (counter_ < duration_)
        {
            counter_++;
        }
    }

    int Get() const
    {
        if (counter_ >= duration_)
        {
            return target_;
        }

        return (target_ - start_) * counter_ / duration_ + start_;
    }

    bool Moving() const
    {
        return counter_ < duration_;
    }

    // Ramp from the current value to `t` over `ticks`. nw::snd stores the count in 16 bits, so arguments >= 0x8000 are
    // negative and apply immediately.
    void SetTarget(int t, int ticks)
    {
        start_ = Get();
        counter_ = 0;
        target_ = t;
        duration_ = std::max(0, static_cast<int>(static_cast<int16_t>(ticks)));
    }

private:
    int start_ = 0, target_ = 0, duration_ = 0, counter_ = 0;
};

// nw::snd's random generator.
class Random
{
public:
    explicit Random(uint32_t seed) : state_(seed)
    {
    }

    uint16_t Next()
    {
        state_ = state_ * 0x19660Du + 0x3C6EF35Fu;

        return static_cast<uint16_t>(state_ >> 16);
    }

private:
    uint32_t state_;
};

// A sounding nw::snd channel and its corresponding MIDI note.
struct Note
{
    // Return when a one-shot wave's channel is freed (never for a looping wave). The DSP reports completion one frame
    // after the wave ends; nw::snd then frees it after that frame's ticks.
    double NaturalEndMs() const
    {
        if (!one_shot || per_frame <= 0)
        {
            return kNever;
        }

        const double frames = remaining / per_frame;
        return (static_cast<double>(start_frame) + std::ceil(frames - 1e-9) + 1.0) * kFrameMs;
    }

    // Return the earlier of wave completion and release completion.
    double EndMs() const
    {
        return std::min(NaturalEndMs(), release_end_ms);
    }

    // True if the one-shot wave has finished by the end of sound frame `frame`.
    bool WaveEndedBy(uint64_t frame) const
    {
        return one_shot && frame >= start_frame &&
               static_cast<double>(frame + 1 - start_frame) * per_frame >= remaining;
    }

    // Set playback to `new_per_frame` samples per frame, starting after `frame`. First account for samples played
    // through `frame` at the old rate. Finished waves keep their end time. A rate change for the voice's first frame
    // also changes its effective length (see StartWave).
    void ChangeRate(uint64_t frame, double new_per_frame)
    {
        if (WaveEndedBy(frame))
        {
            return;
        }

        if (frame >= start_frame)
        {
            remaining -= static_cast<double>(frame + 1 - start_frame) * per_frame;
            start_frame = frame + 1;
        }
        else
        {
            remaining += (new_per_frame - per_frame) / kFrameSamples;
        }

        per_frame = new_per_frame;
    }

    // Start a one-shot wave of `samples` samples after `frame`, at `new_per_frame` samples per frame. After n frames at
    // rate r, the DSP position is 1 + (160 n - 1) r. This makes the effective length r - 1 samples longer, using the
    // rate from the voice's first frame.
    void StartWave(uint64_t frame, double samples, double new_per_frame)
    {
        one_shot = true;
        start_frame = frame + 1;
        per_frame = new_per_frame;
        remaining = samples + per_frame / kFrameSamples - 1.0;
    }

    // The sweep's pitch offset at `now_ms`, in semitones.
    float SweepOffset(double now_ms) const
    {
        if (sweep == 0.0f || sweep_length <= 0)
        {
            return 0.0f;
        }

        const double elapsed = sweep_in_ms ? (now_ms - sweep_start_ms) / kEnvTimeScale : sweep_ticks;
        if (elapsed >= sweep_length)
        {
            return 0.0f;
        }

        return static_cast<float>(sweep * (sweep_length - elapsed) / sweep_length);
    }

    // Calculate sweep offset in semitones for sound frame `frame`, as Channel::Update does. Timed sweeps advance 5 ms
    // per frame after pitch calculation, starting in the initial frame. Note-length sweeps advance once per tick.
    float SweepAt(uint64_t frame) const
    {
        const int total = static_cast<int>(sweep_length);
        const int gone = sweep_in_ms ? static_cast<int>(std::min<uint64_t>((frame - sweep_frame) * kEnvStepMs, total))
                                     : static_cast<int>(std::min(sweep_ticks, sweep_length));
        if (sweep == 0.0f || gone >= total)
        {
            return 0.0f;
        }

        return static_cast<float>(total - gone) * sweep / static_cast<float>(total);
    }

    // Calculate wave samples played per sound frame at `pitch` semitones from the region's root key. Channel::Update
    // converts 1/256-semitone pitch to a ratio and applies region tuning. nn::snd multiplies by sample rate / 32728
    // (VoiceImpl::UpdateParameters, 0x186134). All arithmetic is single precision.
    double SamplesPerFrame(float pitch) const
    {
        const float ratio = PitchRatio(static_cast<int>(pitch * 256.0f));
        const float voice_pitch = std::max(tune * ratio, 0.0f);
        return kFrameSamples * (voice_pitch * (static_cast<float>(static_cast<int32_t>(sample_rate)) * kInvDspRate));
    }

    uint8_t key = 0;           // the key nw::snd plays
    uint8_t midi_key = 0;      // the key of the MIDI note
    bool midi_on = false;      // the MIDI note is on
    uint8_t midi_velocity = 0; // the MIDI note's velocity
    uint8_t midi_volume = 127; // volume of the region selected for the MIDI note (see Legato)
    uint32_t midi_id = 0;      // the MIDI note's id (TrackEvent::note)
    int32_t length = -1;       // ticks until note-off; -1: none
    bool released = false;
    bool ignore_note_off = false;
    double quiet_ms = kNever; // time an ignore-note-off note becomes inaudible
    uint8_t key_group = 0;
    uint64_t on_frame = 0; // the sound frame the note started in
    EnvelopeValues env;
    double release_end_ms = kNever;
    uint64_t released_frame = 0; // the sound frame the note was last released in
    float release_level = 0;     // 0.1 dB when released
    double release_rate = 0;     // 0.1 dB per envelope millisecond

    // One-shot playback: `remaining` samples at the start of `start_frame`, consumed at `per_frame` samples per frame.
    // The DSP starts a voice one frame after note-on and uses the pitch supplied by nw::snd in the preceding frame.
    bool one_shot = false;
    double remaining = 0, per_frame = 0;
    uint64_t start_frame = 0;

    // Instrument and initial pan at note-on. Tie/mono continue the channel with these values even if the track changes
    // them.
    uint8_t bank_slot = 0;
    uint32_t program = 0;
    int8_t init_pan = 0;

    uint8_t original_key = 60;
    float tune = 1.0f;
    uint32_t sample_rate = 32728;

    // Pitch inputs for Channel::Update, in addition to key and sweep: the track bend, its latest LFO settings (see
    // GiveTrackSettings), and the channel's LFO state.
    float bend = 0.0f;
    Lfo lfo;
    uint8_t lfo_type = 0;

    // Sweep/portamento starts `sweep` semitones from the target and returns linearly.
    float sweep = 0.0f;
    double sweep_length = 0; // ticks, or envelope milliseconds when sweep_in_ms
    double sweep_ticks = 0;
    double sweep_start_ms = 0;
    uint64_t sweep_frame = 0; // the sound frame a timed sweep starts in
    bool sweep_in_ms = false;
};

// Call stack entry: a return address, or a loop start and remaining count.
struct CallEntry
{
    bool is_loop = false;
    uint8_t count = 0;
    uint32_t address = 0;
};

// nw::snd SequenceTrack state and the last values sent to its MIDI channel.
struct Track
{
    // The pitch bend in semitones.
    float Bend() const
    {
        return static_cast<float>(bend.Get()) / 128.0f * bend_range;
    }

    // Reset to the initial sound state, preserving track allocation.
    void Init()
    {
        const bool was_allocated = allocated;
        *this = Track{};
        allocated = was_allocated;

        vol.Init(127);
        pan.Init(0);
        bend.Init(0);
        vars.fill(-1);
    }

    bool allocated = false;
    bool open = false;
    uint32_t current = 0;  // offset of the next command
    bool cmp_flag = true;  // the last comparison's result, which the if prefix tests
    bool note_wait = true; // a note makes the track wait for its length (MML notewait)
    bool tie = false;
    bool mono = false;
    std::array<CallEntry, 3> calls{};
    int call_depth = 0;
    int32_t wait = 0; // ticks until the next command
    bool mute = false;
    bool note_finish_wait = false; // waiting for the track's notes to end
    bool porta = false;
    bool damper = false;
    uint8_t bank_slot = 0;
    uint32_t program = 0;
    uint8_t lfo_depth = 0, lfo_speed = 16, lfo_range = 1, lfo_type = 0;
    int32_t lfo_delay = 0;    // 5 ms units; a negative delay prevents the LFO from starting
    float sweep_pitch = 0.0f; // a note's starting offset from its pitch, in semitones
    MoveValue vol, pan, bend;
    uint8_t volume2 = 127;
    uint8_t velocity_range = 127; // velocities are scaled by this / 127
    uint8_t bend_range = 2;
    int8_t init_pan = 0;
    int8_t transpose = 0;
    uint8_t porta_key = 60; // the key portamento starts from
    uint8_t porta_time = 0; // 0: the sweep takes the note's length
    uint8_t attack = 0xff, decay = 0xff, sustain = 0xff, release = 0xff, hold = 0xff;
    uint8_t fx_send_a = 0, fx_send_b = 0;
    std::array<int16_t, 16> vars{}; // track variables, 32-47 to commands
    std::vector<Note> notes;        // the track's channels, newest last
    std::vector<Note> detached;     // released channels no longer on the track, still sounding

    // First tick of a run without waits (kParseLimit), last tick that began a wait (OnLoop), and counts of
    // channels/legatos started in `note_tick` (NoteOn).
    double running_since_ms = -1.0;
    std::optional<uint32_t> last_wait;
    uint32_t note_tick = 0;
    int tick_notes = 0, tick_legatos = 0;

    // Track filter arguments: low-pass (0xd8, initially off), biquad type/value (0xb4/0xb5), and their SoundFont
    // approximation.
    int32_t low_pass = 0x40;
    int32_t biquad_type = 0, biquad_value = 0;
    SoundFontFilter filter;

    // Last MIDI controller values, valid after the track's first note.
    bool played = false;
    int out_preset = -1;
    int out_volume = -1, out_expression = -1, out_pan = -1, out_mod = -1, out_reverb = -1, out_chorus = -1,
        out_filter = -1;
    int out_sfz_low_pass = 0, out_sfz_biquad = 0; // default MIDI value; filter off
    float out_pitch = 0.0f;

    // Last tick that changed the track's sound, used for main-loop detection (OnLoop). This must agree across Perform's
    // passes even when legato MIDI output differs (Legato). Count every legato as a change. Compare the game's
    // unrounded level before legato gain, MIDI scaling or clamping; legato gain can make CC11 steps finer. Also compare
    // the filter cutoff independently of controller use, and pitch without the legato key offset. change_level,
    // change_filter and change_pitch store those values.
    std::optional<uint32_t> last_change;
    double change_level = -1.0;
    int change_filter = -1;
    float change_pitch = 0.0f;

    // Legato velocity relative to the continuing MIDI note, and key offset in semitones (see Legato).
    double legato_gain = 1.0;
    float legato_bend = 0.0f;
};

// Command argument encodings.
enum ArgType
{
    kArgNone,
    kArgU8,
    kArgS16,
    kArgVmidi,
    kArgRandom,
    kArgVariable
};

// Build a preset key for a new note on `tr`. Envelope values outside 0-127 leave the bank value unchanged. Discard
// settings for ineffective LFOs to avoid duplicate presets. These include unsupported types and negative delays:
// nw::snd converts delay * 5 to unsigned milliseconds, preventing the LFO from starting.
PresetKey TrackPresetKey(const Track& tr)
{
    PresetKey pk;
    pk.bank_slot = tr.bank_slot;
    pk.program = tr.program;
    pk.attack = tr.attack <= 127 ? tr.attack : 0xff;
    pk.decay = tr.decay <= 127 ? tr.decay : 0xff;
    pk.sustain = tr.sustain <= 127 ? tr.sustain : 0xff;
    pk.release = tr.release <= 127 ? tr.release : 0xff;
    pk.hold = tr.hold <= 127 ? tr.hold : 0xff;
    if (tr.lfo_type <= 2 && tr.lfo_delay >= 0)
    {
        pk.lfo_type = tr.lfo_type;
        pk.lfo_range = tr.lfo_range;
        pk.lfo_speed = tr.lfo_speed;
        pk.lfo_delay = static_cast<uint16_t>(tr.lfo_delay);
    }
    else
    {
        pk.lfo_type = 3;
    }
    pk.init_pan = tr.init_pan;

    return pk;
}

// Copy pitch-related settings from track `tr` to channel `n`. SequenceTrack::UpdateChannelParam (0x31de18) does this
// every frame and before detaching channels.
void GiveTrackSettings(const Track& tr, Note& n)
{
    n.bend = tr.Bend();
    n.lfo.Set(tr.lfo_depth, tr.lfo_speed, tr.lfo_range, static_cast<uint32_t>(tr.lfo_delay * kEnvStepMs));
    n.lfo_type = tr.lfo_type;
}

// Build channel `n`'s current preset key from track `tr`, retaining the channel's original instrument and initial pan.
PresetKey ChannelPresetKey(const Track& tr, const Note& n)
{
    PresetKey pk = TrackPresetKey(tr);
    pk.bank_slot = n.bank_slot;
    pk.program = n.program;
    pk.init_pan = n.init_pan;

    return pk;
}

// Execute a sequence tick by tick, following nw::snd::SequenceSoundPlayer, and record its output.
class Performer
{
public:
    // `filter_controller` selects tracks with controller-driven filters (FilterMoves). Multiply CC11 by `level_scale`
    // (LevelPeak).
    Performer(const SoundInfo& sound, const Sequence& seq, BankSet& banks, const PerformOptions& options,
              const std::array<bool, 16>& filter_controller, double level_scale)
        : sound_(sound),
          seq_(seq),
          banks_(banks),
          options_(options),
          rng_(options.seed),
          level_scale_(level_scale),
          filter_controller_(filter_controller)
    {
        player_vars_.fill(-1);
        global_vars_.fill(-1);
    }

    // Run the sound and return its recorded performance.
    Performance Run();

    // Tracks whose filters changed during a sounding note. Their filters need MIDI controllers because presets cannot
    // update an existing note.
    const std::array<bool, 16>& FilterMoves() const
    {
        return filter_moves_;
    }

    // Highest sounding CC11 value before `level_scale` and MIDI clamping. Game volumes can reach 255, so this may
    // exceed 127.
    double LevelPeak() const
    {
        return level_peak_;
    }

private:
    // Sound frame containing `ms`.
    static uint64_t FrameOf(double ms)
    {
        return static_cast<uint64_t>(ms / kFrameMs + 1e-9);
    }

    // Record `now_ms_` for the new `tick_`, and its frame start as a fractional tick (Performance::frame_ticks).
    void RecordTickTime()
    {
        tick_ms_.push_back(now_ms_);
        const double start = static_cast<double>(frame_) * kFrameMs;
        std::size_t k = tick_ms_.size() - 1;
        while (k > 0 && tick_ms_[k] > start)
        {
            k--;
        }

        double tick = static_cast<double>(k);
        if (k + 1 < tick_ms_.size() && tick_ms_[k + 1] > tick_ms_[k])
        {
            tick += (start - tick_ms_[k]) / (tick_ms_[k + 1] - tick_ms_[k]);
        }
        out_.frame_ticks.push_back(std::min(tick, static_cast<double>(tick_)));
    }

    // Advance `frame_`, `frame_rest_` and `now_ms_` to the next tick at the current tempo. Return false when tempo or
    // timebase is 0, which stops nw::snd sequence timing.
    bool NextTick();

    // After sequence timing stops, continue sounding channels at the MIDI tempo (EmitTempo). Releases and one-shot
    // waves finish normally; held notes last kHoldSeconds. No further sequence commands run.
    void PlayOutStopped();

    // Real milliseconds per tick at the current tempo, or 0 at tempo 0 or timebase 0.
    double MsPerTick() const
    {
        const double tpm = static_cast<double>(timebase_) * tempo_; // ticks per minute
        return tpm > 0 ? 60000.0 / tpm : 0.0;
    }

    // First tick at or after `ms`. Extrapolate beyond the last tick at the final tempo, or the MIDI tempo if sequence
    // timing stopped.
    uint32_t MsToTick(double ms) const;

    // Record the current tempo. At tempo or timebase 0, the sequence stops advancing (Run), while MIDI retains its
    // previous tempo.
    void EmitTempo()
    {
        const double bpm = static_cast<double>(tempo_) * timebase_ / out_.timebase;
        if (bpm == 0.0)
        {
            return;
        }

        if (!out_.tempo.empty() && out_.tempo.back().tick == tick_)
        {
            out_.tempo.back().bpm = bpm;
        }
        else if (out_.tempo.empty() || out_.tempo.back().bpm != bpm)
        {
            out_.tempo.push_back({tick_, bpm});
        }
    }

    // Record an event for track `t` at the current tick and update Track::last_change.
    //
    // nw::snd applies track settings at frame end (SequenceTrack::UpdateChannelParam), so a note uses settings from the
    // end of its tick, including changes after its note command. Insert controller changes before that tick's
    // notes/presets, replacing any earlier value for the same setting. SFZ depends on this order to select the biquad
    // filter at note-on.
    void Emit(int t, TrackEvent e)
    {
        e.tick = tick_;
        if (e.kind != EventKind::kExpression && e.kind != EventKind::kFilter && e.kind != EventKind::kPitch)
        {
            tracks_[t].last_change = tick_; // UpdateOutputs handles the three values that can differ between passes
        }

        std::vector<TrackEvent>& events = out_.tracks[t];
        if (!IsTrackSetting(e.kind))
        {
            events.push_back(e);
            return;
        }

        auto at = events.end();
        auto at_tick = [&](std::vector<TrackEvent>::iterator it)
        {
            return it != events.begin() && std::prev(it)->tick == tick_;
        };
        while (at_tick(at) && !IsTrackSetting(std::prev(at)->kind) && std::prev(at)->kind != EventKind::kSoundOff)
        {
            --at;
        }
        for (auto it = at; at_tick(it) && IsTrackSetting(std::prev(it)->kind); --it)
        {
            if (std::prev(it)->kind == e.kind)
            {
                *std::prev(it) = e;
                return;
            }
        }
        events.insert(at, e);
    }

    // Count an approximation required by MIDI or SoundFont.
    void Approximate(const char* what)
    {
        out_.approximations[what]++;
    }

    // Include a stopped note's end time in the sound's duration.
    void Ended(const Note& n)
    {
        sound_end_ms_ = std::max(sound_end_ms_, std::min(n.EndMs(), now_ms_));
    }

    // Allocate track 0, tracks requested by sound flags, and those in a leading alloctrack command.
    void AllocateTracks();

    // End all notes and record duration/end tick. `finished` means the sequence ended naturally.
    void EndPerformance(bool finished);

    // Starts track `t` at command offset `offset`.
    void OpenTrack(int t, uint32_t offset);

    // Releases track `t`'s channels and closes the track.
    void CloseTrack(int t);

    // Run open track `t` for one tick. Return true when it reaches FIN or is stopped for running without waits.
    bool ParseNextTick(int t);

    // Read and execute one command on track `t`. Return true for FIN.
    bool Parse(int t);

    // Read the next byte on track `t`, or return 0xff (FIN) at end of data.
    uint8_t ReadByte(int t);

    // Reads an argument of track `t`, encoded as `type`.
    int32_t ReadArg(int t, ArgType type);

    // Resolve player, global or track variable `index` for track `t`. Return nullptr above 47.
    int16_t* Variable(int t, uint8_t index);

    // Execute a command with its arguments already decoded.
    void CommandProc(int t, uint32_t cmd, int32_t arg1, int32_t arg2);

    // Execute 0xf0 arithmetic (0x80-0x8b) or comparison (0x90-0x95) on variable `index`. Comparisons set the flag used
    // by the if prefix.
    void VariableCommand(int t, uint32_t op, uint8_t index, int16_t v);

    // Unmutes track `t` (mode 0) or mutes it (1-3), first releasing (2) or stopping (3) its channels.
    void Mute(int t, uint8_t mode);

    // Ends a loop of track `t`: jumps back to its start until its count runs out.
    void LoopEnd(int t);

    // Handle track `t` jumping back to `target`. Detect the main loop and stop after the requested repetitions.
    void OnLoop(int t, uint32_t target);

    // Play a note on track `t`, allocating a channel or reusing the newest one for tie/mono.
    void NoteOn(int t, int key, int velocity, int32_t length);

    // Move channel `n` to `key` without another attack. Continue its MIDI note where possible, otherwise start a new
    // one, unless this tick has reached the kVoices legato cap (`past_cap`; see NoteOn).
    void Legato(int t, Note& n, int key, int velocity, int32_t length, bool past_cap);

    // Start `n`'s sweep, combining track sweep pitch and portamento from the previous key.
    void StartSweep(int t, Note& n, int key, int32_t length);

    // Start/end channel `n`'s MIDI note. Multiply track CC11 by `gain` while it plays (see Legato).
    void MidiNoteOn(int t, Note& n, int velocity, double gain = 1.0);
    void MidiNoteOff(int t, Note& n);

    // End `n`'s MIDI note on release, stop or replacement. Remove unheard notes entirely (see Unheard).
    void EndMidiNote(int t, Note& n);

    // True before `n` has been heard. Envelopes start at -90.4 dB and first advance at the end of the note-on frame.
    bool Unheard(const Note& n) const;

    // True if released channel `n` remains above the inaudibility threshold (kInaudibleSustain).
    bool StillAudible(const Note& n) const;

    // Releases channel `n`, at MML release rate `release_value` if one is given.
    void ReleaseNote(int t, Note& n, int release_value = -1);

    // Stops channel `n` at once.
    void StopNote(int t, Note& n);

    // Detach track `t`'s channels, allowing them to keep sounding.
    void DetachAll(int t);

    // Remove stopped channels from track `t`, including detached channels.
    void PruneNotes(int t);

    // Advance track `t`'s channels by one tick. Release notes whose lengths expire and remove stopped channels.
    void UpdateNoteLengths(int t);

    // Run Channel::Update (0x17afcc) for one-shot waves from the previous tick's frame up to, but excluding, `frame`.
    // Compute each next frame's pitch from key, sweep, track bend and pitch LFO.
    void UpdateOneShots(uint64_t frame);

    // Emit changed volume, pan, pitch and other controllers for track `t`.
    void UpdateOutputs(int t);

    // Return track `t`'s CC11 before legato gain, `level_scale_` and the 127 limit (UpdateOutputs).
    double TrackLevel(int t) const;

    // Find `key` in the preset list, adding it if absent.
    int PresetIndex(const PresetKey& key);

    // Build channel `n`'s current preset key, using a controller for the filter if track `t` is in
    // `filter_controller_`.
    PresetKey NoteKey(int t, const Note& n) const;

    // Recalculate track `t`'s filter and record changes affecting sounding notes (FilterMoves). Detached channels keep
    // their previous filter, as in the game.
    void SetFilter(int t);

    // Set track `t`'s LFO `setting` to `value`. Presets fix these at note-on, but the game updates sounding notes.
    // Count changes that would affect them.
    template <typename T>
    void SetLfoSetting(int t, T& setting, T value);

    // Returns true if one of track `t`'s channels is still sounding.
    bool TrackBusy(int t) const;

    // True if any channel of track `t` except `except` is sounding, including detached channels.
    bool OthersSounding(int t, const Note* except) const;

    // True if open track `t` is blocked on held, looping notes that never end.
    bool WaitsForever(int t) const;

    // True if every open track is blocked on endless notes and no sweep, volume, pan or bend can change.
    bool Steady() const;

    const SoundInfo& sound_;
    const Sequence& seq_;
    BankSet& banks_;
    PerformOptions options_;
    Random rng_;
    Performance out_;

    std::array<Track, 16> tracks_{};
    std::array<int16_t, 16> player_vars_{};
    std::array<int16_t, 16> global_vars_{};
    uint32_t timebase_ = 48;
    int tempo_ = 120;
    uint8_t main_volume_ = 127;
    uint32_t tick_ = 0;
    double now_ms_ = 0;
    uint64_t frame_ = 0;                      // the sound frame the current tick is processed in
    uint64_t frame_rest_ = kFrameMilliCycles; // thousandths of ARM cycles left in that frame after the tick
    std::array<std::map<uint32_t, uint32_t>, 16> last_run_; // the last tick each command offset ran at, per track
    std::vector<double> tick_ms_;                           // the time of each tick
    double sound_end_ms_ = 0;                               // end time of the last completed note
    uint32_t next_midi_id_ = 1;
    uint64_t rng_frame_ = 0;  // sound frames the random generator has been advanced for
    uint64_t next_frame_ = 0; // first sound frame with pending channel updates
    bool stop_ = false;
    double level_scale_ = 1.0;
    double level_peak_ = 0.0;
    std::array<bool, 16> filter_controller_{};
    std::array<bool, 16> filter_moves_{};
};

int16_t* Performer::Variable(int t, uint8_t index)
{
    // Player variables 0-15, global 16-31 (SequenceSoundPlayer 0x3201ec), track 32-47.
    if (index < 16)
    {
        return &player_vars_[index];
    }
    if (index < 32)
    {
        return &global_vars_[index - 16];
    }
    if (index < 48)
    {
        return &tracks_[t].vars[index - 32];
    }

    return nullptr;
}

uint8_t Performer::ReadByte(int t)
{
    Track& tr = tracks_[t];
    if (tr.current >= seq_.data.size())
    {
        tr.current = static_cast<uint32_t>(seq_.data.size()) + 1;
        return 0xff; // past the end: behave like FIN
    }

    return seq_.data[tr.current++];
}

int32_t Performer::ReadArg(int t, ArgType type)
{
    // As the game reads arguments (code.bin 0x4908d4).
    switch (type)
    {
    case kArgU8:
        return ReadByte(t);

    case kArgS16:
        {
            const uint32_t hi = ReadByte(t);
            const uint32_t lo = ReadByte(t);
            return static_cast<int32_t>(((hi << 8) & 0xffff) | lo);
        }

    case kArgVmidi:
        {
            // Accept any encoded length. ReadByte returns 0xff at end of data, terminating the number there.
            uint32_t v = 0;
            uint8_t b;
            do
            {
                b = ReadByte(t);
                v = (b & 0x7f) | (v << 7);
            } while ((b & 0x80) && tracks_[t].current <= seq_.data.size());
            return static_cast<int32_t>(v);
        }

    case kArgRandom:
        {
            const uint32_t h1 = ReadByte(t), l1 = ReadByte(t);
            const int32_t lo = static_cast<int16_t>(((h1 << 8) & 0xffff) | l1);
            const uint32_t h2 = ReadByte(t), l2 = ReadByte(t);
            const int32_t hi = static_cast<int16_t>(((h2 << 8) & 0xffff) | l2);
            const uint32_t r = rng_.Next();

            // 32-bit wrapping multiply and arithmetic shift, as the ARM code does.
            const int32_t prod = static_cast<int32_t>(static_cast<uint32_t>((hi - lo) + 1) * r);
            return lo + (prod >> 16);
        }

    case kArgVariable:
        {
            const uint8_t index = ReadByte(t);
            const int16_t* v = Variable(t, index);
            return v ? *v : 0;
        }

    default:
        return 0;
    }
}

int Performer::PresetIndex(const PresetKey& key)
{
    for (std::size_t i = 0; i < out_.presets.size(); i++)
    {
        if (out_.presets[i] == key)
        {
            return static_cast<int>(i);
        }
    }

    out_.presets.push_back(key);

    return static_cast<int>(out_.presets.size() - 1);
}

PresetKey Performer::NoteKey(int t, const Note& n) const
{
    PresetKey pk = ChannelPresetKey(tracks_[t], n);
    if (filter_controller_[t])
    {
        pk.filter_controller = true;
    }
    else
    {
        pk.filter = tracks_[t].filter;
    }

    return pk;
}

void Performer::SetFilter(int t)
{
    Track& tr = tracks_[t];
    const SoundFontFilter filter = ToSoundFont(TrackFilters(tr.low_pass, tr.biquad_type, tr.biquad_value));
    if (filter != tr.filter && TrackBusy(t))
    {
        filter_moves_[t] = true;
    }
    tr.filter = filter;
}

template <typename T>
void Performer::SetLfoSetting(int t, T& setting, T value)
{
    const Track& tr = tracks_[t];
    const PresetKey before = TrackPresetKey(tr);
    setting = value;
    if (tr.lfo_depth != 0 && TrackBusy(t) && TrackPresetKey(tr) != before)
    {
        Approximate("LFO parameters changed during a note (using its note-on settings)");
    }
}

void Performer::OpenTrack(int t, uint32_t offset)
{
    // SetSeqData (0x186d80) and Open (0x186d90) preserve the other track parameters.
    Track& tr = tracks_[t];
    tr.current = offset;
    tr.note_finish_wait = false;
    tr.call_depth = 0;
    tr.wait = 0;
    tr.open = true;
}

void Performer::MidiNoteOff(int t, Note& n)
{
    if (!n.midi_on)
    {
        return;
    }

    n.midi_on = false;
    TrackEvent ev{0, EventKind::kNoteOff, n.midi_key};
    ev.note = n.midi_id;
    Emit(t, ev);
}

void Performer::ReleaseNote(int t, Note& n, int release_value)
{
    // Channel::Release (0x320cbc) applies a new rate even to channels already releasing, as key groups require.
    //
    // Envelopes advance 5 ms per frame, starting in the note-on frame. A channel stops in the frame it falls below
    // -90.4 dB and is gone before the next frame's ticks.
    const int64_t frame = static_cast<int64_t>(frame_);
    if (n.released)
    {
        if (release_value >= 0 && n.release_end_ms > now_ms_)
        {
            const int64_t steps = frame - static_cast<int64_t>(n.released_frame);
            const float level = ReleasedLevel(n.release_level, n.release_rate, steps);
            n.release_level = level;
            n.released_frame = frame_;
            n.release_rate = DecayRate(release_value);
            n.release_end_ms = static_cast<double>(frame + ReleaseFrames(level, n.release_rate) + 1) * kFrameMs;
        }
        return;
    }

    n.released = true;
    n.released_frame = frame_;
    n.release_level = HeldLevel(n.env, frame - static_cast<int64_t>(n.on_frame));
    n.release_rate = DecayRate(release_value >= 0 ? release_value : n.env.release);
    n.release_end_ms = static_cast<double>(frame + ReleaseFrames(n.release_level, n.release_rate) + 1) * kFrameMs;
    EndMidiNote(t, n);
}

void Performer::EndMidiNote(int t, Note& n)
{
    if (!n.midi_on || !Unheard(n))
    {
        MidiNoteOff(t, n);
        return;
    }

    std::vector<TrackEvent>& events = out_.tracks[t];
    auto is_note_on = [&](const TrackEvent& e)
    {
        return e.kind == EventKind::kNoteOn && e.note == n.midi_id;
    };
    const auto on = std::find_if(events.rbegin(), events.rend(), is_note_on);
    if (on != events.rend())
    {
        events.erase(std::next(on).base());
    }

    n.midi_on = false;
}

bool Performer::Unheard(const Note& n) const
{
    return frame_ == n.on_frame;
}

bool Performer::StillAudible(const Note& n) const
{
    const double steps = static_cast<double>(frame_ - n.released_frame);
    const double level = n.release_level - n.release_rate * kEnvStepMs * steps;
    return n.EndMs() > now_ms_ && level > -kInaudibleSustain;
}

void Performer::DetachAll(int t)
{
    // FreeAllChannel (0x31dd0c). Copy the track's final settings before detaching.
    Track& tr = tracks_[t];
    for (Note& n : tr.notes)
    {
        GiveTrackSettings(tr, n);
        tr.detached.push_back(n);
    }
    tr.notes.clear();
}

bool Performer::OthersSounding(int t, const Note* except) const
{
    const Track& tr = tracks_[t];
    for (const Note& n : tr.notes)
    {
        if (&n != except && n.EndMs() > now_ms_)
        {
            return true;
        }
    }

    for (const Note& n : tr.detached)
    {
        if (n.EndMs() > now_ms_)
        {
            return true;
        }
    }

    return false;
}

void Performer::StopNote(int t, Note& n)
{
    // Channel::Stop (0x320b6c) silences immediately. MIDI CC120 cuts the whole channel, so use it only for an audible
    // note with no other sounds remaining on the track.
    const bool heard = !Unheard(n);
    EndMidiNote(t, n);
    if (heard && n.EndMs() > now_ms_)
    {
        if (!OthersSounding(t, &n))
        {
            Emit(t, {0, EventKind::kSoundOff});
        }
        else
        {
            Approximate("immediate channel stop (MIDI release tail remains)");
        }
    }

    n.released = true;
    n.release_end_ms = now_ms_;
}

void Performer::CloseTrack(int t)
{
    // SequenceTrack::Close (0x31e240) releases every channel, even one that ignores note-off.
    Track& tr = tracks_[t];
    for (Note& n : tr.notes)
    {
        ReleaseNote(t, n);
    }

    DetachAll(t);
    tr.open = false;
}

bool Performer::TrackBusy(int t) const
{
    for (const Note& n : tracks_[t].notes)
    {
        if (n.EndMs() > now_ms_)
        {
            return true;
        }
    }

    return false;
}

bool Performer::WaitsForever(int t) const
{
    const Track& tr = tracks_[t];
    if (!tr.note_finish_wait || tr.notes.empty())
    {
        return false;
    }

    for (const Note& n : tr.notes)
    {
        if (n.length > 0 || n.EndMs() < kNever)
        {
            return false;
        }
    }

    return true;
}

bool Performer::Steady() const
{
    bool any = false;
    for (int t = 0; t < 16; t++)
    {
        const Track& tr = tracks_[t];
        if (!tr.open)
        {
            continue;
        }

        any = true;
        if (!WaitsForever(t) || tr.vol.Moving() || tr.pan.Moving() || tr.bend.Moving())
        {
            return false;
        }
        for (const Note& n : tr.notes)
        {
            if (n.SweepOffset(now_ms_) != 0.0f)
            {
                return false;
            }
        }
    }

    return any;
}

void Performer::PruneNotes(int t)
{
    // Remove channels when their one-shot wave or release finishes (channel callbacks, 0x31e148).
    auto ended = [&](Note& n)
    {
        if (n.EndMs() > now_ms_)
        {
            return false;
        }

        MidiNoteOff(t, n); // the wave ended by itself
        Ended(n);

        return true;
    };
    std::erase_if(tracks_[t].notes, ended);
    std::erase_if(tracks_[t].detached, ended);
}

void Performer::UpdateNoteLengths(int t)
{
    // SequenceTrack::UpdateChannelLength (0x31e1c4).
    Track& tr = tracks_[t];
    for (Note& n : tr.notes)
    {
        if (n.length > 0)
        {
            n.length--;
        }

        // Ignore-note-off regions continue playing. End their MIDI note on channel release/stop, wave completion or
        // inaudibility. In the game, silent channels remain allocated until their voice is needed.
        if (n.length == 0 && !n.released && !tr.damper && !n.ignore_note_off)
        {
            ReleaseNote(t, n);
        }
        if (n.ignore_note_off && !n.released && now_ms_ >= n.quiet_ms)
        {
            MidiNoteOff(t, n);
        }

        if (!n.sweep_in_ms)
        {
            n.sweep_ticks += 1.0;
        }
    }

    PruneNotes(t);
}

void Performer::UpdateOneShots(uint64_t frame)
{
    // nw::snd updates channels after each frame's ticks, immediately after copying track settings. Track state stays
    // fixed between ticks, so each channel's intervening frames can be processed together. Stop calculating pitch once
    // the wave ends.
    auto update = [&](Note& n)
    {
        if (!n.one_shot)
        {
            return;
        }

        for (uint64_t f = next_frame_; f < frame && !n.WaveEndedBy(f); f++)
        {
            float pitch = n.SweepAt(f) + static_cast<float>(n.key - n.original_key) + n.bend;
            if (n.lfo_type == 0)
            {
                pitch = n.lfo.Value() + pitch;
            }

            const double per_frame = n.SamplesPerFrame(pitch);
            if (per_frame != n.per_frame)
            {
                n.ChangeRate(f, per_frame);
            }

            n.lfo.Update(kEnvStepMs);
        }
    };

    for (Track& tr : tracks_)
    {
        for (Note& n : tr.notes)
        {
            GiveTrackSettings(tr, n);
            update(n);
        }
        for (Note& n : tr.detached)
        {
            update(n);
        }
    }

    next_frame_ = std::max(next_frame_, frame);
}

void Performer::UpdateOutputs(int t)
{
    // The values SequenceTrack::UpdateChannelParam (0x31de18) gives the track's channels.
    Track& tr = tracks_[t];
    if (!tr.played || !tr.open)
    {
        return;
    }

    // Detached notes keep their old settings in the game. In MIDI they still share the track's channel and receive
    // later controller changes.
    const bool tails = std::ranges::any_of(tr.detached, [&](const Note& n) { return StillAudible(n); });
    auto control = [&](int& last, int value, EventKind kind)
    {
        if (value != last)
        {
            last = value;
            Emit(t, {0, kind, 0, static_cast<uint8_t>(value)});
            if (tails)
            {
                Approximate("detached notes (still receive track controller changes in MIDI)");
            }
        }
    };

    // Channel gain is (vol * volume2 * main volume / 127^3)^2 * sound volume / 127 * (velocity / 127)^2. Each volume
    // can reach 255. SoundFont CC7, CC11 and velocity all act squared: put up to 127 of track volume in CC7, and the
    // excess, other volumes and legato velocity change in CC11. If CC11 exceeds 127, rerun with all CC11 values scaled
    // equally (Perform).
    control(tr.out_volume, std::clamp(tr.vol.Get(), 0, 127), EventKind::kVolume);
    const double level = TrackLevel(t);
    control(tr.out_expression, std::clamp(static_cast<int>(std::lround(level * tr.legato_gain * level_scale_)), 0, 127),
            EventKind::kExpression);
    const int change_filter = FilterControllerValue(tr.filter.cutoff);
    if (level != tr.change_level || change_filter != tr.change_filter)
    {
        tr.change_level = level;
        tr.change_filter = change_filter;
        tr.last_change = tick_;
    }

    // nw::snd pan is value / 63, and the SoundFont CC10 modulator is (value - 64) / 64, on the synth's pan curve (see
    // pan.h).
    const int pan = std::clamp(static_cast<int>(std::lround(64.0 + SoundFontPan(tr.pan.Get() / 63.0) * 64.0)), 0, 127);
    control(tr.out_pan, pan, EventKind::kPan);

    control(tr.out_mod, std::min(127, (tr.lfo_depth + 1) / 2), EventKind::kModulation);
    control(tr.out_reverb, std::min<int>(127, tr.fx_send_a), EventKind::kReverb);
    control(tr.out_chorus, std::min<int>(127, tr.fx_send_b), EventKind::kChorus);
    if (filter_controller_[t])
    {
        control(tr.out_filter, FilterControllerValue(tr.filter.cutoff), EventKind::kFilter);
    }

    // SFZ controllers carry the original filter settings. Each track file uses its first enabled biquad type; omit
    // later types.
    control(tr.out_sfz_low_pass, SfzLowPassValue(tr.low_pass), EventKind::kSfzLowPass);
    uint8_t biquad = SfzBiquadValue(tr.biquad_type, tr.biquad_value);
    if (biquad != 0 && out_.biquad_types[t] == 0)
    {
        out_.biquad_types[t] = static_cast<uint8_t>(tr.biquad_type);
    }
    if (tr.biquad_type != out_.biquad_types[t])
    {
        biquad = 0;
    }
    control(tr.out_sfz_biquad, biquad, EventKind::kSfzBiquad);

    // Combine track bend, legato key offset and the newest active note sweep. MIDI has one pitch per channel; nw::snd
    // has one per note. For change detection, exclude the legato key offset (Track::last_change).
    float pitch = tr.Bend() + tr.legato_bend;
    float game_pitch = tr.Bend();
    for (auto it = tr.notes.rbegin(); it != tr.notes.rend(); ++it)
    {
        const float s = it->SweepOffset(now_ms_);
        if (s != 0.0f)
        {
            pitch += s;
            game_pitch += s;
            break;
        }
    }
    if (std::fabs(game_pitch - tr.change_pitch) > 0.0005f || (game_pitch != tr.change_pitch && game_pitch == 0.0f))
    {
        tr.change_pitch = game_pitch;
        tr.last_change = tick_;
    }
    if (std::fabs(pitch - tr.out_pitch) > 0.0005f || (pitch != tr.out_pitch && pitch == 0.0f))
    {
        tr.out_pitch = pitch;
        TrackEvent ev{0, EventKind::kPitch};
        ev.semitones = pitch;
        Emit(t, ev);
        if (tails)
        {
            Approximate("detached notes (still receive track controller changes in MIDI)");
        }
    }
}

double Performer::TrackLevel(int t) const
{
    const Track& tr = tracks_[t];
    return std::max(tr.vol.Get(), 127) * (tr.volume2 / 127.0) * (main_volume_ / 127.0) *
           std::sqrt(sound_.volume / 127.0);
}

void Performer::StartSweep(int t, Note& n, int key, int32_t length)
{
    Track& tr = tracks_[t];
    float sweep = tr.sweep_pitch;
    if (tr.porta)
    {
        sweep += static_cast<float>(static_cast<int>(tr.porta_key) - key);
    }

    n.sweep = sweep;
    n.sweep_ticks = 0;
    n.sweep_start_ms = now_ms_;
    n.sweep_frame = frame_;
    if (tr.porta_time != 0)
    {
        const int tt = tr.porta_time * tr.porta_time;
        const int time = static_cast<int>(std::fabs(sweep) * static_cast<float>(tt)) >> 5;
        n.sweep_length = time * kEnvStepMs;
        n.sweep_in_ms = true;
    }
    else
    {
        n.sweep_length = length > 0 ? length : 0;
        n.sweep_in_ms = false;
    }

    tr.porta_key = static_cast<uint8_t>(key);
}

void Performer::MidiNoteOn(int t, Note& n, int velocity, double gain)
{
    Track& tr = tracks_[t];
    n.midi_key = n.key;
    if (velocity <= 0)
    {
        return; // silent in nw::snd; MIDI velocity 0 would be a note-off
    }

    tr.legato_gain = gain;
    tr.legato_bend = 0.0f;
    UpdateOutputs(t); // the controllers, and the pitch of a sweeping note, come first

    if (tr.lfo_type == 2 && tr.lfo_depth != 0 && tr.lfo_delay >= 0)
    {
        Approximate("pan LFO (omitted from SoundFont)");
    }

    const GameFilters filters = TrackFilters(tr.low_pass, tr.biquad_type, tr.biquad_value);
    if (filters.low_pass >= 0)
    {
        Approximate("low-pass filter (one-pole in the game, two-pole in the SoundFont)");
    }
    if (filters.biquad_type >= 2)
    {
        Approximate("biquad high-pass or band-pass (omitted from SoundFont)");
    }
    if (filters.low_pass >= 0 && filters.biquad_type == 1)
    {
        Approximate("combined low-pass filters (SoundFont uses the lower cutoff)");
    }
    if (filters.biquad_type != 0 && filters.biquad_type != out_.biquad_types[t])
    {
        Approximate("biquad type change (SFZ supports one type per track)");
    }

    const int preset = PresetIndex(NoteKey(t, n));
    if (preset != tr.out_preset)
    {
        tr.out_preset = preset;
        TrackEvent ev{0, EventKind::kPreset};
        ev.preset = static_cast<uint32_t>(preset);
        Emit(t, ev);
    }

    // Assign IDs to overlapping notes of the same key so the MIDI writer can distribute them across channels.
    n.midi_on = true;
    n.midi_velocity = static_cast<uint8_t>(std::min(velocity, 127));
    n.midi_id = next_midi_id_++;
    TrackEvent ev{0, EventKind::kNoteOn, n.midi_key, n.midi_velocity};
    ev.note = n.midi_id;
    Emit(t, ev);
}

void Performer::NoteOn(int t, int key, int velocity, int32_t length)
{
    // SequenceTrack::NoteOn (0x31e2a4).
    Track& tr = tracks_[t];

    // At most kVoices new channels can sound in one tick, fewer with stereo waves. Cap new channels to avoid thousands
    // of MIDI notes from loops without waits. Legatos above the same cap continue their existing MIDI note.
    if (tr.note_tick != tick_)
    {
        tr.note_tick = tick_;
        tr.tick_notes = 0;
        tr.tick_legatos = 0;
    }

    const int vel = velocity * tr.velocity_range / 127;
    if (!tr.played)
    {
        tr.played = true;
        UpdateOutputs(t);
    }

    // Tie/mono continue the newest channel without another attack (see Legato for MIDI handling).
    if (!tr.notes.empty() && (tr.tie || tr.mono))
    {
        Note& head = tr.notes.back();
        if (!tr.mono || !head.released)
        {
            Legato(t, head, key, vel, length, ++tr.tick_legatos > kVoices);
            return;
        }

        StopNote(t, head); // nw::snd stops a releasing channel and starts a new one
        Ended(head);
        tr.notes.pop_back();
    }

    // Bank::NoteOn (0x48f954) finds the new channel's region and wave, or leaves it silent.
    const VelocityRegion* region = banks_.FindRegion(tr.bank_slot, tr.program, key, vel);
    if (!region)
    {
        return;
    }

    const Pcm* pcm = banks_.GetWave(tr.bank_slot, region->wave_id_index);
    if (!pcm)
    {
        return;
    }

    if (++tr.tick_notes > kVoices)
    {
        Approximate(
            "more than 24 new channels on a track in one tick (excess omitted; the game keeps the "
            "newest)");
        return;
    }

    // Use nw::snd's default pan curve and dual mode for stereo waves.
    if (vel > 0 && (sound_.pan_curve != 0 || (sound_.pan_mode != 0 && pcm->channels.size() > 1)))
    {
        Approximate("custom pan curve or mode (using defaults)");
    }

    // nw::snd adds region pan, track initial pan and track pan before applying its pan law. The synth receives the
    // first two as zone pan and the third as CC10, then combines them using its own law.
    if (vel > 0 && region->pan + tr.init_pan != 64 && tr.pan.Get() != 0)
    {
        Approximate("combined region and track pan (using the synth pan law)");
    }

    // The DSP uses the region's interpolation mode. Synth interpolation most closely matches the default mode.
    if (vel > 0 && region->interpolation == 1)
    {
        Approximate("linear sample interpolation (using synth interpolation)");
    }
    else if (vel > 0 && region->interpolation == 2)
    {
        Approximate("uninterpolated sample playback (using synth interpolation)");
    }

    Note n;
    n.key = static_cast<uint8_t>(key);
    n.length = tr.tie ? -1 : length;
    n.on_frame = frame_;
    n.ignore_note_off = region->ignore_note_off;
    n.key_group = region->key_group;
    n.bank_slot = tr.bank_slot;
    n.program = tr.program;
    n.init_pan = tr.init_pan;
    n.env = OverrideEnvelope(
        {region->adshr.attack, region->adshr.decay, region->adshr.sustain, region->adshr.hold, region->adshr.release},
        TrackPresetKey(tr));

    if (n.ignore_note_off)
    {
        const int64_t frames = FramesToLevel(n.env, -static_cast<float>(kInaudibleSustain));
        if (frames >= 0)
        {
            n.quiet_ms = static_cast<double>(frame_ + frames + 1) * kFrameMs;
        }
    }

    n.original_key = region->original_key;
    n.tune = region->pitch;
    n.sample_rate = pcm->sample_rate;
    n.midi_volume = region->volume;
    if (!pcm->loop)
    {
        n.StartWave(frame_, pcm->Length(), n.SamplesPerFrame(static_cast<float>(key - n.original_key)));
    }

    StartSweep(t, n, key, length);

    // Release other channels in the key group at rate 126. SoundFont uses exclusiveClass for this.
    if (n.key_group != 0)
    {
        for (Note& other : tr.notes)
        {
            if (other.key_group == n.key_group)
            {
                ReleaseNote(t, other, 126);
            }
        }
    }

    tr.notes.push_back(n);
    MidiNoteOn(t, tr.notes.back(), vel);
}

void Performer::Legato(int t, Note& n, int key, int velocity, int32_t length, bool past_cap)
{
    Track& tr = tracks_[t];
    n.key = static_cast<uint8_t>(key);
    if (tr.mono)
    {
        n.length = length;
    }
    n.env = OverrideEnvelope(n.env, TrackPresetKey(tr));
    StartSweep(t, n, key, length);
    tr.last_change = tick_; // count the change regardless of MIDI representation, which can differ between passes

    // SequenceTrack::NoteOn sets gain from the new velocity without the region volume. The synth zone still applies
    // region volume, so compensate through CC11. A zero-volume region cannot be made audible this way.
    auto without_region_volume = [&](double gain)
    {
        if (n.midi_volume == 0 && velocity > 0)
        {
            Approximate("legato on a zero-volume region (audible in the game, silent in the synth)");
        }
        return n.midi_volume > 0 ? gain * std::sqrt(127.0 / n.midi_volume) : gain;
    };

    // Continue the MIDI note if its preset matches. CC11 handles reduced velocity and pitch bend handles key changes
    // (UpdateOutputs), preserving sample position and the original region. At velocity 0, keep the note silent through
    // CC11 regardless of preset; ending it would leave a release tail.
    //
    // After kVoices legatos in a tick, continue regardless of preset/velocity, capped at the starting velocity. Larger
    // gains could attenuate the whole sequence (Perform). Count only legatos that would otherwise need a new MIDI note
    // as approximations.
    const bool carries_on = n.midi_on && velocity <= n.midi_velocity &&
                            (velocity <= 0 || NoteKey(t, n) == out_.presets[static_cast<std::size_t>(tr.out_preset)]);
    if (carries_on || (n.midi_on && past_cap))
    {
        if (!carries_on)
        {
            Approximate(
                "more than 24 legatos in a tick (keeping the MIDI preset and limiting gain to the starting level)");
        }

        const int kept = std::clamp<int>(velocity, 0, n.midi_velocity);
        tr.legato_gain = without_region_volume(static_cast<double>(kept) / n.midi_velocity);
        tr.legato_bend = static_cast<float>(key - n.midi_key);
        return;
    }

    // MIDI cannot increase an existing note's velocity. Start a new note, selecting its region by the new key and
    // velocity.
    const VelocityRegion* played = banks_.FindRegion(n.bank_slot, n.program, key, velocity);
    n.midi_volume = played ? played->volume : 127;
    EndMidiNote(t, n);
    MidiNoteOn(t, n, velocity, without_region_volume(1.0));
    Approximate("legato (tie or mono) played as a new note");
}

void Performer::CommandProc(int t, uint32_t cmd, int32_t arg1, int32_t arg2)
{
    // MmlParser::CommandProc (0x48fad4).
    if (cmd > 0xff) // variable and comparison commands (0xf0 prefix)
    {
        VariableCommand(t, cmd & 0xff, static_cast<uint8_t>(arg1), static_cast<int16_t>(arg2));
        return;
    }

    Track& tr = tracks_[t];
    const uint8_t a8 = static_cast<uint8_t>(arg1);
    switch (cmd)
    {
    case 0x81: // prg
        if (arg1 < 0x10000)
        {
            tr.program = static_cast<uint16_t>(arg1);
        }
        break;

    case 0x88: // opentrack: a track that reached FIN was freed and can't be opened again
        if (arg1 < 16 && arg1 != t && tracks_[arg1].allocated)
        {
            CloseTrack(arg1);
            OpenTrack(arg1, static_cast<uint32_t>(arg2));
        }
        break;

    case 0x89: // jump
        if (static_cast<uint32_t>(arg1) < tr.current)
        {
            OnLoop(t, static_cast<uint32_t>(arg1));
        }
        tr.current = static_cast<uint32_t>(arg1);
        break;

    case 0x8a: // call
        if (tr.call_depth >= 3)
        {
            break;
        }

        tr.calls[tr.call_depth] = {false, 0, tr.current};
        tr.call_depth++;
        tr.current = static_cast<uint32_t>(arg1);
        break;

    case 0xb0: // timebase: 0 stops sequence timing, as tempo 0 does; MIDI retains its timebase
        timebase_ = a8;
        if (tick_ == 0 && timebase_ > 0)
        {
            out_.timebase = timebase_;
        }
        EmitTempo();
        break;

    case 0xb1:
        tr.hold = a8;
        break;

    case 0xb2: // monophonic
        tr.mono = arg1 != 0;
        if (tr.mono)
        {
            for (Note& n : tr.notes)
            {
                ReleaseNote(t, n);
            }
            DetachAll(t);
        }
        break;

    case 0xb3:
        tr.velocity_range = a8;
        break;

    case 0xb4:
        tr.biquad_type = a8;
        SetFilter(t);
        break;

    case 0xb5:
        tr.biquad_value = arg1;
        SetFilter(t);
        break;

    case 0xb6: // bank select: nw::snd accepts any slot, but only four contain banks (see BankSet)
        tr.bank_slot = a8;
        break;

    case 0xc0:
        tr.pan.SetTarget(static_cast<int8_t>(arg1 - 0x40), arg2);
        break;

    case 0xc1:
        tr.vol.SetTarget(a8, arg2);
        break;

    case 0xc2:
        main_volume_ = a8;
        break;

    case 0xc3:
        tr.transpose = static_cast<int8_t>(arg1);
        break;

    case 0xc4:
        tr.bend.SetTarget(static_cast<int8_t>(arg1), arg2);
        break;

    case 0xc5:
        tr.bend_range = a8;
        break;

    case 0xc7:
        tr.note_wait = arg1 != 0;
        break;

    case 0xc8: // tie
        tr.tie = arg1 != 0;
        for (Note& n : tr.notes)
        {
            ReleaseNote(t, n);
        }
        DetachAll(t);
        break;

    case 0xc9: // porta (from a key)
        tr.porta = true;
        tr.porta_key = static_cast<uint8_t>(static_cast<uint8_t>(tr.transpose) + arg1);
        break;

    case 0xca:
        tr.lfo_depth = a8;
        break;

    case 0xcb:
        SetLfoSetting(t, tr.lfo_speed, a8);
        break;

    case 0xcc:
        SetLfoSetting(t, tr.lfo_type, a8);
        break;

    case 0xcd:
        SetLfoSetting(t, tr.lfo_range, a8);
        break;

    case 0xce:
        tr.porta = arg1 != 0;
        break;

    case 0xcf:
        tr.porta_time = a8;
        break;

    case 0xd0:
        tr.attack = a8;
        break;

    case 0xd1:
        tr.decay = a8;
        break;

    case 0xd2:
        tr.sustain = a8;
        break;

    case 0xd3:
        tr.release = a8;
        break;

    case 0xd4: // loop start
        if (tr.call_depth < 3)
        {
            tr.calls[tr.call_depth] = {true, a8, tr.current};
            tr.call_depth++;
        }
        break;

    case 0xd5:
        tr.volume2 = a8;
        break;

    case 0xd7:
        if (arg1 != 0)
        {
            Approximate("surround pan (omitted)");
        }
        break;

    case 0xd8:
        tr.low_pass = arg1;
        SetFilter(t);
        break;

    case 0xd9:
        tr.fx_send_a = a8;
        break;

    case 0xda:
        tr.fx_send_b = a8;
        break;

    case 0xdb:
        if (a8 != 127)
        {
            Approximate("main send (omitted)");
        }
        break;

    case 0xdc:
        tr.init_pan = static_cast<int8_t>(arg1 - 0x40);
        break;

    case 0xdd:
        Mute(t, a8);
        break;

    case 0xdf:
        tr.damper = a8 >= 0x40;
        break;

    case 0xe0:
        SetLfoSetting(t, tr.lfo_delay, arg1);
        break;

    case 0xe1: // tempo
        tempo_ = std::clamp(arg1, 0, 0x3ff);
        EmitTempo();
        break;

    case 0xe3:
        tr.sweep_pitch = static_cast<float>(arg1) * 0.015625f;
        break;

    case 0xfb: // envelope reset
        tr.attack = tr.decay = tr.sustain = tr.release = 0xff;
        tr.hold = 0xff;
        break;

    case 0xfc:
        LoopEnd(t);
        break;

    case 0xfd: // return
        while (tr.call_depth != 0)
        {
            tr.call_depth--;
            if (!tr.calls[tr.call_depth].is_loop)
            {
                tr.current = tr.calls[tr.call_depth].address;
                break;
            }
        }
        break;

    default:
        // 0xbf front bypass, 0xc6 priority, 0xd6 printvar: nothing to record.
        break;
    }
}

void Performer::VariableCommand(int t, uint32_t op, uint8_t index, int16_t v)
{
    int16_t* const var = Variable(t, index);
    if (!var)
    {
        return;
    }

    Track& tr = tracks_[t];
    switch (op)
    {
    case 0x80:
        *var = v;
        break;

    case 0x81:
        *var = static_cast<int16_t>(*var + v);
        break;

    case 0x82:
        *var = static_cast<int16_t>(*var - v);
        break;

    case 0x83:
        *var = static_cast<int16_t>(*var * v);
        break;

    case 0x84:
        if (v != 0)
        {
            *var = static_cast<int16_t>(*var / v);
        }
        break;

    case 0x85: // ARM register shifts use the low byte of the amount
        if (v >= 0)
        {
            const uint32_t n = static_cast<uint32_t>(v) & 0xff;
            *var = static_cast<int16_t>(n >= 32 ? 0u : static_cast<uint32_t>(*var) << n);
        }
        else
        {
            const uint32_t n = static_cast<uint32_t>(-v) & 0xff;
            const int32_t x = *var;
            *var = static_cast<int16_t>(n >= 32 ? (x < 0 ? -1 : 0) : x >> n);
        }
        break;

    case 0x86:
        {
            int32_t range = v;
            const bool neg = range < 0;
            if (neg)
            {
                range = static_cast<int16_t>(-v);
            }

            const int32_t r =
                static_cast<int32_t>(static_cast<uint32_t>(rng_.Next()) * static_cast<uint32_t>(range + 1)) >> 16;
            *var = static_cast<int16_t>(neg ? -r : r);
            break;
        }

    case 0x87:
        *var = static_cast<int16_t>(*var & v);
        break;

    case 0x88:
        *var = static_cast<int16_t>(*var | v);
        break;

    case 0x89:
        *var = static_cast<int16_t>(*var ^ v);
        break;

    case 0x8a:
        *var = static_cast<int16_t>(~v);
        break;

    case 0x8b:
        if (v != 0)
        {
            *var = static_cast<int16_t>(*var % v);
        }
        break;

    case 0x90:
        tr.cmp_flag = *var == v;
        break;

    case 0x91:
        tr.cmp_flag = *var >= v;
        break;

    case 0x92:
        tr.cmp_flag = *var > v;
        break;

    case 0x93:
        tr.cmp_flag = *var <= v;
        break;

    case 0x94:
        tr.cmp_flag = *var < v;
        break;

    case 0x95:
        tr.cmp_flag = *var != v;
        break;

    default:
        break;
    }
}

void Performer::Mute(int t, uint8_t mode)
{
    // As the game does it (code.bin 0x31e5cc).
    Track& tr = tracks_[t];
    switch (mode)
    {
    case 0:
        tr.mute = false;
        break;

    case 1:
        tr.mute = true;
        break;

    case 2: // release the channels
        for (Note& n : tr.notes)
        {
            ReleaseNote(t, n);
        }
        DetachAll(t);
        tr.mute = true;
        break;

    case 3: // stop the channels
        {
            bool sounding = false;
            for (Note& n : tr.notes)
            {
                sounding = sounding || (n.EndMs() > now_ms_ && !Unheard(n));
                EndMidiNote(t, n);
                n.released = true;
                n.release_end_ms = now_ms_;
                Ended(n);
            }
            if (sounding)
            {
                Emit(t, {0, EventKind::kSoundOff});
            }

            tr.notes.clear();
            tr.mute = true;
            break;
        }

    default:
        break;
    }
}

void Performer::LoopEnd(int t)
{
    Track& tr = tracks_[t];
    if (tr.call_depth == 0)
    {
        return;
    }

    CallEntry& e = tr.calls[tr.call_depth - 1];
    if (!e.is_loop)
    {
        return;
    }

    // Only a count of 0 loops indefinitely and can become the main loop.
    if (e.count == 0)
    {
        OnLoop(t, e.address);
    }
    else if (--e.count == 0)
    {
        tr.call_depth--;
        return;
    }

    tr.current = e.address;
}

void Performer::OnLoop(int t, uint32_t target)
{
    // A backward jump to unvisited commands is not a loop; shared code may precede its callers. Also ignore passes
    // without waits, such as random retries or variable polling (ParseNextTick).
    const auto run = last_run_[t].find(target);
    const std::optional<uint32_t> last_wait = tracks_[t].last_wait;
    if (run == last_run_[t].end() || !last_wait || *last_wait < run->second)
    {
        return;
    }

    // Use the lowest-numbered track that can still advance: track 0 while active, or a later track for continuing sound
    // effects. A pass counts only if no earlier track contributed during it, so the marked section contains only
    // repeating material.
    const uint32_t start = run->second;
    for (int i = 0; i < t; i++)
    {
        const bool plays_on = tracks_[i].open && !WaitsForever(i);
        const bool in_pass = tracks_[i].last_change && *tracks_[i].last_change >= start;
        if (plays_on || in_pass)
        {
            return;
        }
    }

    out_.loops++;
    if (!out_.loop)
    {
        out_.loop = Performance::Loop{start, tick_};
    }

    if (out_.loops >= options_.loops && now_ms_ >= kMinLoopSeconds * 1000.0)
    {
        stop_ = true;
    }
}

bool Performer::Parse(int t)
{
    // MmlParser::Parse (0x49036c).
    Track& tr = tracks_[t];
    last_run_[t][tr.current] = tick_;

    // Prefixes specify the condition (if), timed-command duration encoding, and main argument encoding.
    ArgType arg_type = kArgNone;
    ArgType time_type = kArgNone;
    bool cond = true;
    uint32_t cmd = ReadByte(t);
    if (cmd == 0xa2) // if
    {
        cmd = ReadByte(t);
        cond = tr.cmp_flag;
    }

    if (cmd == 0xa3)
    {
        time_type = kArgS16;
        cmd = ReadByte(t);
    }
    else if (cmd == 0xa4)
    {
        time_type = kArgRandom;
        cmd = ReadByte(t);
    }
    else if (cmd == 0xa5)
    {
        time_type = kArgVariable;
        cmd = ReadByte(t);
    }

    if (cmd == 0xa0)
    {
        cmd = ReadByte(t);
        arg_type = kArgRandom;
    }
    else if (cmd == 0xa1)
    {
        cmd = ReadByte(t);
        arg_type = kArgVariable;
    }

    // Read the main argument using the prefix encoding, or `encoding` by default.
    auto arg = [&](ArgType encoding)
    {
        return ReadArg(t, arg_type != kArgNone ? arg_type : encoding);
    };

    if ((cmd & 0x80) == 0) // note: key, velocity, length
    {
        const uint8_t velocity = ReadByte(t);
        const int32_t length = arg(kArgVmidi);
        if (!cond)
        {
            return false;
        }

        const int key = std::clamp(static_cast<int>(tr.transpose) + static_cast<int>(cmd), 0, 127);
        if (!tr.mute)
        {
            NoteOn(t, key, velocity, length > 0 ? length : -1);
        }

        if (tr.note_wait)
        {
            tr.wait = length;
            if (length == 0)
            {
                tr.note_finish_wait = true;
            }
        }

        return false;
    }

    switch ((cmd & 0xf0) >> 4)
    {
    case 0x8:
        switch (cmd)
        {
        case 0x80: // wait
            {
                const int32_t w = arg(kArgVmidi);
                if (cond)
                {
                    tr.wait = w;
                }
                return false;
            }

        case 0x81: // prg
            {
                const int32_t prg = arg(kArgVmidi);
                if (cond)
                {
                    CommandProc(t, cmd, prg, 0);
                }
                return false;
            }

        case 0x88: // opentrack
            {
                const uint32_t track_no = ReadByte(t);
                uint32_t off = ReadByte(t);
                off = (off << 8) | ReadByte(t);
                off = (off << 8) | ReadByte(t);
                if (cond)
                {
                    CommandProc(t, cmd, static_cast<int32_t>(track_no), static_cast<int32_t>(off));
                }
                return false;
            }

        case 0x89:
        case 0x8a: // jump, call
            {
                uint32_t off = ReadByte(t);
                off = (off << 8) | ReadByte(t);
                off = (off << 8) | ReadByte(t);
                if (cond)
                {
                    CommandProc(t, cmd, static_cast<int32_t>(off), 0);
                }
                return false;
            }

        default:
            return false;
        }

    case 0x9:
        if (cond)
        {
            CommandProc(t, cmd, 0, 0);
        }
        return false;

    case 0xa:
        return false;

    case 0xb:
    case 0xc:
    case 0xd:
        {
            const uint32_t v = static_cast<uint32_t>(arg(kArgU8)) & 0xff;
            int32_t time = 0;
            if (time_type != kArgNone)
            {
                time = ReadArg(t, time_type);
            }
            if (!cond)
            {
                return false;
            }

            const int32_t a1 = (cmd == 0xc3 || cmd == 0xc4) ? static_cast<int8_t>(v) : static_cast<int32_t>(v);
            CommandProc(t, cmd, a1, time);
            return false;
        }

    case 0xe:
        {
            const int32_t v = static_cast<int16_t>(arg(kArgS16));
            if (cond)
            {
                CommandProc(t, cmd, v, 0);
            }
            return false;
        }

    case 0xf:
        if (cmd == 0xf0)
        {
            const uint32_t sub = ReadByte(t);
            const uint32_t kind = sub & 0xf0;
            if (kind == 0x80 || kind == 0x90)
            {
                const uint32_t var_no = ReadByte(t);
                const int32_t v = static_cast<int16_t>(arg(kArgS16));
                if (cond)
                {
                    CommandProc(t, 0xf000 + sub, static_cast<int32_t>(var_no), v);
                }
                return false;
            }

            if (kind == 0xe0)
            {
                arg(kArgS16); // userproc calls into the game; consume its argument
            }
            return false;
        }

        if (cmd == 0xfe) // alloctrack: handled at the start
        {
            ReadByte(t);
            ReadByte(t);
            return false;
        }

        if (cmd == 0xff) // fin
        {
            return cond;
        }

        if (cond)
        {
            CommandProc(t, cmd, 0, 0);
        }
        return false;

    default:
        return false;
    }
}

bool Performer::ParseNextTick(int t)
{
    // SequenceTrack::ParseNextTick (0x31dbf4).
    Track& tr = tracks_[t];
    tr.vol.Update();
    tr.pan.Update();
    tr.bend.Update();

    if (tr.note_finish_wait)
    {
        if (TrackBusy(t))
        {
            return false;
        }

        tr.note_finish_wait = false;
    }

    if (tr.wait > 0)
    {
        tr.wait--;
        if (tr.wait > 0)
        {
            return false;
        }
    }

    // nw::snd yields after kParseLimit commands without a wait and resumes next tick. Tracks can poll variables set by
    // other tracks this way, playing a note on each update. Stop a track still running after kHoldSeconds.
    // Note-producing loops also obey the per-tick kVoices cap (NoteOn).
    int count = 0;
    while (tr.wait == 0 && !tr.note_finish_wait && !stop_)
    {
        if (++count > kParseLimit)
        {
            if (tr.running_since_ms < 0)
            {
                tr.running_since_ms = now_ms_;
            }
            if (now_ms_ - tr.running_since_ms < kHoldSeconds * 1000.0)
            {
                return false;
            }

            out_.warnings.push_back("track " + std::to_string(t) + " stopped after running without waits");
            return true;
        }
        if (Parse(t))
        {
            return true;
        }
    }

    tr.running_since_ms = -1.0; // it waits again
    tr.last_wait = tick_;

    return false;
}

void Performer::AllocateTracks()
{
    for (int t = 0; t < 16; t++)
    {
        tracks_[t].Init();
        tracks_[t].allocated = (sound_.allocate_track_flags >> t) & 1;
    }
    tracks_[0].allocated = true;

    // A leading alloctrack (0xfe, 16-bit mask) also allocates tracks. The start offset can be 0xffffffff, so guard
    // against addition wrapping.
    const std::size_t start = sound_.start_offset;
    if (start < seq_.data.size() && seq_.data.size() - start > 2 && seq_.data[start] == 0xfe)
    {
        const uint32_t mask = (seq_.data[start + 1] << 8) | seq_.data[start + 2];
        for (int t = 0; t < 16; t++)
        {
            if ((mask >> t) & 1)
            {
                tracks_[t].allocated = true;
            }
        }
    }
}

Performance Performer::Run()
{
    AllocateTracks();
    OpenTrack(0, sound_.start_offset);
    EmitTempo();
    RecordTickTime();

    bool finished = false;
    double steady_since = -1.0;
    for (;;)
    {
        // SoundThread::FrameProcess advances the PRNG after the players, once per frame. Random arguments therefore
        // depend on timing. Process each tick in the frame containing its timestamp.
        const uint64_t frame = frame_;
        for (; rng_frame_ < frame; rng_frame_++)
        {
            rng_.Next();
        }

        // Update all frames since the previous tick, including its frame.
        UpdateOneShots(frame);

        bool any_open = false;
        for (int t = 0; t < 16 && !stop_; t++)
        {
            Track& tr = tracks_[t];
            if (!tr.open)
            {
                continue;
            }

            UpdateNoteLengths(t);
            if (ParseNextTick(t))
            {
                CloseTrack(t);
                tr.allocated = false; // the player frees a finished track
                continue;
            }

            any_open = true;
        }
        if (stop_)
        {
            break;
        }

        for (int t = 0; t < 16; t++)
        {
            UpdateOutputs(t);
            if (tracks_[t].played && tracks_[t].open && TrackBusy(t))
            {
                level_peak_ = std::max(level_peak_, TrackLevel(t) * tracks_[t].legato_gain);
            }
            if (!tracks_[t].open)
            {
                PruneNotes(t);
            }
        }

        if (!any_open)
        {
            finished = true;
            break;
        }

        if (steady_since < 0 && Steady())
        {
            steady_since = now_ms_;
        }
        if (steady_since >= 0 && now_ms_ - steady_since >= kHoldSeconds * 1000.0)
        {
            out_.holds = true;
            break;
        }

        if (now_ms_ >= kMaxSeconds * 1000.0)
        {
            out_.truncated = true;
            break;
        }

        if (!NextTick())
        {
            out_.warnings.push_back(timebase_ == 0 ? "timebase 0: the sequence stops advancing"
                                                   : "tempo 0: the sequence stops advancing");
            PlayOutStopped();
            break;
        }

        tick_++;
        RecordTickTime();
    }

    EndPerformance(finished);

    return out_;
}

bool Performer::NextTick()
{
    // SequenceSoundPlayer::UpdateTick (code.bin 0x31ff50) schedules ticks within each frame in thousandths of ARM
    // cycles, using a ticks-per-millisecond rate. Carry the remainder to the next frame as a fraction of a tick. All
    // arithmetic is single precision. Rounding causes a few microseconds of drift per minute, occasionally pushing a
    // boundary tick into the next frame.
    const float per_ms = static_cast<float>(timebase_ * tempo_) * (1.0f / 60000.0f);
    if (per_ms == 0.0f)
    {
        return false;
    }

    uint64_t next = static_cast<uint64_t>(kArmClock / per_ms);
    while (next >= frame_rest_)
    {
        const float fraction = static_cast<float>(next - frame_rest_) * per_ms * (1.0f / kArmClock);
        frame_++;
        frame_rest_ = kFrameMilliCycles;
        next = static_cast<uint64_t>(fraction * kArmClock / per_ms);
    }

    frame_rest_ -= next;
    const uint64_t milli_cycles = frame_ * kFrameMilliCycles + (kFrameMilliCycles - frame_rest_);
    now_ms_ = static_cast<double>(milli_cycles) / kArmClock;

    return true;
}

void Performer::PlayOutStopped()
{
    // Finish one-shot waves using their per-frame pitch, as in EndPerformance.
    UpdateOneShots(FrameOf(now_ms_ + kMaxTailMs) + 1);

    double end_ms = now_ms_;
    for (const Track& tr : tracks_)
    {
        for (const auto* list : {&tr.notes, &tr.detached})
        {
            for (const Note& n : *list)
            {
                out_.holds = out_.holds || n.EndMs() >= kNever;
                end_ms = std::max(end_ms, std::min(n.EndMs(), now_ms_ + kMaxTailMs));
            }
        }
    }
    if (out_.holds)
    {
        end_ms = now_ms_ + kHoldSeconds * 1000.0;
    }

    // MIDI ticks continue after sequence timing stops. Millisecond sweeps keep moving; note-length sweeps remain frozen
    // with the sequence's ticks (UpdateOutputs).
    const double ticks_per_second = out_.tempo.back().bpm * out_.timebase / 60.0;
    const auto ticks = static_cast<uint32_t>(std::ceil((end_ms - now_ms_) / 1000.0 * ticks_per_second));
    const double start_ms = now_ms_;
    for (uint32_t i = 1; i <= ticks; i++)
    {
        tick_++;
        now_ms_ = start_ms + i / ticks_per_second * 1000.0;
        frame_ = FrameOf(now_ms_);
        for (int t = 0; t < 16; t++)
        {
            PruneNotes(t); // finished notes have no active sweep
            UpdateOutputs(t);
        }
    }
}

void Performer::EndPerformance(bool finished)
{
    // Stop immediately at a loop boundary, time limit or held-sound limit. A naturally finished sequence ends with its
    // final sound, including release tails; nw::snd can leave finished tracks waiting long after silence. Continue
    // one-shot waves with their per-frame pitch.
    if (finished)
    {
        UpdateOneShots(FrameOf(now_ms_ + kMaxTailMs) + 1);
    }

    for (int t = 0; t < 16; t++)
    {
        Track& tr = tracks_[t];
        for (auto* list : {&tr.notes, &tr.detached})
        {
            for (Note& n : *list)
            {
                if (finished)
                {
                    sound_end_ms_ = std::max(sound_end_ms_, std::min(n.EndMs(), now_ms_ + kMaxTailMs));
                }
                MidiNoteOff(t, n);
            }
            list->clear();
        }
    }

    if (finished)
    {
        uint32_t last_event = 0;
        for (const auto& events : out_.tracks)
        {
            if (!events.empty())
            {
                last_event = std::max(last_event, events.back().tick);
            }
        }
        if (!out_.tempo.empty())
        {
            last_event = std::max(last_event, out_.tempo.back().tick);
        }

        out_.end_tick = std::max(last_event, MsToTick(sound_end_ms_));
        out_.seconds = std::max(sound_end_ms_, tick_ms_[last_event]) / 1000.0;
    }
    else
    {
        out_.end_tick = tick_;
        out_.seconds = now_ms_ / 1000.0;
    }
}

uint32_t Performer::MsToTick(double ms) const
{
    const auto it = std::lower_bound(tick_ms_.begin(), tick_ms_.end(), ms);
    if (it != tick_ms_.end())
    {
        return static_cast<uint32_t>(it - tick_ms_.begin());
    }

    // Extrapolate at the last tempo, or the MIDI tempo if sequence timing stopped.
    const double per_tick = MsPerTick() > 0 ? MsPerTick() : 60000.0 / (out_.tempo.back().bpm * out_.timebase);
    return static_cast<uint32_t>(tick_ms_.size() - 1 + std::ceil((ms - tick_ms_.back()) / per_tick));
}

// Scale factor that keeps rounded CC11 values within 127 for a maximum unscaled value of `peak` (Perform).
double LevelScale(double peak)
{
    return peak >= 127.5 ? 127.0 / peak : 1.0;
}

} // namespace

EnvelopeValues OverrideEnvelope(EnvelopeValues env, const PresetKey& key)
{
    if (key.attack <= 127)
    {
        env.attack = key.attack;
    }
    if (key.decay <= 127)
    {
        env.decay = key.decay;
    }
    if (key.sustain <= 127)
    {
        env.sustain = key.sustain;
    }
    if (key.release <= 127)
    {
        env.release = key.release;
    }
    if (key.hold <= 127)
    {
        env.hold = key.hold;
    }

    return env;
}

Performance Perform(const SoundArchive& archive, uint32_t sound_index, BankSet& banks, const PerformOptions& options)
{
    const SoundInfo& sound = archive.Sounds()[sound_index];
    const auto file = archive.FileData(sound.file_id);
    if (file.empty() && !archive.FileMissing(sound.file_id))
    {
        throw FormatError("sequence file is not stored in the archive");
    }
    if (file.empty())
    {
        throw FormatError(archive.Truncated() ? "sequence extends past the truncated archive"
                                              : "sequence extends past the archive");
    }

    const Sequence seq = Sequence::Parse(file);
    Performer performer(sound, seq, banks, options, {}, 1.0);
    Performance performance = performer.Run();

    // Rerun sequences whose filters change during notes, moving those filters from presets to controllers. Also rerun
    // if rounded CC11 exceeds 127, scaling every value equally to preserve track balance.
    //
    // Only MIDI events and presets change between passes. A controller-driven filter may allow a legato to continue its
    // MIDI note on the second pass (Legato), changing the peak CC11. If so, run a third pass with the corrected scale.
    double peak = performer.LevelPeak();
    if (performer.FilterMoves() != std::array<bool, 16>{} || LevelScale(peak) < 1.0)
    {
        Performer again(sound, seq, banks, options, performer.FilterMoves(), LevelScale(peak));
        performance = again.Run();
        if (LevelScale(again.LevelPeak()) != LevelScale(peak))
        {
            peak = again.LevelPeak();
            Performer third(sound, seq, banks, options, performer.FilterMoves(), LevelScale(peak));
            performance = third.Run();
        }
    }

    if (LevelScale(peak) < 1.0)
    {
        // CC11 acts squared, so scaling it by 127 / peak makes every note 40 log10(peak / 127) dB quieter.
        performance.level_cut_db = 40.0 * std::log10(peak / 127.0);
        char what[160];
        std::snprintf(what, sizeof(what),
                      "volume exceeds MIDI range (SoundFont playback attenuated by %.1f dB; "
                      "SFZ restores the gain)",
                      performance.level_cut_db);
        performance.approximations[what] = NoteCount(performance);
    }

    return performance;
}

uint32_t NoteCount(const Performance& perf)
{
    uint32_t notes = 0;
    for (const std::vector<TrackEvent>& events : perf.tracks)
    {
        for (const TrackEvent& e : events)
        {
            if (e.kind == EventKind::kNoteOn)
            {
                notes++;
            }
        }
    }

    return notes;
}

} // namespace citrusf2
