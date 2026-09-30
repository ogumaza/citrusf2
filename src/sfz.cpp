// SPDX-License-Identifier: MIT

// SFZ output (see sfz.h).
//
// Each track file has a <master> per preset, selected by loprog/hiprog and, above 128 presets, bank select (CC0).
// Regions match the SoundFont: nw::snd key/velocity precedence, sample, root key, tuning, volume, pan, overridden
// envelope, key groups (group/off_by) and loops. LFOs use a sine with depth from CC1.
//
// Controller curves map kSfzLowPassController to lpf_1p cutoff (filter 1), and kSfzBiquadController to biquad cutoff,
// resonance and gain. SFZ cannot bypass a biquad, so hicc/locc choose filtered or unfiltered regions at note-on.
//
// The low-pass must support being enabled during a note, so it stays active at sfizz's maximum 20 kHz cutoff when
// nominally off. This attenuates 10 kHz by about 0.5 dB and 16 kHz by 1 dB.

#include "sfz.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <map>
#include <numbers>
#include <set>
#include <utility>

#include "envelope.h"
#include "filter.h"
#include "instruments.h"
#include "pan.h"
#include "wave.h"

namespace citrusf2
{

namespace
{

// Custom curve IDs; sfizz reserves 0-6.
constexpr int kPanCurve = 7;              // CC10 to pan
constexpr int kLowPassCurve = 8;          // kSfzLowPassController to the low-pass's cutoff
constexpr int kBiquadCutoffCurve = 9;     // kSfzBiquadController to the biquad filter's cutoff
constexpr int kBiquadResonanceCurve = 10; // ... and resonance
constexpr int kBiquadGainCurve = 11;      // ... and gain

// Modulation amounts at curve value 1: cutoff in cents, resonance and volume in dB. Keep curves within -1..1 for
// compatibility with other SFZ players.
constexpr int kCutoffSpan = 12000;
constexpr int kResonanceSpan = 60;
constexpr int kGainSpan = 2;

// sfizz divides pitch wheel values by 8191; the MIDI writer uses 8192 steps per full range. Scale the SFZ bend range by
// 8191/8192.
constexpr double kBendScale = 8191.0 / 8192.0;

// Format `v` with at most `decimals` fractional digits.
std::string Number(double v, int decimals)
{
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%.*f", decimals, v);
    std::string s = buf;
    if (s.find('.') != std::string::npos)
    {
        while (s.back() == '0')
        {
            s.pop_back();
        }
        if (s.back() == '.')
        {
            s.pop_back();
        }
    }

    return s == "-0" ? "0" : s;
}

// Write a <curve> mapping controller values 0-127 through `f`.
template <typename F>
std::string Curve(int index, const char* what, F f)
{
    std::string s = "// " + std::string(what) + "\n<curve> curve_index=" + std::to_string(index);
    for (int v = 0; v < 128; v++)
    {
        char name[8];
        std::snprintf(name, sizeof(name), "v%03d", v);
        s += (v % 8 == 0 ? "\n" : " ") + std::string(name) + "=" + Number(f(v), 6);
    }

    return s + "\n\n";
}

// Make `text` safe for a single-line comment by replacing control characters, including line breaks, with spaces.
std::string CommentText(std::string text)
{
    for (char& c : text)
    {
        if (static_cast<unsigned char>(c) < 0x20 || c == 0x7f)
        {
            c = ' ';
        }
    }

    return text;
}

// Return opcode `name` for filter 1 or 2, e.g. "cutoff" or "cutoff2".
std::string FilterOpcode(const char* name, int index)
{
    return index == 1 ? name : std::string(name) + "2";
}

// Shared samples: one WAV file per wave channel, generated on first use.
class SampleSet
{
public:
    // Sample names (two for stereo) and loop bounds. Empty names indicate an unreadable wave.
    struct Wave
    {
        std::vector<std::string> names;
        bool loop = false;
        uint32_t loop_start = 0, loop_end = 0; // inclusive, as required by SFZ
    };

    explicit SampleSet(BankSet& banks) : banks_(banks)
    {
    }

    // Get bank wave `wave_id_index` from `slot`, generating its samples on first use.
    const Wave& Get(int slot, uint32_t wave_id_index);

    // Take ownership of the generated samples, leaving the collection empty.
    std::vector<SfzSample> Take()
    {
        return std::move(samples_);
    }

private:
    BankSet& banks_;
    std::map<std::pair<uint32_t, uint32_t>, Wave> waves_;
    std::vector<SfzSample> samples_;
};

const SampleSet::Wave& SampleSet::Get(int slot, uint32_t wave_id_index)
{
    static const Wave kNone;
    const auto key = banks_.WaveKey(slot, wave_id_index);
    if (!key)
    {
        return kNone;
    }
    if (auto it = waves_.find(*key); it != waves_.end())
    {
        return it->second;
    }

    Wave wave;
    if (const Pcm* pcm = banks_.GetWave(slot, wave_id_index))
    {
        const PreparedWave w = PrepareForSfz(*pcm);
        const std::size_t n = std::min<std::size_t>(w.channels.size(), 2);
        const std::string base = "w" + std::to_string(key->first) + "_" + std::to_string(key->second);
        for (std::size_t c = 0; c < n; c++)
        {
            const std::string name = base + (n == 1 ? "" : (c == 0 ? "L" : "R")) + ".wav";
            samples_.push_back({name, WavFile(w.channels[c], pcm->sample_rate)});
            wave.names.push_back(name);
        }

        wave.loop = w.loop;
        if (w.loop)
        {
            wave.loop_start = w.loop_start;
            wave.loop_end = w.loop_end - 1;
        }
    }

    return waves_.emplace(*key, std::move(wave)).first->second;
}

// Write velocity region opcodes using track settings `key`. Exclude sample, key/velocity ranges and pan. Add `gain_db`
// to the region volume.
std::string RegionOpcodes(const PresetKey& key, const VelocityRegion& r, const SampleSet::Wave& wave, double gain_db)
{
    const EnvelopeValues env =
        OverrideEnvelope({r.adshr.attack, r.adshr.decay, r.adshr.sustain, r.adshr.hold, r.adshr.release}, key);
    SfzEnvelope e = ConvertEnvelopeForSfz(env);

    // Keep decaying after note-off for regions that ignore it, as in the SoundFont (see convert.cpp).
    if (r.ignore_note_off && -SustainCentibels(env.sustain) >= kInaudibleSustain)
    {
        e.release = std::min(e.release, e.decay);
    }

    std::string s = " pitch_keycenter=" + std::to_string(r.original_key);
    const double cents = r.pitch > 0 ? std::clamp(1200.0 * std::log2(r.pitch), -12000.0, 12000.0) : 0.0;
    const int total = static_cast<int>(std::lround(cents));
    if (total / 100 != 0)
    {
        s += " transpose=" + std::to_string(total / 100);
    }
    if (total % 100 != 0)
    {
        s += " tune=" + std::to_string(total % 100);
    }

    // Convert linear nw::snd region volume to hundredths of a dB for SFZ.
    const int volume =
        r.volume == 0 ? -14400 : static_cast<int>(std::lround(2000.0 * std::log10(r.volume / 127.0) + 100.0 * gain_db));
    if (volume != 0)
    {
        s += " volume=" + Number(volume / 100.0, 2);
    }

    if (e.attack > 0.0)
    {
        s += " ampeg_attack=" + Number(e.attack, 6);
    }
    if (e.hold > 0.0)
    {
        s += " ampeg_hold=" + Number(e.hold, 6);
    }
    if (e.sustain < 100.0)
    {
        s += " ampeg_decay=" + Number(e.decay, 6) + " ampeg_sustain=" + Number(e.sustain, 6);
    }
    s += " ampeg_release=" + Number(e.release, 6);

    if (wave.loop)
    {
        s += " loop_mode=loop_continuous loop_start=" + std::to_string(wave.loop_start) +
             " loop_end=" + std::to_string(wave.loop_end);
    }
    else
    {
        s += " loop_mode=no_loop";
    }

    if (r.key_group != 0)
    {
        s += " group=" + std::to_string(r.key_group) + " off_by=" + std::to_string(r.key_group);
    }

    return s;
}

// Write one line per region in preset `key`, adding `gain_db` to each volume.
std::string PresetRegions(const PresetKey& key, BankSet& banks, SampleSet& samples, double gain_db)
{
    std::string s;
    const Instrument* inst = banks.GetInstrument(key.bank_slot, key.program);
    if (!inst)
    {
        return s; // already reported by the SoundFont writer
    }

    for (const PlayedRanges& played : RegionRanges(*inst))
    {
        const VelocityRegion& r = *played.region;
        const SampleSet::Wave& wave = samples.Get(key.bank_slot, r.wave_id_index);
        if (wave.names.empty())
        {
            continue; // already reported by the SoundFont writer
        }

        // nw::snd pan: (region pan + track initial pan - 64) / 63. Stereo channels get an additional -1 or +1.
        const double pan = (static_cast<int>(r.pan) + key.init_pan - 64) / 63.0;
        const std::string opcodes = RegionOpcodes(key, r, wave, gain_db);
        for (const std::pair<int, int>& keys : played.keys)
        {
            for (const std::pair<int, int>& velocities : played.velocities)
            {
                for (std::size_t c = 0; c < wave.names.size(); c++)
                {
                    double p = pan;
                    if (wave.names.size() == 2)
                    {
                        p += c == 0 ? -1.0 : 1.0;
                    }

                    s += "<region> sample=" + wave.names[c] + " lokey=" + std::to_string(keys.first) +
                         " hikey=" + std::to_string(keys.second) + " lovel=" + std::to_string(velocities.first) +
                         " hivel=" + std::to_string(velocities.second);
                    const int pan_tenths = static_cast<int>(std::lround(SoundFontPan(p) * 1000.0));
                    if (pan_tenths != 0)
                    {
                        s += " pan=" + Number(pan_tenths / 10.0, 1);
                    }
                    s += opcodes + "\n";
                }
            }
        }
    }

    return s;
}

// Write preset `key`'s track LFO as SFZ LFO 1: a sine with depth from CC1 (depth / 2). MML depth / 128 scales `range`
// semitones, 6 * range dB, or range times nw::snd's full pan. Omit LFOs with no effect or a delay that prevents them
// from starting.
std::string LfoOpcodes(const PresetKey& key)
{
    constexpr double kDepthScale = 2.0 * 127.0 / 128.0; // sfizz divides controller values by 127
    const double hz = key.lfo_speed * 100.0 / 256.0 / kEnvTimeScale;
    if (key.lfo_type > 2 || hz <= 0.0)
    {
        return "";
    }

    std::string s = "lfo1_wave=1 lfo1_freq=" + Number(hz, 6);
    if (key.lfo_delay != 0)
    {
        s += " lfo1_delay=" + Number(key.lfo_delay * kEnvStepMs * kEnvTimeScale / 1000.0, 6);
    }
    if (key.lfo_type == 0)
    {
        s += " lfo1_pitch_oncc1=" + Number(100.0 * key.lfo_range * kDepthScale, 6);
    }
    else if (key.lfo_type == 1)
    {
        s += " lfo1_volume_oncc1=" + Number(6.0 * key.lfo_range * kDepthScale, 6);
    }
    else
    {
        // Near centre, sfizz's sin/cos pan law moves 2 / pi as far as nw::snd's curve (see pan.h).
        s += " lfo1_pan_oncc1=" + Number(100.0 * key.lfo_range * kDepthScale * 2.0 / std::numbers::pi, 6);
    }

    return s + "\n";
}

// Write opcodes for biquad preset `type` in filter slot `index`.
std::string BiquadOpcodes(int type, int index)
{
    constexpr const char* kKinds[] = {"lpf_2p", "hpf_2p", "bpf_2p", "bpf_2p", "bpf_2p"};
    const std::string cc = "_oncc" + std::to_string(kSfzBiquadController);
    const std::string curve = "_curvecc" + std::to_string(kSfzBiquadController);
    return FilterOpcode("fil", index) + "_type=" + kKinds[type - 1] + " " + FilterOpcode("cutoff", index) + "=" +
           std::to_string(kSfzBiquadBase) + " " + FilterOpcode("cutoff", index) + cc + "=" +
           std::to_string(kCutoffSpan) + " " + FilterOpcode("cutoff", index) + curve + "=" +
           std::to_string(kBiquadCutoffCurve) + " " + FilterOpcode("resonance", index) + "=0 " +
           FilterOpcode("resonance", index) + cc + "=" + std::to_string(kResonanceSpan) + " " +
           FilterOpcode("resonance", index) + curve + "=" + std::to_string(kBiquadResonanceCurve) + " volume" + cc +
           "=" + std::to_string(kGainSpan) + " volume" + curve + "=" + std::to_string(kBiquadGainCurve);
}

// Write track `t`'s SFZ file.
std::string TrackFile(const Performance& perf, int t, int bend_range, BankSet& banks, SampleSet& samples,
                      const std::string& title, const std::vector<std::string>& preset_names)
{
    // Collect used presets and check whether the track enables its low-pass filter.
    std::set<uint32_t> presets;
    bool low_pass = false;
    for (const TrackEvent& e : perf.tracks[t])
    {
        if (e.kind == EventKind::kPreset)
        {
            presets.insert(e.preset);
        }
        else if (e.kind == EventKind::kSfzLowPass && TrackFilters(0x40 - e.value, 0, 0).low_pass >= 0)
        {
            low_pass = true;
        }
    }

    const int biquad = perf.biquad_types[t];
    const int biquad_filter = low_pass ? 2 : 1;

    std::string s = "// Track " + std::to_string(t) + " of " + CommentText(title) +
                    ", converted by citrusf2. Load for MIDI track \"Track " + std::to_string(t) +
                    "\" and its layers.\n";
    if (perf.level_cut_db > 0.0)
    {
        s += "// MIDI gain was reduced by " + Number(perf.level_cut_db, 2) + " dB. Region volumes restore that gain.\n";
    }
    s += "\n";
    s += "<control>\ndefault_path=../" + kSfzSamplesFolder + "/\nhint_ram_based=1\n\n";

    // Map CC10 to (value - 64) / 64, matching the SoundFont pan convention used by the MIDI writer.
    s += Curve(kPanCurve, "CC10 to pan", [](int v) { return (v - 64) / 64.0; });
    if (low_pass)
    {
        s += Curve(kLowPassCurve, "Low-pass cutoff by controller value: units of 12000 cents relative to 20 kHz",
                   [](int v)
                   {
                       const int index = TrackFilters(0x40 - v, 0, 0).low_pass;
                       return index < 0 ? 0.0 : SfzLowPassCutoff(index) / static_cast<double>(kCutoffSpan);
                   });
    }
    if (biquad != 0)
    {
        auto step = [biquad](int v)
        {
            return SfzBiquadStep(biquad, TrackFilters(0x40, biquad, std::max(v, 1)).biquad_step);
        };
        s += Curve(kBiquadCutoffCurve, "Biquad cutoff: units of 12000 cents relative to 1000 Hz",
                   [&](int v) { return step(v).cutoff / static_cast<double>(kCutoffSpan); });
        s += Curve(kBiquadResonanceCurve, "Biquad resonance (Q in dB): units of 60 dB",
                   [&](int v) { return step(v).resonance / 100.0 / kResonanceSpan; });
        s += Curve(kBiquadGainCurve, "Biquad gain: units of 2 dB",
                   [&](int v) { return step(v).gain / 100.0 / kGainSpan; });
    }

    const std::string bend = Number(bend_range * 100.0 * kBendScale, 6);
    s += "<global>\nbend_up=" + bend + " bend_down=-" + bend + "\n";
    s += "pan_oncc10=100 pan_curvecc10=" + std::to_string(kPanCurve) + " pan_smoothcc10=10\n";
    if (low_pass)
    {
        const std::string cc = std::to_string(kSfzLowPassController);
        s += "fil_type=lpf_1p cutoff=" + std::to_string(kSfzLowPassOpen) + " cutoff_oncc" + cc + "=" +
             std::to_string(kCutoffSpan) + " cutoff_curvecc" + cc + "=" + std::to_string(kLowPassCurve) + "\n";
    }

    const bool banks_selected = perf.presets.size() > 128;
    for (uint32_t p : presets)
    {
        const PresetKey& key = perf.presets[p];
        s += "\n// " + CommentText(preset_names[p]) + "\n<master> loprog=" + std::to_string(p % 128) +
             " hiprog=" + std::to_string(p % 128);
        if (banks_selected)
        {
            s += " locc0=" + std::to_string(p / 128) + " hicc0=" + std::to_string(p / 128);
        }
        s += "\n" + LfoOpcodes(key);

        const std::string regions = PresetRegions(key, banks, samples, perf.level_cut_db);
        if (biquad == 0)
        {
            s += "<group>\n" + regions;
        }
        else
        {
            const std::string cc = std::to_string(kSfzBiquadController);
            s += "<group> hicc" + cc + "=0\n" + regions;
            s += "<group> locc" + cc + "=1 " + BiquadOpcodes(biquad, biquad_filter) + "\n" + regions;
        }
    }

    return s;
}

} // namespace

SfzInstruments BuildSfz(const Performance& perf, const MidiReport& midi, BankSet& banks, const std::string& title,
                        const std::vector<std::string>& preset_names)
{
    SfzInstruments out;
    SampleSet samples(banks);
    for (int t = 0; t < 16; t++)
    {
        if (midi.bend_ranges[t] == 0)
        {
            continue; // no MIDI channel assigned to this track
        }

        out.files.push_back({"Track " + std::to_string(t) + ".sfz",
                             TrackFile(perf, t, midi.bend_ranges[t], banks, samples, title, preset_names)});
    }

    out.samples = samples.Take();

    return out;
}

std::vector<uint8_t> WavFile(std::span<const int16_t> samples, uint32_t rate)
{
    const uint32_t data_size = static_cast<uint32_t>(samples.size() * 2);
    std::vector<uint8_t> out;
    out.reserve(44 + data_size);

    auto u32 = [&](uint32_t v)
    {
        for (int i = 0; i < 4; i++)
        {
            out.push_back(static_cast<uint8_t>(v >> (8 * i)));
        }
    };
    auto u16 = [&](uint16_t v)
    {
        out.push_back(static_cast<uint8_t>(v));
        out.push_back(static_cast<uint8_t>(v >> 8));
    };
    auto tag = [&](const char* t)
    {
        out.insert(out.end(), t, t + 4);
    };

    tag("RIFF");
    u32(36 + data_size);
    tag("WAVE");
    tag("fmt ");
    u32(16);
    u16(1); // PCM
    u16(1); // mono
    u32(rate);
    u32(rate * 2);
    u16(2);
    u16(16);
    tag("data");
    u32(data_size);
    for (int16_t v : samples)
    {
        u16(static_cast<uint16_t>(v));
    }

    return out;
}

} // namespace citrusf2
