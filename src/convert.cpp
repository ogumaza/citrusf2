// SPDX-License-Identifier: MIT

// Sequence conversion (see convert.h). This file builds the SoundFont; midi.cpp and sfz.cpp write the other formats.
//
// Each performance preset combines a bank instrument with its track settings. It becomes one SoundFont instrument and
// preset, with a zone for each region. Zones carry the region's key and velocity ranges, sample, root key, tuning,
// volume, pan, envelope, key group and loop. Track overrides apply to the envelope. Presets used by MIDI channel 10
// also get a copy in bank 128, where synths look for drum instruments.

#include "convert.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <iterator>
#include <map>
#include <string_view>
#include <utility>

#include "binary.h"
#include "envelope.h"
#include "instruments.h"
#include "midi.h"
#include "pan.h"
#include "sf2.h"
#include "wave.h"

namespace citrusf2
{

namespace
{

// SoundFont modulator sources: 7-bit index, CC flag (bit 7), direction (bit 8), polarity (bit 9), type (bits 10-15).
constexpr uint16_t kSrcVelocityLinearNegative = 0x0102;
constexpr uint16_t kSrcVelocitySwitch = 0x0C02;
constexpr uint16_t kSrcModWheel = 0x0081;   // CC1, linear, unipolar, positive
constexpr uint16_t kSrcPitchWheel = 0x020E; // linear, bipolar, positive
constexpr uint16_t kSrcPitchWheelSensitivity = 0x0010;
constexpr uint16_t kSrcFilterController = 0x0080 | kFilterController; // linear, unipolar, positive

// SoundFont bank for MIDI channel 10 (General MIDI drums). Synths use this bank regardless of bank select.
constexpr uint16_t kChannel10Bank = 128;

// Build the instrument's global zone. Disable the default velocity-to-filter modulator, which nw::snd doesn't use,
// overriding both the SoundFont 2.01 and FluidSynth versions.
//
// CC1 carries LFO depth / 2. The resulting depth is MML depth / 128 times `range` semitones for pitch, or 6 * range dB
// for volume. When the LFO is omitted, disable the default CC1-to-vibrato modulator too.
//
// The default pitch wheel modulator uses 12700 cents times the bend range / 128. This makes a 7-semitone bend set
// through RPN 0 about 5.5 cents flat. Replace it with a 12800-cent modulator targeting fineTune, as FluidSynth does, to
// get the full bend range.
//
// Set the track filter here (see filter.h). If it changes during a note, use a modulator driven by kFilterController
// instead.
sf2::Zone GlobalZone(const PresetKey& key)
{
    sf2::Zone global;
    global.modulators.push_back({kSrcVelocityLinearNegative, sf2::kInitialFilterFc, 0, 0});
    global.modulators.push_back({kSrcVelocityLinearNegative, sf2::kInitialFilterFc, 0, kSrcVelocitySwitch});
    global.modulators.push_back({kSrcPitchWheel, sf2::kFineTune, 12800, kSrcPitchWheelSensitivity});
    if (key.filter_controller)
    {
        global.modulators.push_back({kSrcFilterController, sf2::kInitialFilterFc, -kFilterControllerRange, 0});
    }
    if (key.filter.cutoff != kFilterOpen)
    {
        global.generators.push_back(sf2::Generator::Value(sf2::kInitialFilterFc, key.filter.cutoff));
    }
    if (key.filter.resonance != 0)
    {
        global.generators.push_back(sf2::Generator::Value(sf2::kInitialFilterQ, key.filter.resonance));
    }

    auto amount = [](double v)
    {
        return static_cast<int16_t>(std::clamp<int64_t>(std::llround(v), 0, 32767));
    };

    constexpr double kDepthScale = 2.0; // CC1 = depth / 2; both use a divisor of 128
    const double hz = key.lfo_speed * 100.0 / 256.0 / kEnvTimeScale;
    const bool lfo = hz > 0.0;
    const double pitch_amount = key.lfo_type == 0 && lfo ? 100.0 * key.lfo_range * kDepthScale : 0.0;
    global.modulators.push_back({kSrcModWheel, sf2::kVibLfoToPitch, amount(pitch_amount), 0});

    const int freq =
        std::clamp(static_cast<int>(std::lround(1200.0 * std::log2(std::max(hz, 0.001) / 8.176))), -16000, 4500);
    const int delay = ToTimecents(key.lfo_delay * kEnvStepMs * kEnvTimeScale, -12000, 5000);
    if (key.lfo_type == 1 && lfo)
    {
        const int16_t db = amount(60.0 * key.lfo_range * kDepthScale);
        global.modulators.push_back({kSrcModWheel, sf2::kModLfoToVolume, db, 0});
        global.generators.push_back(sf2::Generator::Value(sf2::kFreqModLfo, freq));
        global.generators.push_back(sf2::Generator::Value(sf2::kDelayModLfo, delay));
    }
    else if (key.lfo_type == 0 && lfo)
    {
        global.generators.push_back(sf2::Generator::Value(sf2::kFreqVibLfo, freq));
        global.generators.push_back(sf2::Generator::Value(sf2::kDelayVibLfo, delay));
    }

    return global;
}

// Generator values shared by a region's zones, after applying the track settings.
struct RegionGenerators
{
    int attenuation = 0; // centibels
    Sf2Envelope envelope;
    uint8_t root_key = 60;
    int coarse_tune = 0, fine_tune = 0; // semitones and cents
    bool loop = false;
    uint8_t exclusive_class = 0;
};

// Convert region `r` using track settings `key`. `loop` indicates a looping wave. `quieter` adds attenuation in
// centibels to every zone (see LoudestRegion).
RegionGenerators ConvertRegion(const PresetKey& key, const VelocityRegion& r, bool loop, double quieter)
{
    RegionGenerators g;
    g.envelope = ConvertEnvelope(
        OverrideEnvelope({r.adshr.attack, r.adshr.decay, r.adshr.sustain, r.adshr.hold, r.adshr.release}, key));

    // Regions that ignore note-off get a MIDI note-off when nw::snd releases the channel, when they become inaudible,
    // or when the key is struck again (see performance.cpp). For the latter two, keep the decay going by using the
    // faster of the decay and release rates.
    if (r.ignore_note_off && g.envelope.sustain >= kInaudibleSustain)
    {
        g.envelope.release = std::min(g.envelope.release, g.envelope.decay);
    }

    // Clamp tuning to SoundFont's +/-10 octaves. This also prevents invalid pitches, such as infinity, from rounding
    // differently across platforms.
    const double cents = r.pitch > 0 ? std::clamp(1200.0 * std::log2(r.pitch), -12000.0, 12000.0) : 0.0;
    const int total = static_cast<int>(std::lround(cents));
    g.coarse_tune = total / 100;
    g.fine_tune = total - g.coarse_tune * 100;

    g.attenuation = 1440;
    if (r.volume != 0)
    {
        const double attenuation = quieter - 200.0 * std::log10(r.volume / 127.0);
        g.attenuation = std::clamp(static_cast<int>(std::lround(attenuation)), 0, 1440);
    }

    g.root_key = r.original_key;
    g.loop = loop;
    g.exclusive_class = r.key_group;

    return g;
}

// Build a zone for a key range and velocity range, using `sample` and the region's generators. `pan` uses nw::snd's -1
// (left) to 1 (right) range.
sf2::Zone RegionZone(const RegionGenerators& g, const std::pair<int, int>& keys, const std::pair<int, int>& velocities,
                     double pan, uint16_t sample)
{
    sf2::Zone z;

    auto gen = [&](uint16_t oper, int v)
    {
        z.generators.push_back(sf2::Generator::Value(oper, v));
    };

    z.generators.push_back(sf2::Generator::Range(sf2::kKeyRange, keys.first, keys.second));
    z.generators.push_back(sf2::Generator::Range(sf2::kVelRange, velocities.first, velocities.second));
    if (g.attenuation != 0)
    {
        gen(sf2::kInitialAttenuation, g.attenuation);
    }

    const int pan_gen = static_cast<int>(std::lround(SoundFontPan(pan) * 500.0));
    if (pan_gen != 0)
    {
        gen(sf2::kPan, pan_gen);
    }

    gen(sf2::kAttackVolEnv, g.envelope.attack);
    gen(sf2::kHoldVolEnv, g.envelope.hold);
    gen(sf2::kDecayVolEnv, g.envelope.decay);
    gen(sf2::kSustainVolEnv, g.envelope.sustain);
    gen(sf2::kReleaseVolEnv, g.envelope.release);

    gen(sf2::kOverridingRootKey, g.root_key);
    if (g.coarse_tune != 0)
    {
        gen(sf2::kCoarseTune, g.coarse_tune);
    }
    if (g.fine_tune != 0)
    {
        gen(sf2::kFineTune, g.fine_tune);
    }

    if (g.loop)
    {
        gen(sf2::kSampleModes, 1);
    }
    if (g.exclusive_class != 0)
    {
        gen(sf2::kExclusiveClass, g.exclusive_class);
    }
    gen(sf2::kSampleId, sample);

    return z;
}

// Build a SoundFont instrument and preset for each performance preset. Add each wave's samples only once.
class SoundFontBuilder
{
public:
    // Add `quieter` centibels of attenuation to every region (see LoudestRegion).
    SoundFontBuilder(BankSet& banks, double quieter) : banks_(banks), quieter_(quieter)
    {
    }

    // Add preset `index` at bank index / 128, program index % 128, using the bank instrument specified by `key`.
    void Add(uint32_t index, const PresetKey& key, const std::string& name);

    // Copy a preset previously added by Add to bank 128 for MIDI channel 10. Keep its program number; warn if another
    // preset already occupies that slot.
    void AddChannel10Copy(uint32_t index);

    // The resulting SoundFont.
    sf2::SoundFont& Result()
    {
        return sf_;
    }

    // Missing instruments and waves, and presets that MIDI channel 10 cannot select.
    const std::vector<std::string>& Warnings() const
    {
        return warnings_;
    }

private:
    // Add a wave's samples on first use (two for stereo). Return an empty list if the wave cannot be read.
    const std::vector<uint16_t>& Samples(int slot, uint32_t wave_id_index, uint8_t root_key);

    BankSet& banks_;
    double quieter_;
    std::vector<std::string> warnings_;
    sf2::SoundFont sf_;
    std::map<std::pair<uint32_t, uint32_t>, std::vector<uint16_t>> samples_; // wave key -> sample ids
};

const std::vector<uint16_t>& SoundFontBuilder::Samples(int slot, uint32_t wave_id_index, uint8_t root_key)
{
    static const std::vector<uint16_t> kNone;
    const auto key = banks_.WaveKey(slot, wave_id_index);
    if (!key)
    {
        return kNone;
    }
    if (auto it = samples_.find(*key); it != samples_.end())
    {
        return it->second;
    }

    std::vector<uint16_t> ids;
    if (const Pcm* pcm = banks_.GetWave(slot, wave_id_index))
    {
        PreparedWave w = PrepareForSoundFont(*pcm);
        const std::size_t n = std::min<std::size_t>(w.channels.size(), 2);
        for (std::size_t c = 0; c < n; c++)
        {
            sf2::Sample s;
            s.name = "w" + std::to_string(key->first) + "_" + std::to_string(key->second) +
                     (n == 1 ? "" : (c == 0 ? "L" : "R"));
            s.data = std::move(w.channels[c]);
            s.sample_rate = pcm->sample_rate;
            s.original_key = root_key;
            if (w.loop)
            {
                s.loop_start = w.loop_start;
                s.loop_end = w.loop_end;
            }
            else if (s.data.size() > 1)
            {
                s.loop_end = static_cast<uint32_t>(s.data.size()); // some synths validate unused loops too
            }

            s.type = n == 1 ? sf2::kMono : (c == 0 ? sf2::kLeft : sf2::kRight);
            ids.push_back(static_cast<uint16_t>(sf_.samples.size()));
            sf_.samples.push_back(std::move(s));
        }

        if (n == 2)
        {
            sf_.samples[ids[0]].link = ids[1];
            sf_.samples[ids[1]].link = ids[0];
        }

        if (sf_.samples.size() > 0xffff)
        {
            throw FormatError("more than 65535 samples");
        }
    }

    return samples_.emplace(*key, std::move(ids)).first->second;
}

void SoundFontBuilder::Add(uint32_t index, const PresetKey& key, const std::string& name)
{
    sf2::Instrument inst;
    inst.name = name;
    inst.zones.push_back(GlobalZone(key));

    const Instrument* bank_inst = banks_.GetInstrument(key.bank_slot, key.program);
    if (!bank_inst)
    {
        warnings_.push_back(name + ": bank " + banks_.BankName(key.bank_slot) + " has no instrument " +
                            std::to_string(key.program));
    }

    for (const PlayedRanges& played : bank_inst ? RegionRanges(*bank_inst) : std::vector<PlayedRanges>())
    {
        const VelocityRegion& r = *played.region;
        // A no-wave entry is silent in the game too.
        const std::vector<uint16_t>& ids = Samples(key.bank_slot, r.wave_id_index, r.original_key);
        if (ids.empty() && !banks_.NoWave(key.bank_slot, r.wave_id_index))
        {
            warnings_.push_back(name + ": no wave for keys " + std::to_string(played.key_region->lo) + "-" +
                                std::to_string(played.key_region->hi));
        }
        if (ids.empty())
        {
            continue;
        }

        // nw::snd pan: (region pan + track initial pan - 64) / 63. Stereo channels get an additional -1 or +1.
        const double pan = (static_cast<int>(r.pan) + key.init_pan - 64) / 63.0;
        const RegionGenerators g =
            ConvertRegion(key, r, banks_.GetWave(key.bank_slot, r.wave_id_index)->loop, quieter_);
        for (const std::pair<int, int>& keys : played.keys)
        {
            for (const std::pair<int, int>& velocities : played.velocities)
            {
                for (std::size_t c = 0; c < ids.size(); c++)
                {
                    double p = pan;
                    if (ids.size() == 2)
                    {
                        p += c == 0 ? -1.0 : 1.0;
                    }

                    inst.zones.push_back(RegionZone(g, keys, velocities, p, ids[c]));
                }
            }
        }
    }

    const uint16_t inst_index = static_cast<uint16_t>(sf_.instruments.size());
    sf_.instruments.push_back(std::move(inst));

    sf2::Preset preset;
    preset.name = name;
    preset.bank = static_cast<uint16_t>(index / 128);
    preset.program = static_cast<uint16_t>(index % 128);
    sf2::Zone z;
    z.generators.push_back(sf2::Generator::Value(sf2::kInstrument, inst_index));
    preset.zones.push_back(std::move(z));
    sf_.presets.push_back(std::move(preset));
}

void SoundFontBuilder::AddChannel10Copy(uint32_t index)
{
    sf2::Preset copy = sf_.presets[index];
    for (const sf2::Preset& p : sf_.presets)
    {
        if (p.bank == kChannel10Bank && p.program == copy.program)
        {
            warnings_.push_back(copy.name + ": MIDI channel 10 plays " + p.name + " instead");
            return;
        }
    }

    copy.bank = kChannel10Bank;
    sf_.presets.push_back(std::move(copy));
}

// Name presets by bank slot and program ("b0 p12"). Add a number when different track settings produce several presets
// from the same instrument.
std::vector<std::string> PresetNames(const std::vector<PresetKey>& keys)
{
    std::map<std::pair<uint8_t, uint32_t>, int> count, seen;
    for (const PresetKey& k : keys)
    {
        count[{k.bank_slot, k.program}]++;
    }

    std::vector<std::string> names;
    for (const PresetKey& k : keys)
    {
        std::string n = "b" + std::to_string(k.bank_slot) + " p" + std::to_string(k.program);
        if (count[{k.bank_slot, k.program}] > 1)
        {
            n += " #" + std::to_string(++seen[{k.bank_slot, k.program}]);
        }

        names.push_back(n);
    }

    return names;
}

// Check whether Windows reserves `stem` (the part before the first dot) as a device name. Trailing spaces are ignored;
// COM and LPT followed by superscript 1, 2 or 3 also count.
bool IsDeviceName(std::string stem)
{
    constexpr std::string_view kDevices[] = {
        "CON",       "PRN",       "AUX",       "NUL",  "CONIN$", "CONOUT$",   "COM0",      "COM1",
        "COM2",      "COM3",      "COM4",      "COM5", "COM6",   "COM7",      "COM8",      "COM9",
        "COM\u00b9", "COM\u00b2", "COM\u00b3", "LPT0", "LPT1",   "LPT2",      "LPT3",      "LPT4",
        "LPT5",      "LPT6",      "LPT7",      "LPT8", "LPT9",   "LPT\u00b9", "LPT\u00b2", "LPT\u00b3"};

    while (!stem.empty() && stem.back() == ' ')
    {
        stem.pop_back();
    }
    for (char& c : stem)
    {
        if (c >= 'a' && c <= 'z')
        {
            c = static_cast<char>(c - 'a' + 'A');
        }
    }

    return std::find(std::begin(kDevices), std::end(kDevices), stem) != std::end(kDevices);
}

// Find the highest region volume used by `perf`. Match regions by preset, key and velocity, as the synth does.
int LoudestRegion(const Performance& perf, const BankSet& banks)
{
    int loudest = 0;
    for (const std::vector<TrackEvent>& events : perf.tracks)
    {
        const PresetKey* preset = nullptr;
        for (const TrackEvent& e : events)
        {
            if (e.kind == EventKind::kPreset)
            {
                preset = &perf.presets[e.preset];
            }
            else if (e.kind == EventKind::kNoteOn && preset)
            {
                const VelocityRegion* r = banks.FindRegion(preset->bank_slot, preset->program, e.key, e.value);
                loudest = std::max(loudest, r ? static_cast<int>(r->volume) : 0);
            }
        }
    }

    return loudest;
}

} // namespace

Conversion ConvertSequence(const SoundArchive& archive, uint32_t sound_index, const PerformOptions& options)
{
    const SoundInfo& sound = archive.Sounds()[sound_index];
    BankSet banks(archive, sound);
    Conversion c;
    c.performance = Perform(archive, sound_index, banks, options);

    MidiReport midi;
    c.midi = WriteMidi(c.performance, sound.name, midi);
    c.channels = midi.channels;
    if (midi.cut_notes)
    {
        c.performance.approximations["overlapping same-key notes cut short (all MIDI channels in use)"] +=
            midi.cut_notes;
    }

    // Region volumes can reach 255, but SoundFont zones cannot amplify their samples. If any used region exceeds 127,
    // attenuate every zone equally to preserve their relative levels. SFZ keeps the original volumes.
    const int loudest = LoudestRegion(c.performance, banks);
    const double quieter = loudest > 127 ? 200.0 * std::log10(loudest / 127.0) : 0.0;
    if (loudest > 127)
    {
        char what[160];
        std::snprintf(what, sizeof(what),
                      "region volume exceeds 127 (SoundFont attenuated by %.1f dB; SFZ keeps the original "
                      "volume)",
                      quieter / 10.0);
        c.performance.approximations[what] = NoteCount(c.performance);
    }

    SoundFontBuilder builder(banks, quieter);
    const std::vector<std::string> names = PresetNames(c.performance.presets);
    for (std::size_t i = 0; i < c.performance.presets.size(); i++)
    {
        builder.Add(static_cast<uint32_t>(i), c.performance.presets[i], names[i]);
    }
    for (uint32_t index : midi.channel_10_presets)
    {
        builder.AddChannel10Copy(index);
    }

    sf2::SoundFont& sf = builder.Result();
    sf.name = sound.name;
    std::string comment = "Converted by citrusf2 from sequence " + sound.name + ". Banks:";
    for (int slot = 0; slot < 4; slot++)
    {
        if (banks.GetBank(slot))
        {
            comment += " b" + std::to_string(slot) + " = " + banks.BankName(slot) + ";";
        }
    }
    sf.comment = comment;

    c.samples = sf.samples.size();
    c.sf2 = sf2::Write(sf);
    c.sfz = BuildSfz(c.performance, midi, banks, sound.name, names);

    c.warnings = midi.warnings;
    c.warnings.insert(c.warnings.end(), builder.Warnings().begin(), builder.Warnings().end());
    c.warnings.insert(c.warnings.end(), banks.Errors().begin(), banks.Errors().end());
    c.warnings.insert(c.warnings.end(), c.performance.warnings.begin(), c.performance.warnings.end());

    return c;
}

std::string FileName(const std::string& sequence_name)
{
    constexpr std::string_view kNotAllowed = "<>:\"/\\|?*";
    std::string name = sequence_name;
    for (char& c : name)
    {
        if (static_cast<unsigned char>(c) < 0x20 || kNotAllowed.find(c) != std::string_view::npos)
        {
            c = '_';
        }
    }

    // This is also the SFZ directory name. Windows strips trailing dots and spaces from directory names; "." and ".."
    // refer to existing directories.
    if (!name.empty() && (name.back() == '.' || name.back() == ' '))
    {
        name.back() = '_';
    }

    // Windows reserves device names regardless of extension: "NUL.mid" and "NUL.x.mid" are invalid, but "NUL_.x.mid" is
    // allowed.
    const std::size_t stem_end = std::min(name.find('.'), name.size());
    if (IsDeviceName(name.substr(0, stem_end)))
    {
        name.insert(stem_end, "_");
    }

    return name;
}

} // namespace citrusf2
