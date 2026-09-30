// SPDX-License-Identifier: MIT

// Standard MIDI File writer (see midi.h).

#include "midi.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <initializer_list>
#include <map>
#include <utility>

namespace citrusf2
{

namespace
{

// General MIDI drum channel (channel 10).
constexpr int kDrumChannel = 9;

// The order in which tracks get MIDI channels. The drum channel comes last.
constexpr int kChannelOrder[16] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 10, 11, 12, 13, 14, 15, kDrumChannel};

// Minimum MIDI ticks per quarter note. Events are rounded to the nearest MIDI tick at the start of a game sound frame
// (see EventTick). At 120 BPM the error is at most 1/30 ms, about one DSP sample at 32728 Hz. This preserves the phase
// relationship of rapid repeated notes in players with exact event timing.
//
// FluidSynth also rounds its position to a tick at each tempo change, then restarts its clock there. Each change can
// advance the rest of the file by half a tick. At the sequence's original 48 or 96 ticks per quarter note, the error
// can be about 5 ms per change at 120 BPM, accumulating to over a second during gradual tempo changes.
constexpr uint32_t kMinDivision = 7680;

// Return the smallest integer MIDI ticks per performance tick that meets kMinDivision.
uint32_t TickScale(const Performance& perf)
{
    const uint32_t timebase = std::max<uint32_t>(perf.timebase, 1);
    return (kMinDivision + timebase - 1) / timebase;
}

// Map performance tick `tick` to a MIDI tick. nw::snd processes ticks within sound frames; the DSP applies their
// changes at the next frame boundary. Place MIDI events at the start of their tick's frame (Performance::frame_ticks),
// rounded to the nearest MIDI tick. The sequence is one frame early, but event spacing is preserved. Clamp at loop
// markers to keep events in the correct loop.
//
// Use 64 bits: MIDI tick positions can exceed 32 bits if a sequence starts with a small timebase and later increases
// it.
uint64_t EventTick(const Performance& perf, uint32_t tick)
{
    const uint64_t scale = TickScale(perf);
    uint64_t midi_tick = tick * scale;
    if (tick < perf.frame_ticks.size())
    {
        midi_tick = std::min(midi_tick, static_cast<uint64_t>(std::llround(perf.frame_ticks[tick] * scale)));
    }
    if (perf.loop && perf.loop->end > perf.loop->start)
    {
        for (const uint32_t marker : {perf.loop->start, perf.loop->end})
        {
            if (tick >= marker)
            {
                midi_tick = std::max(midi_tick, marker * scale);
            }
        }
    }

    return midi_tick;
}

// Build an MTrk chunk from events and delta times, in MIDI ticks.
class TrackWriter
{
public:
    void Event(uint64_t tick, std::initializer_list<uint8_t> bytes)
    {
        Delta(tick);
        data_.insert(data_.end(), bytes.begin(), bytes.end());
    }

    void Meta(uint64_t tick, uint8_t type, const std::string& text)
    {
        Delta(tick);
        data_.push_back(0xff);
        data_.push_back(type);
        Vlq(static_cast<uint32_t>(text.size()));
        data_.insert(data_.end(), text.begin(), text.end());
    }

    // A tempo change, which MIDI gives in microseconds per quarter note.
    void Tempo(uint64_t tick, double bpm)
    {
        const uint32_t us = static_cast<uint32_t>(std::llround(std::clamp(60000000.0 / bpm, 1.0, 16777215.0)));
        Event(tick, {0xff, 0x51, 0x03, static_cast<uint8_t>(us >> 16), static_cast<uint8_t>(us >> 8),
                     static_cast<uint8_t>(us)});
    }

    // End the track at `tick`, or after the last event if it is later.
    void End(uint64_t tick)
    {
        Event(tick, {0xff, 0x2f, 0x00});
    }

    void AppendTo(std::vector<uint8_t>& out) const
    {
        out.insert(out.end(), {'M', 'T', 'r', 'k'});
        const uint32_t size = static_cast<uint32_t>(data_.size());
        out.insert(out.end(), {static_cast<uint8_t>(size >> 24), static_cast<uint8_t>(size >> 16),
                               static_cast<uint8_t>(size >> 8), static_cast<uint8_t>(size)});
        out.insert(out.end(), data_.begin(), data_.end());
    }

private:
    // Largest delta time that fits MIDI's four-byte variable-length encoding.
    static constexpr uint32_t kMaxDelta = 0x0fffffff;

    // Write the delta from the previous event, clamping backwards times to that event's tick. A sequence that jumps
    // back and then finishes can end before its loop-end marker. Split oversized deltas with empty text events.
    void Delta(uint64_t tick)
    {
        const uint64_t midi_tick = std::max(tick, last_);
        uint64_t delta = midi_tick - last_;
        while (delta > kMaxDelta)
        {
            Vlq(kMaxDelta);
            data_.insert(data_.end(), {0xff, 0x01, 0x00});
            delta -= kMaxDelta;
        }

        Vlq(static_cast<uint32_t>(delta));
        last_ = midi_tick;
    }

    // A variable-length quantity: 7 bits a byte, most significant first, with the top bit set on all but the last.
    void Vlq(uint32_t v)
    {
        uint8_t buf[5];
        int n = 0;
        buf[n++] = v & 0x7f;
        while (v >>= 7)
        {
            buf[n++] = static_cast<uint8_t>(0x80 | (v & 0x7f));
        }

        while (n)
        {
            data_.push_back(buf[--n]);
        }
    }

    std::vector<uint8_t> data_;
    uint64_t last_ = 0; // the last event's MIDI tick
};

// Count notes per layer with no channel limit. Layer i holds notes whose key is already sounding on layers 0..i-1.
std::vector<uint32_t> LayerDemand(const std::vector<TrackEvent>& events)
{
    std::vector<uint32_t> demand;
    std::array<std::vector<uint32_t>, 128> held{}; // per key: note id per layer, 0: free
    std::map<uint32_t, std::pair<uint8_t, std::size_t>> where;
    for (const TrackEvent& e : events)
    {
        const uint8_t key = e.key;
        if (e.kind == EventKind::kNoteOn)
        {
            std::vector<uint32_t>& layers = held[key];
            std::size_t l = 0;
            while (l < layers.size() && layers[l] != 0)
            {
                l++;
            }
            if (l == layers.size())
            {
                layers.push_back(0);
            }

            layers[l] = e.note;
            where[e.note] = {key, l};

            if (demand.size() <= l)
            {
                demand.resize(l + 1, 0);
            }
            demand[l]++;
        }
        else if (e.kind == EventKind::kNoteOff)
        {
            const auto it = where.find(e.note);
            held[it->second.first][it->second.second] = 0;
            where.erase(it);
        }
    }

    return demand;
}

// Give each active track one MIDI channel. Allocate remaining channels one at a time to the track whose next layer
// would save the most notes from being cut short.
std::array<std::vector<int>, 16> AssignChannels(const Performance& perf)
{
    std::array<std::vector<uint32_t>, 16> demand;
    for (int t = 0; t < 16; t++)
    {
        demand[t] = LayerDemand(perf.tracks[t]);
    }

    std::array<std::vector<int>, 16> channels;
    int next = 0;
    for (int t = 0; t < 16; t++)
    {
        if (!demand[t].empty())
        {
            channels[t].push_back(kChannelOrder[next++]);
        }
    }

    while (next < 16)
    {
        int best = -1;
        uint32_t best_demand = 0;
        for (int t = 0; t < 16; t++)
        {
            const std::size_t l = channels[t].size();
            if (l > 0 && l < demand[t].size() && demand[t][l] > best_demand)
            {
                best = t;
                best_demand = demand[t][l];
            }
        }
        if (best < 0)
        {
            break;
        }

        channels[best].push_back(kChannelOrder[next++]);
    }

    return channels;
}

// Write the conductor track: title, tempo and loop markers. CC111 marks loop start for RPG Maker players; "loopStart"
// and "loopEnd" serve other players.
TrackWriter ConductorTrack(const Performance& perf, const std::string& title,
                           const std::array<std::vector<int>, 16>& channels)
{
    const uint64_t scale = TickScale(perf);
    TrackWriter conductor;
    conductor.Meta(0, 0x03, title);

    int first_channel = -1;
    for (const auto& c : channels)
    {
        if (!c.empty() && (first_channel < 0 || c[0] < first_channel))
        {
            first_channel = c[0];
        }
    }

    std::size_t tempo_index = 0;
    auto flush_tempo = [&](uint32_t until)
    {
        while (tempo_index < perf.tempo.size() && perf.tempo[tempo_index].tick <= until)
        {
            conductor.Tempo(perf.tempo[tempo_index].tick * scale, perf.tempo[tempo_index].bpm);
            tempo_index++;
        }
    };

    if (perf.loop && perf.loop->end > perf.loop->start)
    {
        flush_tempo(perf.loop->start);
        conductor.Meta(perf.loop->start * scale, 0x06, "loopStart");
        if (first_channel >= 0)
        {
            conductor.Event(perf.loop->start * scale, {static_cast<uint8_t>(0xb0 | first_channel), 111, 0});
        }

        flush_tempo(perf.loop->end);
        conductor.Meta(perf.loop->end * scale, 0x06, "loopEnd");
    }

    flush_tempo(0xffffffff);
    conductor.End(perf.end_tick * scale);

    return conductor;
}

// Choose track `t`'s pitch bend range (RPN 0), up to MIDI's 127-semitone limit. The wheel has 8192 downward steps but
// only 8191 upward steps, so the range must exceed the largest positive bend. Warn in `report` if the track needs more.
int BendRange(const Performance& perf, int t, MidiReport& report)
{
    double needed = 0.0;
    for (const TrackEvent& e : perf.tracks[t])
    {
        if (e.kind == EventKind::kPitch)
        {
            needed = std::max(needed, e.semitones > 0.0f ? e.semitones * 8192.0 / 8191.0 : -e.semitones);
        }
    }

    int range = 2;
    if (needed > 2.0)
    {
        range = static_cast<int>(std::ceil(needed - 1e-4));
    }
    if (range > 127)
    {
        report.warnings.push_back("track " + std::to_string(t) + " bends up to " + std::to_string(range) +
                                  " semitones; MIDI allows 127");
        range = 127;
    }

    return range;
}

// Write one MIDI track per layer of a sequence track. Assign notes to the first layer with a free key. If all layers
// have that key in use, cut its oldest note and record the loss in `report`.
std::vector<TrackWriter> TrackLayers(const Performance& perf, int t, const std::vector<int>& chans, MidiReport& report)
{
    const std::size_t layers = chans.size();
    std::vector<TrackWriter> w(layers);
    for (std::size_t l = 0; l < layers; l++)
    {
        w[l].Meta(0, 0x03, "Track " + std::to_string(t) + (l ? " (layer " + std::to_string(l + 1) + ")" : ""));
    }

    const int range = BendRange(perf, t, report);
    report.bend_ranges[t] = range;

    // Send a channel message to all layers at the MIDI position of performance tick `tick`.
    auto all = [&](uint32_t tick, std::initializer_list<uint8_t> message)
    {
        const uint64_t midi_tick = EventTick(perf, tick);
        for (std::size_t l = 0; l < layers; l++)
        {
            std::vector<uint8_t> b(message);
            b[0] = static_cast<uint8_t>((b[0] & 0xf0) | chans[l]);
            if (b.size() == 2)
            {
                w[l].Event(midi_tick, {b[0], b[1]});
            }
            else
            {
                w[l].Event(midi_tick, {b[0], b[1], b[2]});
            }
        }
    };

    // Set RPN 0 to `range` semitones, then select the null RPN to prevent later data entry messages from changing it.
    all(0, {0xb0, 101, 0});
    all(0, {0xb0, 100, 0});
    all(0, {0xb0, 6, static_cast<uint8_t>(range)});
    all(0, {0xb0, 38, 0});
    all(0, {0xb0, 101, 127});
    all(0, {0xb0, 100, 127});

    std::vector<std::array<uint32_t, 128>> sounding(layers); // note id per key, 0: free
    std::vector<std::array<uint32_t, 128>> since(layers);    // that note's start time
    std::map<uint32_t, std::size_t> layer_of;                // note id -> layer
    for (const TrackEvent& e : perf.tracks[t])
    {
        const uint8_t key = e.key;
        switch (e.kind)
        {
        case EventKind::kNoteOn:
            {
                std::size_t l = 0;
                while (l < layers && sounding[l][key] != 0)
                {
                    l++;
                }
                if (l == layers)
                {
                    l = 0;
                    for (std::size_t i = 1; i < layers; i++)
                    {
                        if (since[i][key] < since[l][key])
                        {
                            l = i;
                        }
                    }

                    w[l].Event(EventTick(perf, e.tick), {static_cast<uint8_t>(0x80 | chans[l]), key, 64});
                    layer_of.erase(sounding[l][key]);
                    report.cut_notes++;
                }

                sounding[l][key] = e.note;
                since[l][key] = e.tick;
                layer_of[e.note] = l;
                w[l].Event(EventTick(perf, e.tick), {static_cast<uint8_t>(0x90 | chans[l]), key, e.value});
                break;
            }

        case EventKind::kNoteOff:
            {
                auto it = layer_of.find(e.note);
                if (it == layer_of.end())
                {
                    break; // cut earlier
                }

                const std::size_t l = it->second;
                layer_of.erase(it);
                sounding[l][key] = 0;
                w[l].Event(EventTick(perf, e.tick), {static_cast<uint8_t>(0x80 | chans[l]), key, 64});
                break;
            }

        case EventKind::kPreset:
            all(e.tick, {0xb0, 0, static_cast<uint8_t>((e.preset >> 7) & 0x7f)});
            all(e.tick, {0xb0, 32, 0});
            all(e.tick, {0xc0, static_cast<uint8_t>(e.preset & 0x7f)});
            break;

        case EventKind::kVolume:
            all(e.tick, {0xb0, 7, e.value});
            break;

        case EventKind::kExpression:
            all(e.tick, {0xb0, 11, e.value});
            break;

        case EventKind::kPan:
            all(e.tick, {0xb0, 10, e.value});
            break;

        case EventKind::kModulation:
            all(e.tick, {0xb0, 1, e.value});
            break;

        case EventKind::kReverb:
            all(e.tick, {0xb0, 91, e.value});
            break;

        case EventKind::kChorus:
            all(e.tick, {0xb0, 93, e.value});
            break;

        case EventKind::kSoundOff:
            all(e.tick, {0xb0, 120, 0});
            break;

        case EventKind::kFilter:
            all(e.tick, {0xb0, kFilterController, e.value});
            break;

        case EventKind::kSfzLowPass:
            all(e.tick, {0xb0, kSfzLowPassController, e.value});
            break;

        case EventKind::kSfzBiquad:
            all(e.tick, {0xb0, kSfzBiquadController, e.value});
            break;

        case EventKind::kPitch:
            {
                const int64_t v = std::clamp<int64_t>(8192 + std::llround(e.semitones / range * 8192.0), 0, 16383);
                all(e.tick, {0xe0, static_cast<uint8_t>(v & 0x7f), static_cast<uint8_t>(v >> 7)});
                break;
            }
        }
    }

    for (TrackWriter& x : w)
    {
        x.End(perf.end_tick * static_cast<uint64_t>(TickScale(perf)));
    }

    return w;
}

} // namespace

std::vector<uint8_t> WriteMidi(const Performance& perf, const std::string& title, MidiReport& report)
{
    const std::array<std::vector<int>, 16> channels = AssignChannels(perf);
    report.channels = 0;
    for (const std::vector<int>& c : channels)
    {
        report.channels += static_cast<int>(c.size());
    }

    // Record presets used on the drum channel so the SoundFont writer can copy them to bank 128.
    for (int t = 0; t < 16; t++)
    {
        if (std::find(channels[t].begin(), channels[t].end(), kDrumChannel) == channels[t].end())
        {
            continue;
        }

        for (const TrackEvent& e : perf.tracks[t])
        {
            if (e.kind == EventKind::kPreset)
            {
                report.channel_10_presets.insert(e.preset);
            }
        }
    }

    std::vector<TrackWriter> tracks;
    tracks.push_back(ConductorTrack(perf, title, channels));
    for (int t = 0; t < 16; t++)
    {
        if (channels[t].empty())
        {
            continue;
        }

        for (TrackWriter& x : TrackLayers(perf, t, channels[t], report))
        {
            tracks.push_back(std::move(x));
        }
    }

    std::vector<uint8_t> out = {'M', 'T', 'h', 'd', 0, 0, 0, 6, 0, 1};
    const uint16_t n = static_cast<uint16_t>(tracks.size());
    out.push_back(static_cast<uint8_t>(n >> 8));
    out.push_back(static_cast<uint8_t>(n));
    const uint16_t division = static_cast<uint16_t>(std::max<uint32_t>(perf.timebase, 1) * TickScale(perf));
    out.push_back(static_cast<uint8_t>(division >> 8));
    out.push_back(static_cast<uint8_t>(division));

    for (const TrackWriter& x : tracks)
    {
        x.AppendTo(out);
    }

    return out;
}

} // namespace citrusf2
