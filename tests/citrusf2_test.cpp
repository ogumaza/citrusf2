// SPDX-License-Identifier: MIT

// Unit tests run without game data. Pass a .bcsar to also convert and validate every sequence in that archive.

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <exception>
#include <fstream>
#include <ios>
#include <iterator>
#include <map>
#include <numbers>
#include <set>
#include <span>
#include <sstream>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include "binary.h"
#include "convert.h"
#include "csar.h"
#include "envelope.h"
#include "filter.h"
#include "instruments.h"
#include "midi.h"
#include "pan.h"
#include "performance.h"
#include "pitch.h"
#include "sf2.h"
#include "sfz.h"
#include "wave.h"

using namespace citrusf2;

namespace
{

// Failed check count.
int failures = 0;

// Report and count a failed check, including file and line, without aborting the test.
#define CITRUSF2_CHECK(cond)                                                              \
    do                                                                                    \
    {                                                                                     \
        if (!(cond))                                                                      \
        {                                                                                 \
            std::fprintf(stderr, "%s:%d: check failed: %s\n", __FILE__, __LINE__, #cond); \
            failures++;                                                                   \
        }                                                                                 \
    } while (0)

uint32_t Le32(const std::vector<uint8_t>& d, std::size_t o)
{
    return d[o] | (d[o + 1] << 8) | (d[o + 2] << 16) | (static_cast<uint32_t>(d[o + 3]) << 24);
}

uint16_t Le16(const std::vector<uint8_t>& d, std::size_t o)
{
    return static_cast<uint16_t>(d[o] | (d[o + 1] << 8));
}

uint32_t Be32(const std::vector<uint8_t>& d, std::size_t o)
{
    return (static_cast<uint32_t>(d[o]) << 24) | (d[o + 1] << 16) | (d[o + 2] << 8) | d[o + 3];
}

// Offsets and sizes of SoundFont list subchunks.
struct Sf2View
{
    bool ok = false;
    std::map<std::string, std::pair<std::size_t, uint32_t>> chunks;
};

// Find SoundFont list subchunks; set `ok` false for invalid RIFF structure.
Sf2View ParseSf2(const std::vector<uint8_t>& d)
{
    Sf2View v;
    if (d.size() < 12 || std::memcmp(d.data(), "RIFF", 4) != 0 || std::memcmp(d.data() + 8, "sfbk", 4) != 0 ||
        Le32(d, 4) + 8 != d.size())
    {
        return v;
    }

    std::size_t off = 12;
    while (off + 8 <= d.size())
    {
        const uint32_t size = Le32(d, off + 4);
        if (std::memcmp(d.data() + off, "LIST", 4) != 0 || off + 8 + size > d.size())
        {
            return v;
        }

        std::size_t sub = off + 12;
        const std::size_t end = off + 8 + size;
        while (sub + 8 <= end)
        {
            const uint32_t s = Le32(d, sub + 4);
            v.chunks[std::string(reinterpret_cast<const char*>(d.data() + sub), 4)] = {sub + 8, s};
            sub += 8 + s + (s & 1);
        }
        if (sub != end)
        {
            return v;
        }

        off = end;
    }

    v.ok = off == d.size();

    return v;
}

// Validate the SoundFont structures used by synths, as check_sf2.py does. Return an error description, or an empty
// string on success.
std::string Sf2Problem(const std::vector<uint8_t>& d)
{
    const Sf2View v = ParseSf2(d);
    if (!v.ok)
    {
        return "RIFF layout";
    }
    for (const char* c :
         {"ifil", "isng", "INAM", "smpl", "phdr", "pbag", "pmod", "pgen", "inst", "ibag", "imod", "igen", "shdr"})
    {
        if (!v.chunks.count(c))
        {
            return std::string("missing ") + c;
        }
    }

    const auto [shdr, shdr_size] = v.chunks.at("shdr");
    const auto [igen, igen_size] = v.chunks.at("igen");
    const auto [ibag, ibag_size] = v.chunks.at("ibag");
    const auto [inst, inst_size] = v.chunks.at("inst");
    const uint32_t smpl_samples = v.chunks.at("smpl").second / 2;
    if (shdr_size % 46 || igen_size % 4 || ibag_size % 4 || inst_size % 22)
    {
        return "record sizes";
    }

    const uint32_t nsamples = shdr_size / 46 - 1;
    for (uint32_t i = 0; i < nsamples; i++)
    {
        const std::size_t o = shdr + i * 46;
        const uint32_t start = Le32(d, o + 20), end = Le32(d, o + 24);
        const uint32_t ls = Le32(d, o + 28), le = Le32(d, o + 32);
        if (!(start < end && end + 46 <= smpl_samples))
        {
            return "sample bounds";
        }
        if (le > ls && (ls < start || le > end))
        {
            return "loop bounds";
        }

        const uint16_t type = Le16(d, o + 44), link = Le16(d, o + 42);
        if (type == 2 || type == 4)
        {
            if (link >= nsamples || Le16(d, shdr + link * 46 + 42) != i || Le16(d, shdr + link * 46 + 44) != 6 - type)
            {
                return "stereo link";
            }
        }
        else if (type != 1)
        {
            return "sample type";
        }
    }

    const uint32_t ninst = inst_size / 22 - 1;
    for (uint32_t i = 0; i < ninst; i++)
    {
        const uint16_t b0 = Le16(d, inst + i * 22 + 20), b1 = Le16(d, inst + (i + 1) * 22 + 20);
        for (uint16_t b = b0; b < b1; b++)
        {
            const uint16_t g0 = Le16(d, ibag + b * 4), g1 = Le16(d, ibag + (b + 1) * 4);
            std::map<uint16_t, uint16_t> gens;
            for (uint16_t g = g0; g < g1; g++)
            {
                const uint16_t oper = Le16(d, igen + g * 4);
                if (oper == 43 && g != g0)
                {
                    return "keyRange not first";
                }
                if (oper == 44 && g != g0 + (Le16(d, igen + g0 * 4) == 43 ? 1 : 0))
                {
                    return "velRange out of place";
                }
                if (oper == 53 && g != g1 - 1)
                {
                    return "sampleID not last";
                }

                gens[oper] = Le16(d, igen + g * 4 + 2);
            }

            if (gens.count(53))
            {
                if (gens[53] >= nsamples)
                {
                    return "sampleID out of range";
                }

                if (gens.count(54) && (gens[54] & 1))
                {
                    const std::size_t o = shdr + gens[53] * 46;
                    const uint32_t start = Le32(d, o + 20), end = Le32(d, o + 24);
                    const uint32_t ls = Le32(d, o + 28), le = Le32(d, o + 32);
                    if (ls - start < 8 || end - le < 8 || le - ls < 32)
                    {
                        return "loop rules";
                    }
                }
            }
        }
    }

    return "";
}

// A parsed MIDI note: track, channel, key and start/end ticks.
struct MidiNote
{
    int track = 0, channel = 0, key = 0;
    uint32_t on = 0, off = 0;
};

// Parsed MIDI track names, end ticks, notes and markers.
struct MidiView
{
    bool ok = false;
    uint16_t division = 0;
    std::vector<std::string> track_names;
    std::vector<uint64_t> track_ends;
    std::vector<MidiNote> notes;
    std::vector<std::pair<uint32_t, std::string>> markers;
};

// Read generated MIDI. Set `ok` false for malformed data, deltas over four bytes, running status or overlapping
// same-key notes on one channel.
MidiView ParseMidi(const std::vector<uint8_t>& d)
{
    MidiView v;
    if (d.size() < 14 || std::memcmp(d.data(), "MThd", 4) != 0 || Be32(d, 4) != 6)
    {
        return v;
    }

    const uint16_t ntracks = static_cast<uint16_t>((d[10] << 8) | d[11]);
    v.division = static_cast<uint16_t>((d[12] << 8) | d[13]);
    std::size_t off = 14;
    for (uint16_t t = 0; t < ntracks; t++)
    {
        if (off + 8 > d.size() || std::memcmp(d.data() + off, "MTrk", 4) != 0)
        {
            return v;
        }

        const uint32_t size = Be32(d, off + 4);
        std::size_t p = off + 8;
        const std::size_t end = p + size;
        if (end > d.size())
        {
            return v;
        }

        uint64_t tick = 0;
        std::map<std::pair<int, int>, uint32_t> on;
        std::string name;
        bool ended = false;
        while (p < end)
        {
            uint32_t delta = 0;
            uint8_t b;
            int length = 0;
            do
            {
                b = d[p++];
                delta = (delta << 7) | (b & 0x7f);
            } while ((b & 0x80) && ++length < 4);
            if (b & 0x80)
            {
                return v; // a delta time of more than 4 bytes
            }

            tick += delta;

            const uint8_t status = d[p++];
            if (status == 0xff)
            {
                const uint8_t type = d[p++];
                uint32_t len = 0;
                do
                {
                    b = d[p++];
                    len = (len << 7) | (b & 0x7f);
                } while (b & 0x80);

                const std::string text(reinterpret_cast<const char*>(d.data() + p), len);
                if (type == 0x03)
                {
                    name = text;
                }
                if (type == 0x06)
                {
                    v.markers.push_back({static_cast<uint32_t>(tick), text});
                }
                if (type == 0x2f)
                {
                    ended = true;
                }

                p += len;
                continue;
            }

            const int kind = status >> 4, ch = status & 15;
            if (kind < 8)
            {
                return v; // no running status is written
            }

            const int nbytes = (kind == 0xc || kind == 0xd) ? 1 : 2;
            const uint8_t a = d[p], bb = nbytes == 2 ? d[p + 1] : 0;
            p += nbytes;
            if (kind == 9 && bb > 0)
            {
                if (on.count({ch, a}))
                {
                    return v; // a key on twice on one channel
                }

                on[{ch, a}] = static_cast<uint32_t>(tick);
            }
            else if (kind == 8 || (kind == 9 && bb == 0))
            {
                auto it = on.find({ch, a});
                if (it != on.end())
                {
                    v.notes.push_back({t, ch, a, it->second, static_cast<uint32_t>(tick)});
                    on.erase(it);
                }
            }
        }
        if (!ended || p != end || !on.empty())
        {
            return v;
        }

        v.track_names.push_back(name);
        v.track_ends.push_back(tick);
        off = end;
    }

    v.ok = off == d.size();

    return v;
}

void TestDspAdpcmScalesNibbles()
{
    // Header 0x12: scale 1 << 2, coefficient pair 1. With zero coefficients a sample is nibble * scale.
    const int16_t kCoefs[16] = {};
    const std::vector<uint8_t> kFrame = {0x12, 0x12, 0x34, 0xf0, 0x00, 0x00, 0x00, 0x00};
    const int16_t kExpect[6] = {4, 8, 12, 16, -4, 0};

    const auto out = DecodeDspAdpcm(kFrame, 6, kCoefs, 0, 0);

    CITRUSF2_CHECK(out.size() == 6);
    for (int i = 0; i < 6 && i < static_cast<int>(out.size()); i++)
    {
        CITRUSF2_CHECK(out[i] == kExpect[i]);
    }
}

void TestDspAdpcmPredictsFromHistory()
{
    // Coefficient pair 0 = (2048, 0) repeats the previous sample.
    const int16_t kHold[16] = {2048, 0};

    const auto held = DecodeDspAdpcm(std::vector<uint8_t>{0x00, 0x00, 0, 0, 0, 0, 0, 0}, 3, kHold, 100, 0);

    CITRUSF2_CHECK(held.size() == 3 && held[0] == 100 && held[2] == 100);
}

void TestDspAdpcmClampsLargePredictions()
{
    // Two coefficients and two history samples at -32768 sum to 2^31 before division by 2048, overflowing int32_t.
    const int16_t kCoefs[16] = {-32768, -32768};

    const auto out = DecodeDspAdpcm(std::vector<uint8_t>{0x00, 0x00, 0, 0, 0, 0, 0, 0}, 1, kCoefs, -32768, -32768);

    CITRUSF2_CHECK(out.size() == 1 && out[0] == 32767);
}

// Convert a string to bytes.
std::vector<uint8_t> Bytes(const std::string& s)
{
    return std::vector<uint8_t>(s.begin(), s.end());
}

// Convert bytes to a string for comparison.
std::string Text(std::span<const uint8_t> data)
{
    return std::string(data.begin(), data.end());
}

// Little-endian writer with a growing buffer.
struct Writer
{
    void U16(std::size_t at, uint32_t v)
    {
        bytes[at] = static_cast<uint8_t>(v);
        bytes[at + 1] = static_cast<uint8_t>(v >> 8);
    }

    void U32(std::size_t at, uint32_t v)
    {
        U16(at, v & 0xffff);
        U16(at + 2, v >> 16);
    }

    // Append `n` zero bytes and return their offset.
    std::size_t Add(std::size_t n)
    {
        bytes.resize(bytes.size() + n);

        return bytes.size() - n;
    }

    // Append `data` and return its offset.
    std::size_t Add(std::span<const uint8_t> data)
    {
        const std::size_t at = Add(data.size());
        std::copy(data.begin(), data.end(), bytes.begin() + static_cast<std::ptrdiff_t>(at));

        return at;
    }

    std::vector<uint8_t> bytes;
};

// Build a CTR header with `magic` and space for `blocks` block entries. The caller fills the entries.
Writer BinaryFileHeader(const char* magic, int blocks)
{
    Writer w;
    w.Add(0x40);
    std::memcpy(w.bytes.data(), magic, 4);
    w.U16(0x04, 0xfeff);
    w.U16(0x06, 0x40);
    w.U16(0x10, static_cast<uint32_t>(blocks));

    return w;
}

// Fill block table entry `i` and write the block's magic and size.
void Block(Writer& w, int i, uint16_t type, const char* magic, std::size_t at, std::size_t size)
{
    w.U16(0x14 + i * 12, type);
    w.U32(0x18 + i * 12, static_cast<uint32_t>(at));
    w.U32(0x1c + i * 12, static_cast<uint32_t>(size));
    std::memcpy(w.bytes.data() + at, magic, 4);
    w.U32(at + 4, static_cast<uint32_t>(size));
}

// Build a PCM16 CWAV declaring `length` samples at `sample_rate`. Each of `channels` channels uses `samples`. If
// `loops` is set, loop the entire wave.
std::vector<uint8_t> Pcm16Cwav(uint32_t length, const std::vector<int16_t>& samples, uint32_t channels = 1,
                               bool loops = false, uint32_t sample_rate = 32728)
{
    Writer w = BinaryFileHeader("CWAV", 2);

    // INFO: PCM16, optional loop from sample 0, and channel references sharing one channel. Samples start at the DATA
    // payload; no ADPCM metadata.
    const std::size_t info = w.Add(8 + 0x18 + channels * 8 + 16), body = info + 8, table = body + 0x14;
    const std::size_t channel = table + 4 + channels * 8;
    w.bytes[body] = 1;
    w.bytes[body + 1] = loops ? 1 : 0;
    w.U32(body + 4, sample_rate);
    w.U32(body + 12, length);
    w.U32(table, channels);
    for (uint32_t c = 0; c < channels; c++)
    {
        w.U32(table + 8 + c * 8, static_cast<uint32_t>(channel - table));
    }
    Block(w, 0, 0x7000, "INFO", info, w.bytes.size() - info);

    const std::size_t data = w.Add(8);
    for (const int16_t s : samples)
    {
        w.U16(w.Add(2), static_cast<uint16_t>(s));
    }
    Block(w, 1, 0x7001, "DATA", data, w.bytes.size() - data);

    return w.bytes;
}

// Build a CBNK with `instruments` references to one instrument. Its 128 key entries share a key region, whose 128
// velocity entries share a velocity region: 16512 regions per instrument from about 2 KB of data.
std::vector<uint8_t> SharedRegionsCbnk(uint32_t instruments)
{
    Writer w = BinaryFileHeader("CBNK", 1);
    const std::size_t info = w.Add(8 + 16), body = info + 8;
    const std::size_t waves = w.Add(4);
    const std::size_t table = w.Add(4 + instruments * 8);
    const std::size_t instrument = w.Add(8);
    const std::size_t keys = w.Add(4 + 128 * 8);
    const std::size_t key = w.Add(8);
    const std::size_t velocities = w.Add(4 + 128 * 8);
    const std::size_t region = w.Add(8); // wave 0, with no options

    // INFO: references to the wave table, which is empty, and to the instrument table.
    w.U16(body, 0x0100);
    w.U32(body + 4, static_cast<uint32_t>(waves - body));
    w.U16(body + 8, 0x0101);
    w.U32(body + 12, static_cast<uint32_t>(table - body));
    w.U32(table, instruments);
    for (uint32_t i = 0; i < instruments; i++)
    {
        w.U16(table + 4 + i * 8, 0x5900);
        w.U32(table + 8 + i * 8, static_cast<uint32_t>(instrument - table));
    }

    // Point the reference at `from` to an index table at `at`. All 128 table entries point to `to`.
    auto index_table = [&](std::size_t from, std::size_t at, std::size_t to)
    {
        w.U16(from, 0x6002);
        w.U32(from + 4, static_cast<uint32_t>(at - from));
        w.bytes[at + 1] = 127;
        for (std::size_t k = 0; k < 128; k++)
        {
            w.U32(at + 8 + k * 8, static_cast<uint32_t>(to - at));
        }
    };
    index_table(instrument, keys, key);
    index_table(key, velocities, region);
    Block(w, 0, 0x5800, "INFO", info, w.bytes.size() - info);

    return w.bytes;
}

// Build a one-instrument CBNK with one region covering all keys and velocities. It plays wave table entry 0 at `volume`
// and `pan`. `waves` supplies (wave archive item ID, wave index) pairs.
std::vector<uint8_t> OneRegionCbnk(const std::vector<std::pair<uint32_t, uint32_t>>& waves, uint8_t volume = 127,
                                   uint8_t pan = 64)
{
    Writer w = BinaryFileHeader("CBNK", 1);
    const std::size_t info = w.Add(8 + 16), body = info + 8;
    const std::size_t wave_table = w.Add(4 + waves.size() * 8);
    const std::size_t table = w.Add(12);
    const std::size_t instrument = w.Add(48);

    // INFO: references to the wave table and the instrument table.
    w.U16(body, 0x0100);
    w.U32(body + 4, static_cast<uint32_t>(wave_table - body));
    w.U16(body + 8, 0x0101);
    w.U32(body + 12, static_cast<uint32_t>(table - body));
    w.U32(wave_table, static_cast<uint32_t>(waves.size()));
    for (std::size_t i = 0; i < waves.size(); i++)
    {
        w.U32(wave_table + 4 + i * 8, waves[i].first);
        w.U32(wave_table + 8 + i * 8, waves[i].second);
    }
    w.U32(table, 1);
    w.U16(table + 4, 0x5900);
    w.U32(table + 8, static_cast<uint32_t>(instrument - table));

    // Instrument and key region each reference a direct table 8 bytes ahead, which points another 8 bytes ahead to the
    // next region. The velocity region plays wave 0 with volume and pan as its only options.
    for (const std::size_t at : {instrument, instrument + 16})
    {
        w.U16(at, 0x6000);
        w.U32(at + 4, 8);
        w.U32(at + 12, 8);
    }

    w.U32(instrument + 36, (1 << 1) | (1 << 2));
    w.U32(instrument + 40, volume);
    w.U32(instrument + 44, pan);
    Block(w, 0, 0x5800, "INFO", info, w.bytes.size() - info);

    return w.bytes;
}

// Build a CWAR containing CWAV files `waves`.
std::vector<uint8_t> Cwar(const std::vector<std::vector<uint8_t>>& waves)
{
    Writer w = BinaryFileHeader("CWAR", 2);
    const std::size_t info = w.Add(12 + waves.size() * 12);
    w.U32(info + 8, static_cast<uint32_t>(waves.size()));
    Block(w, 0, 0x6800, "INFO", info, w.bytes.size() - info);

    const std::size_t file = w.Add(8);
    for (std::size_t i = 0; i < waves.size(); i++)
    {
        w.U32(info + 16 + i * 12, static_cast<uint32_t>(w.Add(waves[i]) - file - 8));
        w.U32(info + 20 + i * 12, static_cast<uint32_t>(waves[i].size()));
    }
    Block(w, 1, 0x6801, "FILE", file, w.bytes.size() - file);

    return w.bytes;
}

// Build a CGRP with embedded files `held` (ID, contents) and references to non-embedded files `named`.
std::vector<uint8_t> Cgrp(const std::vector<std::pair<uint32_t, std::string>>& held, const std::vector<uint32_t>& named)
{
    Writer w = BinaryFileHeader("CGRP", 2);
    const std::size_t n = held.size() + named.size();
    const std::size_t info = w.Add(12 + n * 24), body = info + 8;
    w.U32(body, static_cast<uint32_t>(n));
    for (std::size_t i = 0; i < n; i++)
    {
        const std::size_t e = 4 + n * 8 + i * 16; // from the start of the table
        w.U16(body + 4 + i * 8, 0x7900);
        w.U32(body + 8 + i * 8, static_cast<uint32_t>(e));
        w.U32(body + e, i < held.size() ? held[i].first : named[i - held.size()]);
        w.U32(body + e + 8, 0xffffffff); // a null reference until a file is placed
        w.U32(body + e + 12, 0xffffffff);
    }
    Block(w, 0, 0x7800, "INFO", info, w.bytes.size() - info);

    const std::size_t file = w.Add(8);
    for (std::size_t i = 0; i < held.size(); i++)
    {
        const std::size_t e = body + 4 + n * 8 + i * 16;
        w.U16(e + 4, 0x1f00);
        w.U32(e + 8, static_cast<uint32_t>(w.Add(Bytes(held[i].second)) - file - 8));
        w.U32(e + 12, static_cast<uint32_t>(held[i].second.size()));
    }
    Block(w, 1, 0x7801, "FILE", file, w.bytes.size() - file);

    return w.bytes;
}

// A test file, either embedded or absent using one of the two supported location encodings.
struct ArchiveFile
{
    enum Place
    {
        kStored,
        kNullLocation, // a null reference instead of a location
        kNullOffset    // a location with an offset of 0xFFFFFFFF
    };

    Place place = kStored;
    std::vector<uint8_t> data;
    uint32_t extra_size = 0; // added to the size of a stored file in the file table
};

// Test sequence metadata: CSEQ file, first-track offset and bank slot item IDs.
struct SequenceSound
{
    uint32_t file_id = 0;
    uint32_t start_offset = 0;
    std::vector<uint32_t> banks = {};
    uint8_t pan_mode = 0, pan_curve = 0;
};

// Test archive tables: sequences, bank files, wave archives, group files and item names keyed by ID. Group file ID
// 0xFFFFFFFF means no file. Defaults allow partial designated initializers without warnings.
struct ArchiveLists
{
    std::vector<SequenceSound> sequences = {};
    std::vector<uint32_t> banks = {}, wave_archives = {}, groups = {};
    std::vector<std::pair<uint32_t, std::string>> names = {};
};

// Build a sound archive with `files` in order and metadata from `lists`.
std::vector<uint8_t> Csar(const std::vector<ArchiveFile>& files, const ArchiveLists& lists = {})
{
    Writer w = BinaryFileHeader("CSAR", lists.names.empty() ? 2 : 3);
    w.U32(0x08, 0x02030000);

    // INFO: eight table references followed by their tables. Sound sets and players are empty; player settings are
    // zero.
    const std::size_t info = w.Add(8 + 8 * 8), body = info + 8;
    const uint16_t kTables[8] = {0x2100, 0x2104, 0x2101, 0x2103, 0x2105, 0x2102, 0x2106, 0x220b};
    std::size_t file_table = 0;
    for (int i = 0; i < 8; i++)
    {
        const std::size_t t = w.Add(4);
        w.U16(body + i * 8, kTables[i]);
        w.U32(body + i * 8 + 4, static_cast<uint32_t>(t - body));
        if (kTables[i] == 0x2100)
        {
            // Sound entry: file, volume, sequence settings at +28, and optional pan mode/curve. Sequence settings point
            // to a bank table at +20, allocate track 0 and include the start-offset option.
            const std::vector<SequenceSound>& sequences = lists.sequences;
            w.U32(t, static_cast<uint32_t>(sequences.size()));
            const std::size_t refs = w.Add(sequences.size() * 8);
            for (std::size_t s = 0; s < sequences.size(); s++)
            {
                const SequenceSound& sound = sequences[s];
                const std::size_t e = w.Add(52 + sound.banks.size() * 4), q = e + 28;
                w.U16(refs + s * 8, 0x2200);
                w.U32(refs + s * 8 + 4, static_cast<uint32_t>(e - t));
                w.U32(e, sound.file_id);
                w.bytes[e + 8] = 127;
                w.U16(e + 12, 0x2203);
                w.U32(e + 16, static_cast<uint32_t>(q - e));
                w.U32(e + 20, 1 << 1);
                w.U32(e + 24, sound.pan_mode | (sound.pan_curve << 8));
                w.U32(q + 4, 20);
                w.U32(q + 8, 1);
                w.U32(q + 12, 1);
                w.U32(q + 16, sound.start_offset);
                w.U32(q + 20, static_cast<uint32_t>(sound.banks.size()));
                for (std::size_t b = 0; b < sound.banks.size(); b++)
                {
                    w.U32(q + 24 + b * 4, sound.banks[b]);
                }
            }
        }

        // Bank, wave archive and group entries contain file IDs with no options.
        const std::vector<uint32_t>* file_ids = kTables[i] == 0x2101   ? &lists.banks
                                                : kTables[i] == 0x2103 ? &lists.wave_archives
                                                : kTables[i] == 0x2105 ? &lists.groups
                                                                       : nullptr;
        if (file_ids)
        {
            w.U32(t, static_cast<uint32_t>(file_ids->size()));
            w.Add(file_ids->size() * 16);
            for (std::size_t k = 0; k < file_ids->size(); k++)
            {
                const std::size_t e = 4 + file_ids->size() * 8 + k * 8;
                w.U32(t + 8 + k * 8, static_cast<uint32_t>(e));
                w.U32(t + e, (*file_ids)[k]);
            }
        }
        if (kTables[i] == 0x2106)
        {
            file_table = t;
            w.U32(t, static_cast<uint32_t>(files.size()));
            w.Add(files.size() * 32);
        }
    }
    Block(w, 0, 0x2001, "INFO", info, w.bytes.size() - info);

    // FILE payload and matching locations in the file table.
    const std::size_t file = w.Add(8);
    for (std::size_t i = 0; i < files.size(); i++)
    {
        const std::size_t e = file_table + 4 + files.size() * 8 + i * 24; // location reference, options, location
        w.U16(file_table + 4 + i * 8, 0x220a);
        w.U32(file_table + 8 + i * 8, static_cast<uint32_t>(e - file_table));
        if (files[i].place == ArchiveFile::kNullLocation)
        {
            w.U32(e + 4, 0xffffffff);
            continue;
        }

        w.U16(e, 0x220c);
        w.U32(e + 4, 12);
        w.U32(e + 16, 0xffffffff);
        w.U32(e + 20, 0xffffffff);
        if (files[i].place == ArchiveFile::kStored)
        {
            w.U16(e + 12, 0x1f00);
            w.U32(e + 16, static_cast<uint32_t>(w.Add(files[i].data) - file - 8));
            w.U32(e + 20, static_cast<uint32_t>(files[i].data.size()) + files[i].extra_size);
        }
    }
    Block(w, 1, 0x2002, "FILE", file, w.bytes.size() - file);

    // STRG: references to the name table and Patricia tree. Only the leaves' name-to-item mappings are read.
    if (!lists.names.empty())
    {
        const std::size_t count = lists.names.size();
        const std::size_t strg = w.Add(8 + 16), refs = strg + 8;
        const std::size_t table = w.Add(4 + count * 12), tree = w.Add(8 + count * 20);
        w.U16(refs, 0x2400);
        w.U32(refs + 4, static_cast<uint32_t>(table - refs));
        w.U16(refs + 8, 0x2401);
        w.U32(refs + 12, static_cast<uint32_t>(tree - refs));
        w.U32(table, static_cast<uint32_t>(count));
        w.U32(tree + 4, static_cast<uint32_t>(count));
        for (std::size_t i = 0; i < count; i++)
        {
            const std::string& name = lists.names[i].second;
            const std::size_t at = w.Add(name.size() + 1), e = table + 4 + i * 12, node = tree + 8 + i * 20;
            std::memcpy(w.bytes.data() + at, name.data(), name.size());
            w.U16(e, 0x1f01);
            w.U32(e + 4, static_cast<uint32_t>(at - table));
            w.U32(e + 8, static_cast<uint32_t>(name.size() + 1));
            w.U16(node, 1); // a leaf
            w.U32(node + 12, static_cast<uint32_t>(i));
            w.U32(node + 16, lists.names[i].first);
        }
        Block(w, 2, 0x2000, "STRG", strg, w.bytes.size() - strg);
    }

    w.U32(0x0c, static_cast<uint32_t>(w.bytes.size()));

    return w.bytes;
}

// Build a CSEQ containing `commands` in DATA.
std::vector<uint8_t> Cseq(const std::vector<uint8_t>& commands)
{
    Writer w = BinaryFileHeader("CSEQ", 1);
    const std::size_t data = w.Add(8);
    w.Add(commands);
    Block(w, 0, 0x5000, "DATA", data, w.bytes.size() - data);

    return w.bytes;
}

// Group test layout: file 0 is embedded directly; files 1 and 2 are CGRP groups stored after it. Files 3-5 have no
// direct location. Both groups hold different copies of file 3; only the second holds file 4; neither holds file 5. The
// first also references file 0 without embedding it.
std::vector<ArchiveFile> GroupTestFiles(const char* first_group_magic = "CGRP")
{
    std::vector<ArchiveFile> files(6);
    files[0].data = Bytes("stored");
    files[1].data = Cgrp({{3, "first copy"}}, {0});
    std::memcpy(files[1].data.data(), first_group_magic, 4);
    files[2].data = Cgrp({{3, "second copy"}, {4, std::string(64, 'x')}}, {});
    files[3].place = ArchiveFile::kNullLocation;
    files[4].place = ArchiveFile::kNullOffset;
    files[5].place = ArchiveFile::kNullLocation;

    return files;
}

void TestFilesKeptInGroupsAreFound()
{
    // The third group has no file.
    const SoundArchive a = SoundArchive::Load(Csar(GroupTestFiles(), {.groups = {1, 2, 0xffffffff}}));

    CITRUSF2_CHECK(Text(a.FileData(0)) == "stored");
    CITRUSF2_CHECK(Text(a.FileData(3)) == "first copy");
    CITRUSF2_CHECK(Text(a.FileData(4)) == std::string(64, 'x'));
    CITRUSF2_CHECK(a.FileData(5).empty() && !a.FileMissing(5));
    CITRUSF2_CHECK(a.GroupErrors().empty() && !a.Truncated());
}

void TestBrokenGroupIsReported()
{
    const SoundArchive a = SoundArchive::Load(Csar(GroupTestFiles("XGRP"), {.groups = {1, 2}}));

    CITRUSF2_CHECK(Text(a.FileData(3)) == "second copy");
    CITRUSF2_CHECK(
        a.GroupErrors() ==
        std::vector<std::string>{
            "group GROUP_0 is unreadable (bad magic, expected CGRP); omitting files stored only in this group"});
}

void TestGroupPastTheEndLeavesItsFilesMissing()
{
    // Truncate halfway through file 4: the second group remains partly readable. Truncate at the group start: its
    // contents are unknown, so any file absent from the first group might be there.
    const std::vector<uint8_t> bytes = Csar(GroupTestFiles(), {.groups = {1, 2}});
    for (const std::size_t cut : {std::size_t{32}, GroupTestFiles()[2].data.size()})
    {
        const SoundArchive a =
            SoundArchive::Load(std::vector<uint8_t>(bytes.begin(), bytes.end() - static_cast<std::ptrdiff_t>(cut)));

        CITRUSF2_CHECK(a.Truncated() && a.GroupErrors().empty());
        CITRUSF2_CHECK(Text(a.FileData(3)) == "first copy");
        CITRUSF2_CHECK(a.FileData(4).empty() && a.FileMissing(4));
        CITRUSF2_CHECK(a.FileMissing(5));
    }
}

void TestFilePastTheEndOfItsGroupIsReported()
{
    // Truncate the second CGRP halfway through file 4, while leaving the archive itself complete.
    std::vector<ArchiveFile> files = GroupTestFiles();
    files[2].data.resize(files[2].data.size() - 32);

    const SoundArchive a = SoundArchive::Load(Csar(files, {.groups = {1, 2}}));

    CITRUSF2_CHECK(Text(a.FileData(3)) == "first copy");
    CITRUSF2_CHECK(a.FileData(4).empty() && !a.FileMissing(4) && !a.Truncated());
    CITRUSF2_CHECK(a.GroupErrors() ==
                   std::vector<std::string>{"group GROUP_1: file 4 extends past the group; skipped"});
}

void TestFileCutShortInOneGroupIsTakenFromAnother()
{
    // Truncate the first group's copy of file 3; the second group still has a complete copy.
    std::vector<ArchiveFile> files = GroupTestFiles();
    files[1].data.resize(files[1].data.size() - 5);

    const SoundArchive a = SoundArchive::Load(Csar(files, {.groups = {1, 2}}));

    CITRUSF2_CHECK(Text(a.FileData(3)) == "second copy" && a.GroupErrors().empty());
}

void TestFilePastTheEndOfAWholeArchiveIsntTruncation()
{
    // Give a file an oversized length without truncating the archive itself.
    std::vector<ArchiveFile> files = GroupTestFiles();
    files[0].extra_size = 1000000;

    const SoundArchive a = SoundArchive::Load(Csar(files, {.groups = {1, 2}}));

    CITRUSF2_CHECK(a.FileData(0).empty() && a.FileMissing(0) && !a.Truncated());
    CITRUSF2_CHECK(Text(a.FileData(3)) == "first copy");
}

void TestTruncatedArchiveWhoseTablesCantBeReadIsReported()
{
    // Truncate within INFO's table references, then within the file's block table.
    const std::vector<uint8_t> bytes = Csar(GroupTestFiles(), {.groups = {1, 2}});
    auto message_for = [&](std::ptrdiff_t size)
    {
        try
        {
            SoundArchive::Load(std::vector<uint8_t>(bytes.begin(), bytes.begin() + size));
        }
        catch (const FormatError& e)
        {
            return std::string(e.what());
        }

        return std::string();
    };

    const std::string in_info = message_for(0x60);
    const std::string in_block_table = message_for(0x20);

    const std::string size = std::to_string(bytes.size());
    CITRUSF2_CHECK(in_info ==
                   "truncated archive: 96 of " + size + " bytes; cannot read tables (read out of bounds at offset 96)");
    CITRUSF2_CHECK(in_block_table.starts_with("truncated archive: 32 of " + size +
                                              " bytes; cannot read tables (read out of bounds"));
}

void TestUnreadableGroupTableIsReported()
{
    // Make INFO's fifth reference (groups) point outside the archive. References start at 0x48, after the file and
    // block headers.
    Writer w{Csar(GroupTestFiles(), {.groups = {1, 2}})};
    w.U32(0x48 + 4 * 8 + 4, 0x7ffffff0);

    const SoundArchive a = SoundArchive::Load(w.bytes);

    CITRUSF2_CHECK(Text(a.FileData(0)) == "stored");
    CITRUSF2_CHECK(a.FileData(3).empty() && !a.FileMissing(3));
    const std::string kError =
        "cannot read group table (read out of bounds at offset 2147483704); files stored only in groups may be "
        "missing";
    CITRUSF2_CHECK(a.GroupErrors() == std::vector<std::string>{kError});
}

void TestBrokenBankAndWaveReferencesAreReported()
{
    // Banks 0-4 all play their wave entry 0. Test these references: bank 0 has no entry; bank 1 selects missing wave 1
    // in WARC_0; bank 2 selects absent wave archive 9; bank 3 uses the valid no-wave sentinel 0xFFFFFFFF; bank 4
    // references item 0xFFFFFFFF, which is not a wave archive. Sound 0 uses banks 0-2 and missing bank 99; sound 1 uses
    // banks 3-4.
    std::vector<ArchiveFile> files(6);
    files[0].data = OneRegionCbnk({});
    files[1].data = OneRegionCbnk({{0x05000000, 1}});
    files[2].data = OneRegionCbnk({{0x05000009, 0}});
    files[3].data = OneRegionCbnk({{0x05000000, 0xffffffff}});
    files[4].data = OneRegionCbnk({{0xffffffff, 0}});
    files[5].data = Cwar({Pcm16Cwav(1, {0})});
    const SoundArchive archive = SoundArchive::Load(Csar(files, {.banks = {0, 1, 2, 3, 4}, .wave_archives = {5}}));
    SoundInfo first;
    first.banks = {0x03000000, 0x03000001, 0x03000002, 0x03000063};
    SoundInfo second;
    second.banks = {0x03000003, 0x03000004};

    BankSet banks(archive, first);
    BankSet more_banks(archive, second);
    const bool no_waves = !banks.GetWave(0, 0) && !banks.GetWave(1, 0) && !banks.GetWave(2, 0) &&
                          !more_banks.GetWave(0, 0) && !more_banks.GetWave(1, 0);
    banks.GetWave(0, 0); // repeat the lookup without reporting the error again

    CITRUSF2_CHECK(no_waves && banks.FindRegion(0, 0, 60, 100) && more_banks.NoWave(0, 0) && !more_banks.NoWave(1, 0));
    const std::vector<std::string> kErrors = {"bank slot 3 has 0x03000063: invalid bank ID",
                                              "bank BANK_0 has no wave 0", "wave archive WARC_0 has no wave 1",
                                              "wave archive 9 doesn't exist (the archive has 1)"};
    const std::vector<std::string> kMoreErrors = {"bank BANK_4's wave 0 is in 0xffffffff: invalid wave archive ID"};
    CITRUSF2_CHECK(banks.Errors() == kErrors && more_banks.Errors() == kMoreErrors);
}

void TestSequenceStartingPastItsEndPlaysNothing()
{
    // Start track 0 at 2^32 - 2, beyond the bytecode, so it finishes immediately. Checking for alloctrack must not wrap
    // to the command at the start of the data.
    std::vector<ArchiveFile> files(1);
    files[0].data = Cseq({0xfe, 0x00, 0x03, 0xff});
    const SoundArchive archive = SoundArchive::Load(Csar(files, {.sequences = {{0, 0xfffffffe}}}));
    BankSet banks(archive, archive.Sounds()[0]);

    const Performance p = Perform(archive, 0, banks, {});

    CITRUSF2_CHECK(p.end_tick == 0 && p.presets.empty() && p.warnings.empty());
}

void TestBankSlotPastTheFourthHasNoBank()
{
    // Select a bank slot and play key 60 for 48 ticks. All four valid slots contain the same bank. Higher slots are
    // silent, matching 3SF's model of nw::snd's out-of-bounds lookup.
    std::vector<ArchiveFile> files(4);
    files[0].data = Cseq({0xb6, 0x03, 0x3c, 0x64, 0x30, 0xff});
    files[1].data = Cseq({0xb6, 0x05, 0x3c, 0x64, 0x30, 0xff});
    files[2].data = OneRegionCbnk({{0x05000000, 0}});
    files[3].data = Cwar({Pcm16Cwav(100, std::vector<int16_t>(100, 1000))});
    const std::vector<uint32_t> kBanks(4, 0x03000000);
    const SoundArchive archive = SoundArchive::Load(
        Csar(files, {.sequences = {{0, 0, kBanks}, {1, 0, kBanks}}, .banks = {2}, .wave_archives = {3}}));
    BankSet banks(archive, archive.Sounds()[0]);

    const Performance fourth = Perform(archive, 0, banks, {});
    const Performance fifth = Perform(archive, 1, banks, {});

    CITRUSF2_CHECK(NoteCount(fourth) == 1 && NoteCount(fifth) == 0 && fifth.presets.empty());
}

void TestOnlyAJumpBackToPlayedCommandsIsALoop()
{
    // First sequence: skip a FIN, wait one tick and jump back to the unvisited FIN. Second: wait one tick and loop to
    // the start until the performance reaches 10 s.
    std::vector<ArchiveFile> files(2);
    files[0].data = Cseq({0x89, 0x00, 0x00, 0x05, 0xff, 0x80, 0x01, 0x89, 0x00, 0x00, 0x04});
    files[1].data = Cseq({0x80, 0x01, 0x89, 0x00, 0x00, 0x00});
    const SoundArchive archive = SoundArchive::Load(Csar(files, {.sequences = {{0, 0}, {1, 0}}}));
    BankSet banks(archive, archive.Sounds()[0]);

    const Performance once = Perform(archive, 0, banks, {});
    const Performance looping = Perform(archive, 1, banks, {});

    CITRUSF2_CHECK(!once.loop && once.loops == 0);
    CITRUSF2_CHECK(looping.loop && looping.loop->start == 0 && looping.loop->end == 1);
    CITRUSF2_CHECK(looping.seconds >= 10.0 && looping.seconds < 10.1);
}

void TestMainLoopWaitsForAFadeTooLoudForMidi()
{
    // Track 0 starts a looping note at volume 255, fades to 128 over 2000 ticks, then waits forever. Track 1 loops
    // every 48 ticks. Track 2 plays at tick 1500 with volume and volume2 both 255, setting the peak above MIDI's limit.
    // Perform must scale CC11 on a second pass and wait for track 0's fade before marking the main loop on both passes,
    // even when CC11 clips on the first.
    std::vector<ArchiveFile> files(3);
    files[0].data = Cseq({0xfe, 0x00, 0x07, 0x88, 0x01, 0x00, 0x00, 0x18, 0x88, 0x02, 0x00, 0x00, 0x1e, 0xc1,
                          0xff, 0xa3, 0xc1, 0x80, 0x07, 0xd0, 0x3c, 0x7f, 0x00, 0xff, 0x80, 0x30, 0x89, 0x00,
                          0x00, 0x18, 0x80, 0x8b, 0x5c, 0xc1, 0xff, 0xd5, 0xff, 0x3c, 0x7f, 0x30, 0xff});
    files[1].data = OneRegionCbnk({{0x05000000, 0}});
    files[2].data = Cwar({Pcm16Cwav(64, std::vector<int16_t>(64, 1000), 1, true)});
    const SoundArchive archive =
        SoundArchive::Load(Csar(files, {.sequences = {{0, 0, {0x03000000}}}, .banks = {1}, .wave_archives = {2}}));
    BankSet banks(archive, archive.Sounds()[0]);

    const Performance p = Perform(archive, 0, banks, {});

    CITRUSF2_CHECK(p.loop && p.loop->start > 2000 && p.loop->end - p.loop->start == 48);
    CITRUSF2_CHECK(std::fabs(p.level_cut_db - 40.0 * std::log10(512.0 / 127.0)) < 0.001);
}

void TestMainLoopWaitsForALegatosFade()
{
    // Track 0 ties key 60 on a volume-32 region, with volume2 32 and volume 128. At tick 1 it fades to 250 over 1220
    // ticks, plays key 60 legato, then waits forever. Legato compensation raises CC11, giving it finer steps than the
    // underlying level. Track 1 loops a 12-tick key 72 and 12-tick wait. Its main loop must start after track 0's fade.
    std::vector<ArchiveFile> files(3);
    files[0].data = Cseq({0xfe, 0x00, 0x03, 0x88, 0x01, 0x00, 0x00, 0x1a, 0xc8, 0x01, 0xd5, 0x20,
                          0xc1, 0x80, 0x3c, 0x64, 0x01, 0xa3, 0xc1, 0xfa, 0x04, 0xc4, 0x3c, 0x64,
                          0x00, 0xff, 0x48, 0x64, 0x0c, 0x80, 0x0c, 0x89, 0x00, 0x00, 0x1a});
    files[1].data = OneRegionCbnk({{0x05000000, 0}}, 32);
    files[2].data = Cwar({Pcm16Cwav(64, std::vector<int16_t>(64, 1000), 1, true)});
    const SoundArchive archive =
        SoundArchive::Load(Csar(files, {.sequences = {{0, 0, {0x03000000}}}, .banks = {1}, .wave_archives = {2}}));
    BankSet banks(archive, archive.Sounds()[0]);

    const Performance p = Perform(archive, 0, banks, {});

    CITRUSF2_CHECK(p.loop && p.loop->start == 1224 && p.loop->end == 1248);
}

void TestLegatoLeavesOutTheRegionsVolume()
{
    // With volume2 64 and tie enabled, play key 60 at velocity 100 on a volume-64 region, then key 62 at 100 and key 64
    // at 127. nw::snd continues the channel using velocity gain without region volume. MIDI can continue the first
    // legato, but the velocity increase needs a new note. CC11 compensates for the zone's region volume by 127 / 64 in
    // power.
    std::vector<ArchiveFile> files(3);
    files[0].data = Cseq({0xd5, 0x40, 0xc8, 0x01, 0x3c, 0x64, 0x30, 0x3e, 0x64, 0x30, 0x40, 0x7f, 0x30, 0xff});
    files[1].data = OneRegionCbnk({{0x05000000, 0}}, 64);
    files[2].data = Cwar({Pcm16Cwav(64, std::vector<int16_t>(64, 1000), 1, true)});
    const SoundArchive archive =
        SoundArchive::Load(Csar(files, {.sequences = {{0, 0, {0x03000000}}}, .banks = {1}, .wave_archives = {2}}));
    BankSet banks(archive, archive.Sounds()[0]);

    const Performance p = Perform(archive, 0, banks, {});

    std::vector<std::pair<uint32_t, int>> expression;
    std::vector<std::tuple<uint32_t, int, int>> notes; // tick, key and velocity
    for (const TrackEvent& e : p.tracks[0])
    {
        if (e.kind == EventKind::kExpression)
        {
            expression.emplace_back(e.tick, e.value);
        }
        else if (e.kind == EventKind::kNoteOn)
        {
            notes.emplace_back(e.tick, e.key, e.value);
        }
    }

    const std::vector<std::pair<uint32_t, int>> kExpression = {{0, 64}, {48, 90}};
    const std::vector<std::tuple<uint32_t, int, int>> kNotes = {{0, 60, 100}, {96, 64, 127}};
    CITRUSF2_CHECK(expression == kExpression && notes == kNotes && p.level_cut_db == 0.0);
}

void TestLevelCutFitsTheRunThatsKept()
{
    // Tie key 60 at velocity 127 on a volume-32 region. At tick 24, change the filter and play key 62 at velocity 100.
    // The first pass uses a new preset/note and CC11 253 to cancel region attenuation (127 / 32 in power). With a
    // filter controller on the second pass, the MIDI note continues at 100 / 127 velocity; peak CC11 becomes 199.
    // Scaling must use that new peak.
    std::vector<ArchiveFile> files(3);
    files[0].data = Cseq({0xc8, 0x01, 0x3c, 0x7f, 0x18, 0xd8, 0x20, 0x3e, 0x64, 0x18, 0x80, 0x30, 0xff});
    files[1].data = OneRegionCbnk({{0x05000000, 0}}, 32);
    files[2].data = Cwar({Pcm16Cwav(64, std::vector<int16_t>(64, 1000), 1, true)});
    const SoundArchive archive =
        SoundArchive::Load(Csar(files, {.sequences = {{0, 0, {0x03000000}}}, .banks = {1}, .wave_archives = {2}}));
    BankSet banks(archive, archive.Sounds()[0]);

    const Performance p = Perform(archive, 0, banks, {});

    std::vector<std::pair<uint32_t, int>> expression;
    for (const TrackEvent& e : p.tracks[0])
    {
        if (e.kind == EventKind::kExpression)
        {
            expression.emplace_back(e.tick, e.value);
        }
    }

    const double kCutDb = 40.0 * std::log10(100.0 * std::sqrt(127.0 / 32.0) / 127.0);
    const std::vector<std::pair<uint32_t, int>> kExpression = {{0, 81}, {24, 127}};
    CITRUSF2_CHECK(expression == kExpression && std::fabs(p.level_cut_db - kCutDb) < 1e-9);
}

void TestTiedNoteTakesPartInTheMainLoop()
{
    // Track 0 opens track 1, ties key 72 for 30 ticks, then indefinitely, and waits for it. Track 1 loops a 26-tick key
    // 60. The tie at tick 30 emits no MIDI change but still contributes to track 1's pass starting at tick 26. Mark the
    // following pass as the main loop.
    std::vector<ArchiveFile> files(3);
    files[0].data = Cseq({0xfe, 0x00, 0x03, 0x88, 0x01, 0x00, 0x00, 0x10, 0xc8, 0x01, 0x48, 0x78,
                          0x1e, 0x48, 0x78, 0x00, 0x3c, 0x64, 0x1a, 0x89, 0x00, 0x00, 0x10});
    files[1].data = OneRegionCbnk({{0x05000000, 0}});
    files[2].data = Cwar({Pcm16Cwav(64, std::vector<int16_t>(64, 1000), 1, true)});
    const SoundArchive archive =
        SoundArchive::Load(Csar(files, {.sequences = {{0, 0, {0x03000000}}}, .banks = {1}, .wave_archives = {2}}));
    BankSet banks(archive, archive.Sounds()[0]);

    const Performance p = Perform(archive, 0, banks, {});

    CITRUSF2_CHECK(p.loop && p.loop->start == 52 && p.loop->end == 78);
}

void TestSilentLegatoCarriesItsNoteOn()
{
    // With mono enabled and notewait off, play key 60 for 30 ticks. Change release at tick 10, creating a different
    // preset, then play velocity 0 at tick 20 and finish at tick 50. Keep the MIDI note alive but silent through CC11;
    // note-off would leave a release tail. Do not create the unused preset.
    std::vector<ArchiveFile> files(3);
    files[0].data = Cseq({0xb2, 0x01, 0xc7, 0x00, 0x3c, 0x64, 0x1e, 0x80, 0x0a, 0xd3, 0x40, 0x80, 0x0a, 0x3c, 0x00,
                          0x3c, 0x80, 0x1e, 0xff});
    files[1].data = OneRegionCbnk({{0x05000000, 0}});
    files[2].data = Cwar({Pcm16Cwav(64, std::vector<int16_t>(64, 1000), 1, true)});
    const SoundArchive archive =
        SoundArchive::Load(Csar(files, {.sequences = {{0, 0, {0x03000000}}}, .banks = {1}, .wave_archives = {2}}));
    BankSet banks(archive, archive.Sounds()[0]);

    const Performance p = Perform(archive, 0, banks, {});

    std::vector<std::tuple<uint32_t, EventKind, int>> events; // tick, kind and CC11 value
    for (const TrackEvent& e : p.tracks[0])
    {
        if (e.kind == EventKind::kNoteOn || e.kind == EventKind::kNoteOff || e.kind == EventKind::kExpression)
        {
            events.emplace_back(e.tick, e.kind, e.kind == EventKind::kExpression ? e.value : 0);
        }
    }

    const std::vector<std::tuple<uint32_t, EventKind, int>> kEvents = {{0, EventKind::kExpression, 127},
                                                                       {0, EventKind::kNoteOn, 0},
                                                                       {20, EventKind::kExpression, 0},
                                                                       {50, EventKind::kNoteOff, 0}};
    CITRUSF2_CHECK(events == kEvents && p.presets.size() == 1);
}

void TestLegatosChangeOfKeyIsNoChangeOfTheTracks()
{
    // Track 0 opens track 1, sets tempo 1023, enables tie/portamento (time 100), and holds key 60. At tick 1, change
    // the low-pass during the note and play key 62 legato. The first pass creates a new MIDI note; the second uses a
    // filter controller and a 2-semitone bend on the existing note. Track 1 loops every 6 ticks. Sweep completion
    // counts as a track-0 change on both passes, even though only the first MIDI bend returns to 0. Both passes must
    // choose the same main loop.
    std::vector<ArchiveFile> files(3);
    files[0].data = Cseq({0xfe, 0x00, 0x03, 0x88, 0x01, 0x00, 0x00, 0x20, 0xe1, 0x03, 0xff, 0xc8, 0x01,
                          0xce, 0x01, 0xcf, 0x64, 0xc7, 0x00, 0x3c, 0x64, 0x00, 0x80, 0x01, 0xd8, 0x10,
                          0xc7, 0x01, 0x3e, 0x64, 0x00, 0xff, 0x80, 0x06, 0x89, 0x00, 0x00, 0x20});
    files[1].data = OneRegionCbnk({{0x05000000, 0}});
    files[2].data = Cwar({Pcm16Cwav(64, std::vector<int16_t>(64, 1000), 1, true)});
    const SoundArchive archive =
        SoundArchive::Load(Csar(files, {.sequences = {{0, 0, {0x03000000}}}, .banks = {1}, .wave_archives = {2}}));
    BankSet banks(archive, archive.Sounds()[0]);

    const Performance p = Perform(archive, 0, banks, {});

    CITRUSF2_CHECK(p.loop && p.loop->start == 2508 && p.loop->end == 2514);
}

void TestMonoLegatoTakesItsLengthWithTieOn()
{
    // With mono and tie enabled, play key 60 for 48 ticks, then key 62 for 24 ticks and wait. Mono updates the
    // channel's length even with tie enabled, so the note ends at tick 72.
    std::vector<ArchiveFile> files(3);
    files[0].data = Cseq({0xb2, 0x01, 0xc8, 0x01, 0x3c, 0x64, 0x30, 0x3e, 0x64, 0x18, 0x80, 0x60, 0xff});
    files[1].data = OneRegionCbnk({{0x05000000, 0}});
    files[2].data = Cwar({Pcm16Cwav(64, std::vector<int16_t>(64, 1000), 1, true)});
    const SoundArchive archive =
        SoundArchive::Load(Csar(files, {.sequences = {{0, 0, {0x03000000}}}, .banks = {1}, .wave_archives = {2}}));
    BankSet banks(archive, archive.Sounds()[0]);

    const Performance p = Perform(archive, 0, banks, {});

    std::vector<std::pair<uint32_t, EventKind>> notes;
    for (const TrackEvent& e : p.tracks[0])
    {
        if (e.kind == EventKind::kNoteOn || e.kind == EventKind::kNoteOff)
        {
            notes.emplace_back(e.tick, e.kind);
        }
    }

    const std::vector<std::pair<uint32_t, EventKind>> kNotes = {{0, EventKind::kNoteOn}, {72, EventKind::kNoteOff}};
    CITRUSF2_CHECK(notes == kNotes);
}

void TestNoteReleasedBeforeItsFirstFrameIsSilent()
{
    // With notewait off, play key 60 and enable mono to release it. An immediate release precedes the first envelope
    // update and stays silent; a release one tick later is audible.
    std::vector<ArchiveFile> files(4);
    files[0].data = Cseq({0xc7, 0x00, 0x3c, 0x64, 0x30, 0xb2, 0x01, 0x80, 0x30, 0xff});
    files[1].data = Cseq({0xc7, 0x00, 0x3c, 0x64, 0x30, 0x80, 0x01, 0xb2, 0x01, 0x80, 0x30, 0xff});
    files[2].data = OneRegionCbnk({{0x05000000, 0}});
    files[3].data = Cwar({Pcm16Cwav(64, std::vector<int16_t>(64, 1000), 1, true)});
    const std::vector<uint32_t> kBanks = {0x03000000};
    const SoundArchive archive = SoundArchive::Load(
        Csar(files, {.sequences = {{0, 0, kBanks}, {1, 0, kBanks}}, .banks = {2}, .wave_archives = {3}}));
    BankSet banks(archive, archive.Sounds()[0]);

    const Performance at_once = Perform(archive, 0, banks, {});
    const Performance later = Perform(archive, 1, banks, {});

    std::vector<std::pair<uint32_t, EventKind>> notes;
    for (const TrackEvent& e : later.tracks[0])
    {
        if (e.kind == EventKind::kNoteOn || e.kind == EventKind::kNoteOff)
        {
            notes.emplace_back(e.tick, e.kind);
        }
    }

    const std::vector<std::pair<uint32_t, EventKind>> kNotes = {{0, EventKind::kNoteOn}, {1, EventKind::kNoteOff}};
    CITRUSF2_CHECK(NoteCount(at_once) == 0 && notes == kNotes);
}

void TestWhatMidiCantSeparateIsReported()
{
    // First sequence: release a slow-release key 60 by enabling tie one tick later, then change volume during its
    // detached release tail. Second: combine left track pan with right region pan.
    std::vector<ArchiveFile> files(4);
    files[0].data = Cseq(
        {0xc7, 0x00, 0xd3, 0x40, 0x3c, 0x64, 0x60, 0x80, 0x01, 0xc8, 0x01, 0x80, 0x01, 0xc1, 0x40, 0x80, 0x30, 0xff});
    files[1].data = Cseq({0xc0, 0x20, 0x3c, 0x64, 0x30, 0xff});
    files[2].data = OneRegionCbnk({{0x05000000, 0}}, 127, 100);
    files[3].data = Cwar({Pcm16Cwav(64, std::vector<int16_t>(64, 1000), 1, true)});
    const std::vector<uint32_t> kBanks = {0x03000000};
    const SoundArchive archive = SoundArchive::Load(
        Csar(files, {.sequences = {{0, 0, kBanks}, {1, 0, kBanks}}, .banks = {2}, .wave_archives = {3}}));
    BankSet banks(archive, archive.Sounds()[0]);

    const Performance let_go = Perform(archive, 0, banks, {});
    const Performance panned = Perform(archive, 1, banks, {});

    const std::string kLetGo = "detached notes (still receive track controller changes in MIDI)";
    const std::string kPans = "combined region and track pan (using the synth pan law)";
    CITRUSF2_CHECK(let_go.approximations.contains(kLetGo) && !let_go.approximations.contains(kPans));
    CITRUSF2_CHECK(panned.approximations.contains(kPans) && !panned.approximations.contains(kLetGo));
}

void TestOneShotWaveEndsAsTheDspPlaysIt()
{
    // Play a 6399-sample one-shot at four times the DSP rate (640 samples/frame), then wait for completion. The DSP
    // position is 1 + (160 n - 1) * 4, adding 3 samples to the effective length. Playback takes 11 frames after
    // note-on, not 10. The channel disappears in frame 13 (63.5 ms); execution resumes at tick 7 (72.9 ms), the first
    // tick in that frame.
    std::vector<ArchiveFile> files(3);
    files[0].data = Cseq({0x3c, 0x7f, 0x00, 0xff});
    files[1].data = OneRegionCbnk({{0x05000000, 0}});
    files[2].data = Cwar({Pcm16Cwav(6399, std::vector<int16_t>(6399, 1000), 1, false, 4 * 32728)});
    const SoundArchive archive =
        SoundArchive::Load(Csar(files, {.sequences = {{0, 0, {0x03000000}}}, .banks = {1}, .wave_archives = {2}}));
    BankSet banks(archive, archive.Sounds()[0]);

    const Performance p = Perform(archive, 0, banks, {});

    std::vector<uint32_t> note_offs;
    for (const TrackEvent& e : p.tracks[0])
    {
        if (e.kind == EventKind::kNoteOff)
        {
            note_offs.push_back(e.tick);
        }
    }

    CITRUSF2_CHECK(note_offs == std::vector<uint32_t>{7} && p.end_tick == 7);
}

void TestTicksFallInTheGamesSoundFrames()
{
    // Wait 755 ticks at tempo 229. Single-precision scheduling in thousandths of ARM cycles puts tick 755 at the end of
    // frame 842; exact arithmetic puts it at the start of frame 843. MIDI events must use the fractional tick position
    // of the actual frame start.
    std::vector<ArchiveFile> files(1);
    files[0].data = Cseq({0xe1, 0x00, 0xe5, 0x80, 0x85, 0x73, 0xff});
    const SoundArchive archive = SoundArchive::Load(Csar(files, {.sequences = {{0, 0}}}));
    BankSet banks(archive, archive.Sounds()[0]);

    const Performance p = Perform(archive, 0, banks, {});

    const double frame_842 = 842 * kFrameMs / (60000.0 / (48 * 229));
    CITRUSF2_CHECK(p.frame_ticks.size() == 756 && std::fabs(p.frame_ticks[755] - frame_842) < 0.01);
}

void TestOddArgumentsAreReadAsTheGameReadsThem()
{
    // Test a volume ramp over 0x8000 ticks (negative in nw::snd's 16-bit counter, so immediate), a 128-tick wait
    // encoded in seven bytes (five would leave bytes interpreted as a program change), and timebase 0 stopping sequence
    // timing.
    std::vector<ArchiveFile> files(5);
    files[0].data = Cseq({0xa3, 0xc1, 0x40, 0x80, 0x00, 0x3c, 0x64, 0x30, 0xff});
    files[1].data = Cseq({0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x81, 0x00, 0x3c, 0x64, 0x30, 0xff});
    files[2].data = Cseq({0xb0, 0x00, 0x80, 0x30, 0xff});
    files[3].data = OneRegionCbnk({{0x05000000, 0}});
    files[4].data = Cwar({Pcm16Cwav(64, std::vector<int16_t>(64, 1000), 1, true)});
    const std::vector<uint32_t> kBanks = {0x03000000};
    const SoundArchive archive = SoundArchive::Load(Csar(
        files, {.sequences = {{0, 0, kBanks}, {1, 0, kBanks}, {2, 0, kBanks}}, .banks = {3}, .wave_archives = {4}}));
    BankSet banks(archive, archive.Sounds()[0]);

    const Performance ramp = Perform(archive, 0, banks, {});
    const Performance wait = Perform(archive, 1, banks, {});
    const Performance timebase = Perform(archive, 2, banks, {});

    std::vector<int> volumes;
    for (const TrackEvent& e : ramp.tracks[0])
    {
        if (e.kind == EventKind::kVolume)
        {
            volumes.push_back(e.value);
        }
    }

    auto note_on = [](const TrackEvent& e)
    {
        return e.kind == EventKind::kNoteOn;
    };
    const auto waited = std::ranges::find_if(wait.tracks[0], note_on);

    CITRUSF2_CHECK(volumes == std::vector<int>{64});
    CITRUSF2_CHECK(waited != wait.tracks[0].end() && waited->tick == 128);
    const std::vector<std::string> kStopped = {"timebase 0: the sequence stops advancing"};
    CITRUSF2_CHECK(timebase.warnings == kStopped && timebase.end_tick == 0 && timebase.timebase == 48);
}

void TestSequenceThatStopsAdvancingPlaysOut()
{
    // The first three sequences disable notewait, play key 60, wait and set tempo 0. A 96-tick note then holds
    // indefinitely and is capped at 10 s using MIDI's 120 BPM. A one-tick note at release 90 has already been released
    // and finishes normally. The third sequence also ends on the stopping tick. The latter two match a fourth sequence
    // with the same notes and no tempo 0.
    std::vector<ArchiveFile> files(6);
    files[0].data = Cseq({0xc7, 0x00, 0x3c, 0x64, 0x60, 0x80, 0x01, 0xe1, 0x00, 0x00, 0x80, 0x30, 0xff});
    files[1].data = Cseq({0xc7, 0x00, 0xd3, 0x5a, 0x3c, 0x64, 0x01, 0x80, 0x02, 0xe1, 0x00, 0x00, 0x80, 0x30, 0xff});
    files[2].data = Cseq({0xc7, 0x00, 0xd3, 0x5a, 0x3c, 0x64, 0x01, 0x80, 0x02, 0xe1, 0x00, 0x00, 0xff});
    files[3].data = Cseq({0xc7, 0x00, 0xd3, 0x5a, 0x3c, 0x64, 0x01, 0x80, 0x02, 0xff});
    files[4].data = OneRegionCbnk({{0x05000000, 0}});
    files[5].data = Cwar({Pcm16Cwav(64, std::vector<int16_t>(64, 1000), 1, true)});
    const std::vector<uint32_t> kBanks = {0x03000000};
    const std::vector<SequenceSound> kSounds = {{0, 0, kBanks}, {1, 0, kBanks}, {2, 0, kBanks}, {3, 0, kBanks}};
    const SoundArchive archive =
        SoundArchive::Load(Csar(files, {.sequences = kSounds, .banks = {4}, .wave_archives = {5}}));
    BankSet banks(archive, archive.Sounds()[0]);

    const Performance held = Perform(archive, 0, banks, {});
    const Performance released = Perform(archive, 1, banks, {});
    const Performance ending = Perform(archive, 2, banks, {});
    const Performance unstopped = Perform(archive, 3, banks, {});

    const std::vector<std::string> kStopped = {"tempo 0: the sequence stops advancing"};
    CITRUSF2_CHECK(held.warnings == kStopped && held.holds && held.end_tick == 961 &&
                   held.tracks[0].back().tick == 961);
    CITRUSF2_CHECK(released.warnings == kStopped && !released.holds && unstopped.end_tick > 200 &&
                   released.end_tick == unstopped.end_tick && ending.end_tick == unstopped.end_tick);
}

void TestTieLoopThatHoldsEndsAsAHeldSound()
{
    // The first sequence turns tie on, plays key 60 for 2400 ticks (25 s) and jumps back to the note, which continues
    // it: the sound holds forever, so it ends 10 s after the note starts, without loop markers. The second does the
    // same without tie, so each pass releases the note and strikes it again: a 25 s loop.
    std::vector<ArchiveFile> files(4);
    files[0].data = Cseq({0xc8, 0x01, 0x3c, 0x64, 0x92, 0x60, 0x89, 0x00, 0x00, 0x02});
    files[1].data = Cseq({0x3c, 0x64, 0x92, 0x60, 0x89, 0x00, 0x00, 0x00});
    files[2].data = OneRegionCbnk({{0x05000000, 0}});
    files[3].data = Cwar({Pcm16Cwav(64, std::vector<int16_t>(64, 1000), 1, true)});
    const std::vector<uint32_t> kBanks = {0x03000000};
    const SoundArchive archive = SoundArchive::Load(
        Csar(files, {.sequences = {{0, 0, kBanks}, {1, 0, kBanks}}, .banks = {2}, .wave_archives = {3}}));
    BankSet banks(archive, archive.Sounds()[0]);

    const Performance tied = Perform(archive, 0, banks, {});
    const Performance struck = Perform(archive, 1, banks, {});

    CITRUSF2_CHECK(tied.holds && !tied.loop && std::fabs(tied.seconds - 10.0) < 0.02);
    CITRUSF2_CHECK(!struck.holds && struck.loop && struck.loop->start == 0 && struck.loop->end == 2400);
}

void TestTieLoopOverAOneShotWaveStaysALoop()
{
    // The tied note of the test above, on a 30 s one-shot wave: it's still playing when the 25 s pass ends, but it ends
    // by itself, so the pass doesn't hold it forever.
    std::vector<ArchiveFile> files(3);
    files[0].data = Cseq({0xc8, 0x01, 0x3c, 0x64, 0x92, 0x60, 0x89, 0x00, 0x00, 0x02});
    files[1].data = OneRegionCbnk({{0x05000000, 0}});
    files[2].data = Cwar({Pcm16Cwav(30 * 32728, std::vector<int16_t>(30 * 32728, 1000))});
    const std::vector<uint32_t> kBanks = {0x03000000};
    const SoundArchive archive =
        SoundArchive::Load(Csar(files, {.sequences = {{0, 0, kBanks}}, .banks = {1}, .wave_archives = {2}}));
    BankSet banks(archive, archive.Sounds()[0]);

    const Performance p = Perform(archive, 0, banks, {});

    CITRUSF2_CHECK(!p.holds && p.loop && p.loop->end == 2400);
}

void TestClosingTickReachesTheReleaseTail()
{
    // Release 100 and key 60 for one tick, then volume 0 and FIN in the next. Closing the track gives the releasing
    // note the track's settings, as the end of a tick does, so the tail is silent in the game and needs CC7 0 in MIDI.
    std::vector<ArchiveFile> files(3);
    files[0].data = Cseq({0xd3, 0x64, 0x3c, 0x64, 0x01, 0xc1, 0x00, 0xff});
    files[1].data = OneRegionCbnk({{0x05000000, 0}});
    files[2].data = Cwar({Pcm16Cwav(64, std::vector<int16_t>(64, 1000), 1, true)});
    const std::vector<uint32_t> kBanks = {0x03000000};
    const SoundArchive archive =
        SoundArchive::Load(Csar(files, {.sequences = {{0, 0, kBanks}}, .banks = {1}, .wave_archives = {2}}));
    BankSet banks(archive, archive.Sounds()[0]);

    const Performance p = Perform(archive, 0, banks, {});

    const auto silenced = [](const TrackEvent& e)
    {
        return e.kind == EventKind::kVolume && e.value == 0 && e.tick == 1;
    };
    CITRUSF2_CHECK(std::ranges::any_of(p.tracks[0], silenced));
}

void TestSilenceAtTheTimeLimitEndsWithTheLastSound()
{
    // Key 60 plays for 48 ticks (0.5 s), then the track waits 100,000 ticks (17 minutes), past the 15-minute limit. It
    // has been silent since the note, so the performance ends with the note, as if the sequence had finished.
    std::vector<ArchiveFile> files(3);
    files[0].data = Cseq({0x3c, 0x64, 0x30, 0x80, 0x86, 0x8d, 0x20, 0xff});
    files[1].data = OneRegionCbnk({{0x05000000, 0}});
    files[2].data = Cwar({Pcm16Cwav(64, std::vector<int16_t>(64, 1000), 1, true)});
    const std::vector<uint32_t> kBanks = {0x03000000};
    const SoundArchive archive =
        SoundArchive::Load(Csar(files, {.sequences = {{0, 0, kBanks}}, .banks = {1}, .wave_archives = {2}}));
    BankSet banks(archive, archive.Sounds()[0]);

    const Performance p = Perform(archive, 0, banks, {});

    CITRUSF2_CHECK(!p.truncated && !p.holds && p.seconds < 1.0);
}

void TestOneShotWavePlaysOutAtItsPitch()
{
    // Bend key 60 up by 127/128 of a 12-semitone range, shortening a 4 s one-shot to about 2 s. Compare waiting for
    // completion with disabling notewait and setting tempo 0 immediately. The wave must finish at the same time.
    std::vector<ArchiveFile> files(4);
    files[0].data = Cseq({0xc5, 0x0c, 0xc4, 0x7f, 0x3c, 0x64, 0x00, 0xff});
    files[1].data = Cseq({0xc7, 0x00, 0xc5, 0x0c, 0xc4, 0x7f, 0x3c, 0x64, 0x00, 0xe1, 0x00, 0x00, 0x80, 0x30, 0xff});
    files[2].data = OneRegionCbnk({{0x05000000, 0}});
    files[3].data = Cwar({Pcm16Cwav(4 * 32728, std::vector<int16_t>(4 * 32728, 1000))});
    const std::vector<uint32_t> kBanks = {0x03000000};
    const SoundArchive archive = SoundArchive::Load(
        Csar(files, {.sequences = {{0, 0, kBanks}, {1, 0, kBanks}}, .banks = {2}, .wave_archives = {3}}));
    BankSet banks(archive, archive.Sounds()[0]);

    const Performance waited = Perform(archive, 0, banks, {});
    const Performance stopped = Perform(archive, 1, banks, {});

    CITRUSF2_CHECK(waited.seconds > 2.0 && waited.seconds < 2.1 && !stopped.holds &&
                   std::fabs(stopped.seconds - waited.seconds) < 0.011);
}

void TestTimedSweepGlidesOnWhenTheSequenceStops()
{
    // With notewait off, hold key 60 with about 1 s of portamento from key 48, then set tempo 0. The millisecond sweep
    // must continue. In the second sequence, hold key 60, play key 72 for one tick at release 127 with about 2 s of
    // portamento from key 48, then stop timing at tick 1. Key 72 has ended, so its sweep must stop too.
    std::vector<ArchiveFile> files(4);
    files[0].data = Cseq({0xc7, 0x00, 0xc9, 0x30, 0xcf, 0x17, 0x3c, 0x64, 0x00, 0xe1, 0x00, 0x00, 0x80, 0x30, 0xff});
    files[1].data = Cseq({0xc7, 0x00, 0x3c, 0x64, 0x00, 0xd3, 0x7f, 0xc9, 0x30, 0xcf, 0x17,
                          0x48, 0x64, 0x01, 0x80, 0x01, 0xe1, 0x00, 0x00, 0x80, 0x30, 0xff});
    files[2].data = OneRegionCbnk({{0x05000000, 0}});
    files[3].data = Cwar({Pcm16Cwav(64, std::vector<int16_t>(64, 1000), 1, true)});
    const std::vector<uint32_t> kBanks = {0x03000000};
    const SoundArchive archive = SoundArchive::Load(
        Csar(files, {.sequences = {{0, 0, kBanks}, {1, 0, kBanks}}, .banks = {2}, .wave_archives = {3}}));
    BankSet banks(archive, archive.Sounds()[0]);

    const Performance gliding = Perform(archive, 0, banks, {});
    const Performance ended = Perform(archive, 1, banks, {});

    auto pitch = [](const Performance& p)
    {
        std::vector<std::pair<uint32_t, float>> events;
        for (const TrackEvent& e : p.tracks[0])
        {
            if (e.kind == EventKind::kPitch)
            {
                events.emplace_back(e.tick, e.semitones);
            }
        }

        return events;
    };
    const auto glide = pitch(gliding);
    const auto after_end = pitch(ended);

    CITRUSF2_CHECK(gliding.holds && glide.size() > 50 && glide.front().second == -12.0f &&
                   glide.back().second == 0.0f && glide.back().first < 100);
    CITRUSF2_CHECK(ended.holds && !after_end.empty() && after_end.back().second == 0.0f && after_end.back().first <= 2);
}

void TestTrackWaitingForAVariableGoesOn()
{
    // Track 0 opens track 1, waits 96 ticks and sets player variable 0 to 1. Track 1 polls without waits, then plays
    // key 60. The 10,000-command limit must yield each tick so the note can start at tick 96.
    std::vector<ArchiveFile> files(3);
    files[0].data = Cseq({0xfe, 0x00, 0x03, 0x88, 0x01, 0x00, 0x00, 0x10, 0x80, 0x60, 0xf0, 0x80, 0x00, 0x00, 0x01,
                          0xff, 0xf0, 0x95, 0x00, 0x00, 0x01, 0xa2, 0x89, 0x00, 0x00, 0x10, 0x3c, 0x64, 0x30, 0xff});
    files[1].data = OneRegionCbnk({{0x05000000, 0}});
    files[2].data = Cwar({Pcm16Cwav(64, std::vector<int16_t>(64, 1000), 1, true)});
    const SoundArchive archive =
        SoundArchive::Load(Csar(files, {.sequences = {{0, 0, {0x03000000}}}, .banks = {1}, .wave_archives = {2}}));
    BankSet banks(archive, archive.Sounds()[0]);

    const Performance p = Perform(archive, 0, banks, {});

    std::vector<uint32_t> note_ons;
    for (const TrackEvent& e : p.tracks[1])
    {
        if (e.kind == EventKind::kNoteOn)
        {
            note_ons.push_back(e.tick);
        }
    }

    CITRUSF2_CHECK(note_ons == std::vector<uint32_t>{96} && p.warnings.empty());
}

void TestWaitingForAVariableIsNoLoop()
{
    // Track 1 waits 96 ticks and sets variable 0. Track 0 repeatedly sets volume and polls the variable without waits.
    // The 10,000-command limit stops partway through a pass, so later ticks jump back to commands visited on the
    // previous tick. These are polling passes, not the main loop. After the variable is set, track 0 loops a 48-tick
    // key 60; that is the main loop.
    std::vector<ArchiveFile> files(3);
    files[0].data = Cseq({0xfe, 0x00, 0x03, 0x88, 0x01, 0x00, 0x00, 0x1b, 0xc1, 0x7f, 0xf0, 0x95,
                          0x00, 0x00, 0x01, 0xa2, 0x89, 0x00, 0x00, 0x08, 0x3c, 0x64, 0x30, 0x89,
                          0x00, 0x00, 0x14, 0x80, 0x60, 0xf0, 0x80, 0x00, 0x00, 0x01, 0xff});
    files[1].data = OneRegionCbnk({{0x05000000, 0}});
    files[2].data = Cwar({Pcm16Cwav(64, std::vector<int16_t>(64, 1000), 1, true)});
    const SoundArchive archive =
        SoundArchive::Load(Csar(files, {.sequences = {{0, 0, {0x03000000}}}, .banks = {1}, .wave_archives = {2}}));
    BankSet banks(archive, archive.Sounds()[0]);

    const Performance p = Perform(archive, 0, banks, {});

    CITRUSF2_CHECK(p.loop && p.loop->start == 97 && p.loop->end == 145 && p.warnings.empty());
}

void TestTrackStartsNoMoreNotesATickThanTheGameHasVoices()
{
    // Test per-tick voice limits with notewait off: 30 simultaneous key-60 notes should yield 24 voices. With tie
    // enabled, 26 notes ending on key 72 use one channel; the final legato needs no approximation. Twenty-four notes on
    // missing program 5 allocate nothing, so a following held note on program 0 must play. Finally, 25 tied notes at
    // velocity 1 followed by velocity 127 must keep the existing MIDI note without reducing the sequence's level.
    std::vector<ArchiveFile> files(6);
    files[0].data = Cseq({0xc7, 0x00, 0xd4, 0x1e, 0x3c, 0x64, 0x30, 0xfc, 0x80, 0x30, 0xff});
    files[1].data =
        Cseq({0xc7, 0x00, 0xc8, 0x01, 0xd4, 0x19, 0x3c, 0x64, 0x30, 0xfc, 0x48, 0x64, 0x30, 0x80, 0x30, 0xff});
    files[2].data = Cseq(
        {0xc7, 0x00, 0x81, 0x05, 0xd4, 0x18, 0x3c, 0x64, 0x30, 0xfc, 0x81, 0x00, 0xc7, 0x01, 0x3c, 0x64, 0x00, 0xff});
    files[3].data = Cseq({0xc7, 0x00, 0xc8, 0x01, 0x3c, 0x01, 0x30, 0xd4, 0x18, 0x3c, 0x01, 0x30, 0xfc, 0x3c, 0x7f,
                          0x30, 0x80, 0x30, 0xff});
    files[4].data = OneRegionCbnk({{0x05000000, 0}});
    files[5].data = Cwar({Pcm16Cwav(64, std::vector<int16_t>(64, 1000), 1, true)});
    const std::vector<uint32_t> kBanks = {0x03000000};
    const std::vector<SequenceSound> kSounds = {{0, 0, kBanks}, {1, 0, kBanks}, {2, 0, kBanks}, {3, 0, kBanks}};
    const SoundArchive archive =
        SoundArchive::Load(Csar(files, {.sequences = kSounds, .banks = {4}, .wave_archives = {5}}));
    BankSet banks(archive, archive.Sounds()[0]);

    const Performance flood = Perform(archive, 0, banks, {});
    const Performance tied = Perform(archive, 1, banks, {});
    const Performance held = Perform(archive, 2, banks, {});
    const Performance loud = Perform(archive, 3, banks, {});

    auto note_ons = [](const Performance& p)
    {
        return std::ranges::count_if(p.tracks[0], [](const TrackEvent& e) { return e.kind == EventKind::kNoteOn; });
    };

    float tied_pitch = 0.0f;
    for (const TrackEvent& e : tied.tracks[0])
    {
        if (e.kind == EventKind::kPitch)
        {
            tied_pitch = e.semitones;
        }
    }

    const std::string kLegatosPastCap =
        "more than 24 legatos in a tick (keeping the MIDI preset and limiting gain to the starting level)";
    const auto left_out = flood.approximations.find(
        "more than 24 new channels on a track in one tick (excess omitted; the game keeps the newest)");
    CITRUSF2_CHECK(note_ons(flood) == 24 && left_out != flood.approximations.end() && left_out->second == 6);
    CITRUSF2_CHECK(note_ons(tied) == 1 && tied_pitch == 12.0f && !tied.approximations.contains(kLegatosPastCap));
    CITRUSF2_CHECK(note_ons(held) == 1 && held.holds);
    CITRUSF2_CHECK(note_ons(loud) == 1 && loud.level_cut_db == 0.0 && loud.approximations.contains(kLegatosPastCap));
}

void TestTrackPlayingANoteEachTimeAVariableIsSetGoesOn()
{
    // Track 0 sets variable 0 to 1 every 48 ticks. Track 1 polls without waits, clears it, plays key 60 and resumes
    // polling in the same tick. It should play once per update from track 0.
    std::vector<ArchiveFile> files(3);
    files[0].data = Cseq({0xfe, 0x00, 0x03, 0x88, 0x01, 0x00, 0x00, 0x13, 0x80, 0x30, 0xf0, 0x80, 0x00, 0x00, 0x01,
                          0x89, 0x00, 0x00, 0x08, 0xc7, 0x00, 0xf0, 0x95, 0x00, 0x00, 0x01, 0xa2, 0x89, 0x00, 0x00,
                          0x15, 0xf0, 0x80, 0x00, 0x00, 0x00, 0x3c, 0x64, 0x18, 0x89, 0x00, 0x00, 0x15});
    files[1].data = OneRegionCbnk({{0x05000000, 0}});
    files[2].data = Cwar({Pcm16Cwav(64, std::vector<int16_t>(64, 1000), 1, true)});
    const SoundArchive archive =
        SoundArchive::Load(Csar(files, {.sequences = {{0, 0, {0x03000000}}}, .banks = {1}, .wave_archives = {2}}));
    BankSet banks(archive, archive.Sounds()[0]);

    const Performance p = Perform(archive, 0, banks, {});

    std::vector<uint32_t> note_ons;
    for (const TrackEvent& e : p.tracks[1])
    {
        if (e.kind == EventKind::kNoteOn)
        {
            note_ons.push_back(e.tick);
        }
    }

    const bool every_48 = std::ranges::all_of(note_ons, [](uint32_t tick) { return tick % 48 == 0; });
    CITRUSF2_CHECK(p.warnings.empty() && note_ons.size() == 19 && note_ons.front() == 48 && every_48);
}

void TestTrackPlayingNotesWithoutWaitingIsStopped()
{
    // Loop a one-tick key 60 with notewait off: 5000 note commands per 10,000-command tick. Keep 24 voices per tick,
    // then stop the track after 10 s without a wait.
    std::vector<ArchiveFile> files(3);
    files[0].data = Cseq({0xc7, 0x00, 0x3c, 0x64, 0x01, 0x89, 0x00, 0x00, 0x02});
    files[1].data = OneRegionCbnk({{0x05000000, 0}});
    files[2].data = Cwar({Pcm16Cwav(64, std::vector<int16_t>(64, 1000), 1, true)});
    const SoundArchive archive =
        SoundArchive::Load(Csar(files, {.sequences = {{0, 0, {0x03000000}}}, .banks = {1}, .wave_archives = {2}}));
    BankSet banks(archive, archive.Sounds()[0]);

    const Performance p = Perform(archive, 0, banks, {});

    std::map<uint32_t, int> note_ons; // by tick
    for (const TrackEvent& e : p.tracks[0])
    {
        if (e.kind == EventKind::kNoteOn)
        {
            note_ons[e.tick]++;
        }
    }

    const std::vector<std::string> kStopped = {"track 0 stopped after running without waits"};
    const bool every_tick_24 = std::ranges::all_of(note_ons, [](const auto& tick) { return tick.second == 24; });
    CITRUSF2_CHECK(p.warnings == kStopped && every_tick_24 && note_ons.size() > 900 && note_ons.size() < 1000);
}

void TestSoundsOwnPanCurveIsReported()
{
    // Play key 60 with four sound settings: pan curve 2, balance mode on mono (no effect), balance mode on stereo, and
    // the default curve/mode.
    std::vector<ArchiveFile> files(4);
    files[0].data = Cseq({0x3c, 0x64, 0x30, 0xff});
    files[1].data = OneRegionCbnk({{0x05000000, 0}});
    files[2].data = OneRegionCbnk({{0x05000000, 1}});
    const std::vector<int16_t> kSamples(64, 1000);
    files[3].data = Cwar({Pcm16Cwav(64, kSamples, 1, true), Pcm16Cwav(64, kSamples, 2, true)});
    const std::vector<uint32_t> kMono = {0x03000000}, kStereo = {0x03000001};
    const std::vector<SequenceSound> kSounds = {{.banks = kMono, .pan_curve = 2},
                                                {.banks = kMono, .pan_mode = 1},
                                                {.banks = kStereo, .pan_mode = 1},
                                                {.banks = kMono}};
    const SoundArchive archive =
        SoundArchive::Load(Csar(files, {.sequences = kSounds, .banks = {1, 2}, .wave_archives = {3}}));
    BankSet mono(archive, archive.Sounds()[0]);
    BankSet stereo(archive, archive.Sounds()[2]);

    const Performance curve = Perform(archive, 0, mono, {});
    const Performance mono_balance = Perform(archive, 1, mono, {});
    const Performance stereo_balance = Perform(archive, 2, stereo, {});
    const Performance usual = Perform(archive, 3, mono, {});

    const std::string kPan = "custom pan curve or mode (using defaults)";
    CITRUSF2_CHECK(archive.Sounds()[0].pan_curve == 2 && archive.Sounds()[2].pan_mode == 1);
    CITRUSF2_CHECK(curve.approximations.contains(kPan) && stereo_balance.approximations.contains(kPan));
    CITRUSF2_CHECK(!mono_balance.approximations.contains(kPan) && !usual.approximations.contains(kPan));
}

void TestNamesHaveNoControlCharacters()
{
    // Replace unsafe name characters with '?': newline, terminal escape, DEL, C1 NEL, Unicode line/paragraph separators
    // and invalid UTF-8 (lone byte, overlong encoding, surrogate and truncated character). Preserve valid é and emoji.
    const std::vector<std::pair<std::string, std::string>> kPieces = {
        {"Seq", "Seq"},           {"\n", "?"},
        {"\x1b[2J", "?[2J"},      {"\x7f", "?"},
        {"\xc2\x85", "?"},        {"\xe2\x80\xa8", "?"},
        {"\xe2\x80\xa9", "?"},    {"\xff", "?"},
        {"\xc0\xaf", "??"},       {"\xed\xa0\x80", "???"},
        {"\xc3\xa9", "\xc3\xa9"}, {"\xf0\x9f\x98\x80", "\xf0\x9f\x98\x80"},
        {"\xe2\x80", "??"}};
    std::string name, printable;
    for (const auto& [piece, shown] : kPieces)
    {
        name += piece;
        printable += shown;
    }

    std::vector<ArchiveFile> files(1);
    files[0].data = Cseq({0xff});

    const SoundArchive archive =
        SoundArchive::Load(Csar(files, {.sequences = {{0, 0}}, .names = {{0x01000000, name}}}));

    CITRUSF2_CHECK(archive.Sounds().size() == 1 && archive.Sounds()[0].name == printable);
}

void TestPcm16WaveDecodes()
{
    const Pcm pcm = DecodeWave(Pcm16Cwav(4, {1, -2, 300, -32768}));

    CITRUSF2_CHECK(pcm.channels.size() == 1 && !pcm.loop && pcm.Length() == 4);
    CITRUSF2_CHECK(pcm.channels[0] == std::vector<int16_t>({1, -2, 300, -32768}));
}

void TestWaveLongerThanItsDataIsReported()
{
    // Header claims 2^32 - 1 samples; data contains four.
    const std::vector<uint8_t> cwav = Pcm16Cwav(0xffffffff, {1, 2, 3, 4});

    std::string message;
    try
    {
        DecodeWave(cwav);
    }
    catch (const FormatError& e)
    {
        message = e.what();
    }

    CITRUSF2_CHECK(message == "wave data shorter than the declared sample count");
}

void TestWaveDecodesItsFirstTwoChannels()
{
    // Ignore channels after the first two, as nw::snd does, even if the header claims thousands.
    const Pcm pcm = DecodeWave(Pcm16Cwav(4, {1, -2, 300, -32768}, 3));

    CITRUSF2_CHECK(pcm.channels.size() == 2 && pcm.channels[1] == std::vector<int16_t>({1, -2, 300, -32768}));
}

void TestBankOfSharedRegionsIsReported()
{
    // Shared region table entries can expand a small bank into many regions. A larger version of this input could
    // exhaust memory.
    const std::vector<uint8_t> cbnk = SharedRegionsCbnk(2);

    std::string message;
    try
    {
        Bank::Parse(cbnk);
    }
    catch (const FormatError& e)
    {
        message = e.what();
    }

    CITRUSF2_CHECK(message == "invalid region tables (region count exceeds bank size in bytes)");
}

void TestEnvelopeValuesMatchTheGame()
{
    CITRUSF2_CHECK(AttackMs(127) == 1.0);
    CITRUSF2_CHECK(AttackMs(0) > 13000.0 && AttackMs(0) < 13300.0); // 13.1 s: the slowest attack
    CITRUSF2_CHECK(DecayRate(127) == 65535.0 && DecayRate(126) == 24.0);
    CITRUSF2_CHECK(SustainCentibels(127) == 0 && SustainCentibels(0) == -723);
    CITRUSF2_CHECK(HoldMs(0) == 0.0 && HoldMs(1) == 1.0);
}

void TestInstantEnvelopeIsAnOrgan()
{
    // Attack, decay, sustain and release 127, hold 0: immediate full level, then immediate release.
    const Sf2Envelope e = ConvertEnvelope(EnvelopeValues{127, 127, 127, 0, 127});

    CITRUSF2_CHECK(e.attack == -12000 && e.hold == -12000 && e.sustain == 0 && e.release == -12000);
}

void TestDecayTakesTheGamesTime()
{
    // A decay of 100 dB takes 1000 / rate envelope milliseconds.
    const double decay_ms = 1000.0 / DecayRate(100) * kEnvTimeScale;

    const Sf2Envelope d = ConvertEnvelope(EnvelopeValues{127, 100, 64, 0, 100});

    CITRUSF2_CHECK(std::abs(d.decay - 1200.0 * std::log2(decay_ms / 1000.0)) <= 1.0);
    CITRUSF2_CHECK(d.sustain == -SustainCentibels(64));
}

void TestSlowDecayReachesSustainOnTime()
{
    // Decay 2 is slower than SoundFont's slowest decay, 8000 timecents for 100 dB. The hold makes up the difference, so
    // the decay reaches sustain 100 (-4.2 dB) when the game's does.
    const double game_ms = 1000.0 / DecayRate(2) * kEnvTimeScale;
    const double fall = -SustainCentibels(100) / 1000.0; // of 100 dB

    const Sf2Envelope d = ConvertEnvelope(EnvelopeValues{127, 2, 100, 0, 127});

    const double reached_ms = 1000.0 * (std::exp2(d.hold / 1200.0) + fall * std::exp2(d.decay / 1200.0));
    CITRUSF2_CHECK(d.decay == 8000);
    CITRUSF2_CHECK(std::abs(reached_ms - fall * game_ms) < 0.001 * reached_ms);
}

void TestSfzDecayTakesTheGamesTime()
{
    // sfizz falls by e^9 (78.17 dB) over its decay time. Use the game's time to fall 78.17 dB, based on its 1000 / rate
    // envelope milliseconds per 100 dB.
    const double decay_ms = 1000.0 / DecayRate(100) * kEnvTimeScale;

    const SfzEnvelope d = ConvertEnvelopeForSfz(EnvelopeValues{127, 100, 64, 0, 100});

    const double kSfizzFallDb = 9.0 * 20.0 / std::log(10.0);
    CITRUSF2_CHECK(std::abs(d.decay * 1000.0 - decay_ms * kSfizzFallDb / 100.0) < 1e-9 * decay_ms);
    CITRUSF2_CHECK(d.release == d.decay && d.attack == 0.0 && d.hold == 0.0);
    CITRUSF2_CHECK(std::abs(d.sustain - 100.0 * std::pow(10.0, SustainCentibels(64) / 200.0)) < 1e-9);

    // An attack of 127 is instant.
    const SfzEnvelope organ = ConvertEnvelopeForSfz(EnvelopeValues{127, 127, 127, 0, 127});
    CITRUSF2_CHECK(organ.attack == 0.0 && organ.hold == 0.0 && organ.sustain == 100.0 && organ.release < 0.001);
}

void TestEnvelopeSimulationSustainsAndReleases()
{
    // Reach sustain, then release below -90.4 dB. Before the first update, release starts at -90.4 dB even for instant
    // attack.
    CITRUSF2_CHECK(HeldLevel(EnvelopeValues{127, 127, 64, 0, 127}, 10) == SustainCentibels(64));
    CITRUSF2_CHECK(HeldLevel(EnvelopeValues{127, 127, 64, 0, 127}, 0) == -904.0f);
    CITRUSF2_CHECK(ReleaseFrames(0.0f, DecayRate(127)) == 1);
    CITRUSF2_CHECK(ReleaseFrames(-500.0f, 1.0) == 81); // 80 steps of 5 end at -900, and the 81st goes below -904
    CITRUSF2_CHECK(FramesToLevel(EnvelopeValues{127, 127, 127, 0, 127}, -400.0f) == -1);
    CITRUSF2_CHECK(FramesToLevel(EnvelopeValues{127, 100, 0, 0, 127}, -400.0f) > 0);

    // In single precision, release 106 falls exactly 3 x 0.1 dB per frame. From sustain 2 (-72.1 dB), 61 steps reach
    // -90.4 dB; only the 62nd falls below the stop threshold.
    const float kSustain2 = static_cast<float>(SustainCentibels(2));
    CITRUSF2_CHECK(ReleasedLevel(kSustain2, DecayRate(106), 61) == -904.0f);
    CITRUSF2_CHECK(ReleaseFrames(kSustain2, DecayRate(106)) == 62);
}

void TestNoteIsQuietOnlyAfterItsAttack()
{
    // A slow attack starts below -40 dB, but has not yet decayed to that level.
    const EnvelopeValues kSlowAttack{60, 100, 0, 0, 127};

    const int64_t frames = FramesToLevel(kSlowAttack, -400.0f);

    CITRUSF2_CHECK(frames * 5 > AttackMs(kSlowAttack.attack));
}

void TestPitchRatioIsTheGames()
{
    // Octaves are exact; fractional ratios use the game's single-precision 2^(i/12) and 2^(i/3072) tables.
    CITRUSF2_CHECK(PitchRatio(0) == 1.0f && PitchRatio(3072) == 2.0f && PitchRatio(-3072) == 0.5f);
    CITRUSF2_CHECK(PitchRatio(256) == 0x1.0f38fap+0f); // a semitone
    CITRUSF2_CHECK(PitchRatio(-1) == 0x1.ffe270p-1f);  // 1/256 semitone down, an ulp above 2^(-1/3072) rounded
    int wrong_octave = 0, inexact = 0;
    for (int p = -3 * 3072; p <= 3 * 3072; p++)
    {
        wrong_octave += PitchRatio(p + 3072) != 2.0f * PitchRatio(p);
        inexact += std::abs(PitchRatio(p) / std::exp2(p / 3072.0) - 1.0) > 2e-7;
    }
    CITRUSF2_CHECK(wrong_octave == 0 && inexact == 0);
}

void TestLfoWaitsThenTurns()
{
    // Depth 128, range 1 semitone, speed 64 (25 Hz), delay 10 ms. Remain silent during the delay, then advance 1/8
    // cycle per 5 ms through all four sine quadrants.
    Lfo lfo;
    lfo.Set(128, 64, 1, 10);
    std::vector<float> values = {lfo.Value()};
    for (int i = 0; i < 8; i++)
    {
        lfo.Update(5);
        values.push_back(lfo.Value());
    }

    const float kEighth = 90.0f / 127.0f; // the table's sin(pi / 4)
    const std::vector<float> kExpected = {0, 0, 0, kEighth, 1, kEighth, 0, -kEighth, -1};
    CITRUSF2_CHECK(values.size() == kExpected.size());
    for (std::size_t i = 0; i < values.size() && i < kExpected.size(); i++)
    {
        CITRUSF2_CHECK(std::abs(values[i] - kExpected[i]) < 1e-6f);
    }

    Lfo off;
    off.Set(0, 64, 1, 0);
    off.Update(10);
    CITRUSF2_CHECK(off.Value() == 0.0f);
}

void TestTrackFiltersFollowTheGame()
{
    // Low-pass arguments 0-10 select kLpfFreq[0] (80 Hz); 11-57 advance through the table: 32 -> 800 Hz, 40 -> 2000, 48
    // -> 5120, 54 -> 10240. Arguments >= 58, including 0x40, disable it.
    const std::pair<int, int> kLowPass[] = {{0, 0},   {10, 0},  {11, 1},  {32, 10},   {40, 14}, {48, 18},
                                            {54, 21}, {57, 22}, {58, -1}, {0x40, -1}, {255, -1}};
    for (const auto& [arg, index] : kLowPass)
    {
        CITRUSF2_CHECK(TrackFilters(arg, 0, 0).low_pass == index);
    }

    // A valid biquad type and positive value enable the filter. Value 64 selects low-pass step 55; values >= 127 select
    // the final step.
    CITRUSF2_CHECK(TrackFilters(0x40, 1, 64).biquad_type == 1 && TrackFilters(0x40, 1, 64).biquad_step == 55);
    CITRUSF2_CHECK(TrackFilters(0x40, 1, 255).biquad_step == 111 && TrackFilters(0x40, 2, 127).biquad_step == 96);
    CITRUSF2_CHECK(TrackFilters(0x40, 3, 127).biquad_step == 121 && TrackFilters(0x40, 5, 127).biquad_step == 92);
    CITRUSF2_CHECK(TrackFilters(0x40, 1, 0).biquad_type == 0 && TrackFilters(0x40, 0, 64).biquad_type == 0 &&
                   TrackFilters(0x40, 6, 64).biquad_type == 0);
}

void TestSoundFontFilterStandsInForTheGames()
{
    // No active filter leaves SoundFont open, as does an unsupported high-pass.
    CITRUSF2_CHECK(ToSoundFont(TrackFilters(0x40, 0, 0)) == SoundFontFilter{});
    CITRUSF2_CHECK(ToSoundFont(TrackFilters(0x40, 2, 64)) == SoundFontFilter{});

    // Biquad low-pass Q is 0.864 (17 cB for FluidSynth). With both low-passes enabled, choose the lower cutoff.
    const SoundFontFilter biquad = ToSoundFont(TrackFilters(0x40, 1, 64));
    const SoundFontFilter low_pass = ToSoundFont(TrackFilters(32, 0, 0));
    CITRUSF2_CHECK(biquad.cutoff < kFilterOpen && biquad.resonance == 17 && low_pass.resonance == 0);
    CITRUSF2_CHECK(ToSoundFont(TrackFilters(32, 1, 64)) == (low_pass.cutoff < biquad.cutoff ? low_pass : biquad));

    // Lower settings give lower cutoffs.
    int rising = 0;
    for (int arg = 1; arg < 58; arg++)
    {
        rising += ToSoundFont(TrackFilters(arg, 0, 0)).cutoff < ToSoundFont(TrackFilters(arg - 1, 0, 0)).cutoff;
    }
    for (int value = 1; value < 128; value++)
    {
        rising +=
            ToSoundFont(TrackFilters(0x40, 1, value)).cutoff > ToSoundFont(TrackFilters(0x40, 1, value - 1)).cutoff;
    }
    CITRUSF2_CHECK(rising == 0);

    // Each controller step lowers cutoff by 75 cents.
    CITRUSF2_CHECK(FilterControllerValue(kFilterOpen) == 0 && FilterControllerValue(kFilterOpen - 75) == 1);
    CITRUSF2_CHECK(FilterControllerValue(kFilterOpen - 75 * 111) == 111 && FilterControllerValue(0) == 127);
}

void TestSfzFilterFollowsTheGame()
{
    // SFZ controllers: 64 minus the low-pass argument (<= 6 means off), and the biquad value (0 without a preset).
    CITRUSF2_CHECK(SfzLowPassValue(0x40) == 0 && SfzLowPassValue(200) == 0 && SfzLowPassValue(58) == 6);
    CITRUSF2_CHECK(SfzLowPassValue(57) == 7 && SfzLowPassValue(0) == 64 && SfzLowPassValue(-100) == 127);
    CITRUSF2_CHECK(SfzBiquadValue(1, 64) == 64 && SfzBiquadValue(2, 300) == 127 && SfzBiquadValue(0, 64) == 0);
    CITRUSF2_CHECK(SfzBiquadValue(6, 64) == 0 && SfzBiquadValue(3, -5) == 0);

    // The cosine table shifts the nominal 80 Hz cutoff to a one-pole filter at 102 Hz, about 9131 cents below 20 kHz.
    // Lower settings must still give lower cutoffs.
    CITRUSF2_CHECK(SfzLowPassCutoff(0) == -9131);
    int falling = 0;
    for (int i = 1; i < 23; i++)
    {
        falling += SfzLowPassCutoff(i) <= SfzLowPassCutoff(i - 1);
    }
    CITRUSF2_CHECK(falling == 0);

    // Biquad low-pass value 64 (step 55) matches a cookbook filter at about 1.8 kHz, Q 0.864 (-1.27 dB), gain -0.5 dB.
    // Band-pass gain is 1.
    const SfzBiquad step55 = SfzBiquadStep(1, 55);
    CITRUSF2_CHECK(step55.cutoff > 900 && step55.cutoff < 1100 && step55.resonance == -127 && step55.gain == -50);
    CITRUSF2_CHECK(SfzBiquadStep(3, 60).gain == 0 && SfzBiquadStep(5, 92).gain == 0);
    CITRUSF2_CHECK(SfzBiquadStep(1, 500).cutoff == SfzBiquadStep(1, 111).cutoff);
}

void TestSoundFontPanGivesTheGamesGains()
{
    // Pan conversion makes the sin/cos law reproduce nw::snd's square-root law.
    for (int i = -19; i <= 19; i++)
    {
        const double p = i / 20.0;
        const double w = SoundFontPan(p);

        const double r_sf = std::sin((w + 1.0) * std::numbers::pi / 4.0);
        const double l_sf = std::cos((w + 1.0) * std::numbers::pi / 4.0);

        CITRUSF2_CHECK(std::abs(r_sf - std::sqrt((1.0 + p) / 2.0)) < 1e-9);
        CITRUSF2_CHECK(std::abs(l_sf - std::sqrt((1.0 - p) / 2.0)) < 1e-9);
    }

    CITRUSF2_CHECK(SoundFontPan(0.0) == 0.0 && SoundFontPan(1.0) == 1.0 && SoundFontPan(-1.0) == -1.0);
}

void TestMissingBlockIsReported()
{
    // CWAV with INFO (0x7000) but no DATA block.
    std::vector<uint8_t> cwav(0x20, 0);
    std::memcpy(cwav.data(), "CWAV", 4);
    cwav[4] = 0xff;
    cwav[5] = 0xfe;
    cwav[0x10] = 1;
    cwav[0x15] = 0x70;

    std::string message;
    try
    {
        DecodeWave(cwav);
    }
    catch (const FormatError& e)
    {
        message = e.what();
    }

    CITRUSF2_CHECK(message == "bad CWAV: no DATA block");
}

void TestShortFileIsReported()
{
    std::string message;
    try
    {
        DecodeWave(std::vector<uint8_t>{'C', 'W', 'A', 'V'});
    }
    catch (const FormatError& e)
    {
        message = e.what();
    }

    CITRUSF2_CHECK(message == "bad CWAV: too short to be a CWAV file");
}

// Play the first `n` samples of `data`, looping over [ls, le).
std::vector<int16_t> Play(const std::vector<int16_t>& data, uint32_t ls, uint32_t le, std::size_t n)
{
    std::vector<int16_t> out;
    for (std::size_t i = 0; out.size() < n; i++)
    {
        out.push_back(data[i < le ? i : ls + (i - le) % (le - ls)]);
    }

    return out;
}

void TestLoopUnrollingKeepsTheStream()
{
    // Unroll a four-sample loop starting at 2: move its start to at least 8, extend it to at least 32 samples, and
    // append loop-start data. Verify identical playback.
    Pcm pcm;
    pcm.loop = true;
    pcm.loop_start = 2;
    pcm.loop_end = 6;
    pcm.channels = {{10, 11, 20, 21, 22, 23}};

    const PreparedWave w = PrepareForSoundFont(pcm);

    const auto& s = w.channels[0];
    CITRUSF2_CHECK(w.loop && w.loop_start >= 8 && w.loop_end - w.loop_start >= 32);
    CITRUSF2_CHECK((w.loop_end - w.loop_start) % 4 == 0);
    CITRUSF2_CHECK(s.size() >= w.loop_end + 8);
    CITRUSF2_CHECK(Play(s, w.loop_start, w.loop_end, 200) == Play(pcm.channels[0], 2, 6, 200));
    for (uint32_t i = 0; i < 8; i++)
    {
        CITRUSF2_CHECK(s[w.loop_end + i] == s[w.loop_start + i]);
    }
}

void TestSfzLoopUnrollingKeepsTheStream()
{
    // At 32728 Hz, 4 ms needs 131 samples. A four-sample loop starting at 2 gets 33 copies (132 samples) before it, 16
    // copies within it (64 samples) and 64 trailing samples. Verify identical playback and identical data before both
    // loop boundaries, making sfizz's crossfade inaudible.
    Pcm pcm;
    pcm.sample_rate = 32728;
    pcm.loop = true;
    pcm.loop_start = 2;
    pcm.loop_end = 6;
    pcm.channels = {{10, 11, 20, 21, 22, 23}};

    const PreparedWave w = PrepareForSfz(pcm);

    const auto& s = w.channels[0];
    CITRUSF2_CHECK(w.loop && w.loop_start == 134 && w.loop_end == 198 && s.size() == 262);
    CITRUSF2_CHECK(Play(s, w.loop_start, w.loop_end, 400) == Play(pcm.channels[0], 2, 6, 400));
    int different = 0;
    for (uint32_t i = 1; i <= 131; i++)
    {
        different += s[w.loop_start - i] != s[w.loop_end - i];
    }
    CITRUSF2_CHECK(different == 0);
}

void TestOneShotWaveStaysAsItIs()
{
    Pcm once;
    once.loop_end = 3;
    once.channels = {{1, 2, 3}, {4, 5, 6}};

    const PreparedWave o = PrepareForSoundFont(once);

    CITRUSF2_CHECK(!o.loop && o.channels.size() == 2 && o.channels[1] == std::vector<int16_t>({4, 5, 6}));
}

void TestSoundFontWriterOutputIsValid()
{
    sf2::SoundFont sf;
    sf.name = "test";
    for (int c = 0; c < 3; c++)
    {
        sf2::Sample s;
        s.name = "s" + std::to_string(c);
        s.data.assign(100, static_cast<int16_t>(c * 1000));
        s.loop_start = 10;
        s.loop_end = 90;
        sf.samples.push_back(s);
    }

    sf.samples[1].type = sf2::kLeft;
    sf.samples[1].link = 2;
    sf.samples[2].type = sf2::kRight;
    sf.samples[2].link = 1;

    sf2::Instrument inst;
    inst.name = "i";
    sf2::Zone global;
    global.modulators.push_back({0x0081, sf2::kVibLfoToPitch, 100, 0});
    inst.zones.push_back(global);
    for (uint16_t id = 0; id < 3; id++)
    {
        sf2::Zone z;
        z.generators.push_back(sf2::Generator::Value(sf2::kSampleId, id)); // written last whatever the order
        z.generators.push_back(sf2::Generator::Value(sf2::kSampleModes, 1));
        z.generators.push_back(sf2::Generator::Range(sf2::kVelRange, 0, 127));
        z.generators.push_back(sf2::Generator::Range(sf2::kKeyRange, 0, 127));
        inst.zones.push_back(z);
    }
    sf.instruments.push_back(inst);

    sf2::Preset p;
    p.name = "p";
    sf2::Zone pz;
    pz.generators.push_back(sf2::Generator::Value(sf2::kInstrument, 0));
    p.zones.push_back(pz);
    sf.presets.push_back(p);

    const auto bytes = sf2::Write(sf);

    const std::string problem = Sf2Problem(bytes);
    CITRUSF2_CHECK(problem.empty());
    if (!problem.empty())
    {
        std::fprintf(stderr, "  SoundFont writer: %s\n", problem.c_str());
    }

    const Sf2View v = ParseSf2(bytes);
    CITRUSF2_CHECK(v.chunks.at("phdr").second == 38 * 2 && v.chunks.at("inst").second == 22 * 2);
    CITRUSF2_CHECK(v.chunks.at("smpl").second == 2 * 3 * (100 + 46));
    CITRUSF2_CHECK(v.chunks.at("imod").second == 10 * 2);
}

void TestSoundFontWithoutSamplesHasSampleData()
{
    // Even a silent sequence needs sample padding: FluidSynth rejects empty sample data.
    const auto bytes = sf2::Write(sf2::SoundFont{});

    const Sf2View v = ParseSf2(bytes);

    CITRUSF2_CHECK(Sf2Problem(bytes).empty() && v.chunks.at("smpl").second == 2 * 46);
}

void TestMidiWriterLayersMarkersAndBendRange()
{
    Performance p;
    p.timebase = 48;
    p.tempo = {{0, 120.0}};
    p.loop = Performance::Loop{48, 96};
    p.end_tick = 96;
    p.presets.push_back({});

    auto add = [&](int t, uint32_t tick, EventKind k, uint8_t key, uint8_t value, uint32_t note)
    {
        TrackEvent e;
        e.tick = tick;
        e.kind = k;
        e.key = key;
        e.value = value;
        e.note = note;
        p.tracks[t].push_back(e);
    };

    add(0, 0, EventKind::kPreset, 0, 0, 0);
    add(0, 0, EventKind::kVolume, 0, 100, 0);
    add(0, 0, EventKind::kNoteOn, 60, 100, 1);
    add(0, 10, EventKind::kNoteOn, 60, 90, 2); // the same key again: another channel
    add(0, 20, EventKind::kNoteOff, 60, 0, 1);
    add(0, 30, EventKind::kNoteOff, 60, 0, 2);
    add(3, 5, EventKind::kNoteOn, 64, 80, 3);
    add(3, 50, EventKind::kNoteOff, 64, 0, 3);
    TrackEvent bend;
    bend.tick = 5;
    bend.kind = EventKind::kPitch;
    bend.semitones = -7.5f;
    p.tracks[3].insert(p.tracks[3].begin(), bend);

    MidiReport rep;
    const auto bytes = WriteMidi(p, "test", rep);

    // Timebase 48 gives 160 MIDI ticks for each of the performance's ticks.
    const MidiView v = ParseMidi(bytes);
    CITRUSF2_CHECK(v.ok);
    CITRUSF2_CHECK(v.division == 7680);
    CITRUSF2_CHECK(rep.channels == 3 && rep.cut_notes == 0);
    CITRUSF2_CHECK(v.track_names.size() == 4 && v.track_names[0] == "test" && v.track_names[2] == "Track 0 (layer 2)");
    CITRUSF2_CHECK(v.notes.size() == 3);

    // Track 0 gets channel 0, and channel 2 for its layer (track 3 has channel 1).
    int channels_for_key60 = 0;
    for (const MidiNote& n : v.notes)
    {
        if (n.key == 60)
        {
            channels_for_key60 |= 1 << n.channel;
        }
    }
    CITRUSF2_CHECK(channels_for_key60 == 0b101);

    CITRUSF2_CHECK(v.markers.size() == 2 && v.markers[0].first == 7680 && v.markers[1].second == "loopEnd");

    // Pitch bend range 8 (RPN 0) for a 7.5 semitone bend on track 3's channel (1).
    const std::vector<uint8_t> kRpn = {0xb1, 6, 8};
    CITRUSF2_CHECK(std::search(bytes.begin(), bytes.end(), kRpn.begin(), kRpn.end()) != bytes.end());
}

void TestMidiWriterHasAtLeast7680TicksPerQuarterNote()
{
    // Use at least 7680 MIDI ticks per quarter note, with an integer ratio to performance ticks. Without frame
    // timestamps, preserve note positions.
    const std::pair<uint32_t, uint16_t> kCases[] = {{48, 7680}, {96, 7680}, {100, 7700}, {255, 7905}, {1, 7680}};
    for (const auto& [timebase, division] : kCases)
    {
        Performance p;
        p.timebase = timebase;
        p.tempo = {{0, 120.0}};
        p.end_tick = 8;
        p.presets.push_back({});
        TrackEvent on;
        on.tick = 3;
        on.key = 60;
        on.value = 100;
        on.note = 1;
        TrackEvent off = on;
        off.tick = 7;
        off.kind = EventKind::kNoteOff;
        p.tracks[0] = {on, off};

        MidiReport rep;
        const MidiView v = ParseMidi(WriteMidi(p, "test", rep));
        const uint32_t scale = division / timebase;
        CITRUSF2_CHECK(v.ok && v.division == division);
        CITRUSF2_CHECK(v.notes.size() == 1 && v.notes[0].on == 3 * scale && v.notes[0].off == 7 * scale);
    }
}

void TestMidiEventsGoOnSoundFrames()
{
    // At timebase 48, use 160 MIDI ticks per performance tick. Align events to fractional frame starts without crossing
    // the loop start at tick 2. Keys 60, 62 and 64 span ticks 1-2, 2-3 and 3-4.
    Performance p;
    p.tempo = {{0, 120.0}};
    p.end_tick = 8;
    p.presets.push_back({});
    p.frame_ticks = {0.0, 0.5, 1.75, 2.75, 4.0};
    p.loop = Performance::Loop{2, 6};
    const std::pair<uint8_t, uint32_t> kNotes[] = {{60, 1}, {62, 2}, {64, 3}}; // key, tick
    uint32_t id = 1;
    for (const auto& [key, tick] : kNotes)
    {
        TrackEvent on;
        on.tick = tick;
        on.key = key;
        on.value = 100;
        on.note = id++;
        TrackEvent off = on;
        off.tick = tick + 1;
        off.kind = EventKind::kNoteOff;
        p.tracks[0].push_back(on);
        p.tracks[0].push_back(off);
    }

    MidiReport rep;
    const MidiView m = ParseMidi(WriteMidi(p, "test", rep));

    CITRUSF2_CHECK(m.ok && m.notes.size() == 3);
    if (m.notes.size() == 3)
    {
        CITRUSF2_CHECK(m.notes[0].key == 60 && m.notes[0].on == 80 && m.notes[0].off == 320);
        CITRUSF2_CHECK(m.notes[1].key == 62 && m.notes[1].on == 320 && m.notes[1].off == 440);
        CITRUSF2_CHECK(m.notes[2].key == 64 && m.notes[2].on == 440 && m.notes[2].off == 640);
    }
    CITRUSF2_CHECK(m.markers == (std::vector<std::pair<uint32_t, std::string>>{{320, "loopStart"}, {960, "loopEnd"}}));
}

void TestMidiWriterSplitsLongDeltaTimes()
{
    // At timebase 1, each performance tick is 7680 MIDI ticks. A 600000-tick note exceeds both 32-bit MIDI positions
    // and the 28-bit delta limit.
    Performance p;
    p.timebase = 1;
    p.tempo = {{0, 120.0}};
    p.end_tick = 600000;
    p.presets.push_back({});
    TrackEvent on;
    on.key = 60;
    on.value = 100;
    on.note = 1;
    TrackEvent off = on;
    off.tick = 600000;
    off.kind = EventKind::kNoteOff;
    p.tracks[0] = {on, off};

    MidiReport rep;
    const MidiView v = ParseMidi(WriteMidi(p, "test", rep));

    CITRUSF2_CHECK(v.ok && v.track_ends == std::vector<uint64_t>(2, uint64_t{600000} * 7680));
}

void TestMidiBendUpByAWholeRangeIsExact()
{
    // A +7-semitone bend needs range 8 because the pitch wheel stops at +8191, one step short of full range. With range
    // 8, +7 is 7168 steps: wheel value 15360 (0x3c00, bytes 0 and 0x78).
    Performance p;
    p.tempo = {{0, 120.0}};
    p.end_tick = 48;
    p.presets.push_back({});
    TrackEvent bend;
    bend.kind = EventKind::kPitch;
    bend.semitones = 7.0f;
    p.tracks[0].push_back(bend);
    TrackEvent on;
    on.key = 60;
    on.value = 100;
    on.note = 1;
    p.tracks[0].push_back(on);
    TrackEvent off = on;
    off.tick = 24;
    off.kind = EventKind::kNoteOff;
    p.tracks[0].push_back(off);

    MidiReport rep;
    const auto bytes = WriteMidi(p, "test", rep);

    const std::vector<uint8_t> kRange = {0xb0, 6, 8};
    const std::vector<uint8_t> kBend = {0xe0, 0x00, 0x78};
    CITRUSF2_CHECK(std::search(bytes.begin(), bytes.end(), kRange.begin(), kRange.end()) != bytes.end());
    CITRUSF2_CHECK(std::search(bytes.begin(), bytes.end(), kBend.begin(), kBend.end()) != bytes.end());
}

void TestMidiWriterSendsTheFilterToEveryLayer()
{
    // Overlapping strikes of one key need a second channel. Send filter controllers to both.
    Performance p;
    p.tempo = {{0, 120.0}};
    p.end_tick = 48;
    p.presets.push_back({});
    TrackEvent filter;
    filter.kind = EventKind::kFilter;
    filter.value = 40;
    TrackEvent on;
    on.key = 60;
    on.value = 100;
    on.note = 1;
    TrackEvent again = on;
    again.tick = 12;
    again.note = 2;
    TrackEvent off = on;
    off.tick = 24;
    off.kind = EventKind::kNoteOff;
    TrackEvent off_again = again;
    off_again.tick = 36;
    off_again.kind = EventKind::kNoteOff;
    p.tracks[0] = {filter, on, again, off, off_again};

    MidiReport rep;
    const auto bytes = WriteMidi(p, "test", rep);

    CITRUSF2_CHECK(rep.channels == 2);
    for (const int channel : {0, 1})
    {
        const std::vector<uint8_t> kFilter = {static_cast<uint8_t>(0xb0 | channel), kFilterController, 40};
        CITRUSF2_CHECK(std::search(bytes.begin(), bytes.end(), kFilter.begin(), kFilter.end()) != bytes.end());
    }
}

void TestMidiWriterListsChannel10Presets()
{
    // Track t uses preset t. With 16 tracks, the last gets channel 10 (index 9), so copy its preset to bank 128. With
    // 15 tracks, leave channel 10 unused.
    for (int tracks : {15, 16})
    {
        Performance p;
        p.tempo = {{0, 120.0}};
        p.end_tick = 48;
        for (int t = 0; t < tracks; t++)
        {
            TrackEvent preset;
            preset.kind = EventKind::kPreset;
            preset.preset = static_cast<uint32_t>(t);
            TrackEvent on;
            on.key = 60;
            on.value = 100;
            on.note = static_cast<uint32_t>(t + 1);
            TrackEvent off = on;
            off.tick = 24;
            off.kind = EventKind::kNoteOff;
            p.presets.push_back({});
            p.tracks[t] = {preset, on, off};
        }

        MidiReport rep;
        WriteMidi(p, "test", rep);

        CITRUSF2_CHECK(rep.channels == tracks);
        CITRUSF2_CHECK(rep.channel_10_presets == (tracks == 16 ? std::set<uint32_t>{15} : std::set<uint32_t>{}));
    }
}

void TestWavFileIsMonoPcm16()
{
    const std::vector<uint8_t> wav = WavFile(std::vector<int16_t>{1, -2, 32767}, 32728);

    CITRUSF2_CHECK(wav.size() == 50 && Text(std::span(wav).first(4)) == "RIFF" && Le32(wav, 4) == 42);
    CITRUSF2_CHECK(Text(std::span(wav).subspan(8, 8)) == "WAVEfmt " && Le32(wav, 16) == 16);
    // PCM, one channel, 32728 Hz, 2 bytes a frame, 16 bits.
    CITRUSF2_CHECK(Le16(wav, 20) == 1 && Le16(wav, 22) == 1 && Le32(wav, 24) == 32728 && Le32(wav, 28) == 65456);
    CITRUSF2_CHECK(Le16(wav, 32) == 2 && Le16(wav, 34) == 16);
    CITRUSF2_CHECK(Text(std::span(wav).subspan(36, 4)) == "data" && Le32(wav, 40) == 6);
    CITRUSF2_CHECK(Le16(wav, 44) == 1 && Le16(wav, 46) == 0xfffe && Le16(wav, 48) == 32767);
}

void TestSfzFileHasTheTracksPresetsAndFilters()
{
    // Track 2 uses presets 0 and 1 with nominal 80 Hz low-pass (0xd8 argument 4) and biquad high-pass (type 2). Other
    // tracks are silent and get no MIDI channel or SFZ file. With no archive banks, the presets have no regions.
    const SoundArchive archive = SoundArchive::Load(Csar({}));
    BankSet banks(archive, SoundInfo{});
    Performance p;
    p.presets.resize(2);
    TrackEvent low_pass;
    low_pass.kind = EventKind::kSfzLowPass;
    low_pass.value = SfzLowPassValue(4);
    TrackEvent biquad;
    biquad.kind = EventKind::kSfzBiquad;
    biquad.value = SfzBiquadValue(2, 64);
    TrackEvent first;
    first.kind = EventKind::kPreset;
    TrackEvent second = first;
    second.preset = 1;
    p.tracks[2] = {low_pass, biquad, first, second};
    p.biquad_types[2] = 2;
    p.level_cut_db = 2.5;
    MidiReport midi;
    midi.bend_ranges[2] = 2;

    const SfzInstruments sfz = BuildSfz(p, midi, banks, "TITLE", {"first", "second"});

    CITRUSF2_CHECK(sfz.files.size() == 1 && sfz.samples.empty());
    if (sfz.files.size() != 1)
    {
        return;
    }

    const SfzFile& f = sfz.files[0];
    auto has = [&](const std::string& s)
    {
        return f.text.find(s) != std::string::npos;
    };
    CITRUSF2_CHECK(f.name == "Track 2.sfz");
    CITRUSF2_CHECK(has("MIDI gain was reduced by 2.5 dB. Region volumes restore that gain.\n"));
    CITRUSF2_CHECK(has("<control>\ndefault_path=../samples/\n"));
    CITRUSF2_CHECK(has("bend_up=199.975586 bend_down=-199.975586\n"));

    // Low-pass uses filter 1. Controller value 60 selects nominal 80 Hz (9131 cents below 20 kHz); 0 leaves it open.
    CITRUSF2_CHECK(low_pass.value == 60);
    CITRUSF2_CHECK(has("fil_type=lpf_1p cutoff=20000 cutoff_oncc103=12000 cutoff_curvecc103=8\n"));
    CITRUSF2_CHECK(has("<curve> curve_index=8\nv000=0 ") && has(" v060=-0.760917 "));

    // Each preset gets a master. Notes starting with the biquad controller above 0 use high-pass in filter slot 2.
    CITRUSF2_CHECK(has("// first\n<master> loprog=0 hiprog=0\n") && has("// second\n<master> loprog=1 hiprog=1\n"));
    CITRUSF2_CHECK(has("<group> hicc104=0\n<group> locc104=1 fil2_type=hpf_2p cutoff2=1000 "));
}

void TestSfzCommentKeepsTheNameOnItsLine()
{
    // A newline in a sequence name could end its comment and inject SFZ opcodes. Test without banks, so any region
    // would have come from the name.
    const SoundArchive archive = SoundArchive::Load(Csar({}));
    BankSet banks(archive, SoundInfo{});
    Performance p;
    p.presets.resize(1);
    TrackEvent preset;
    preset.kind = EventKind::kPreset;
    p.tracks[0] = {preset};
    MidiReport midi;
    midi.bend_ranges[0] = 2;

    const SfzInstruments sfz = BuildSfz(p, midi, banks, "A\r\n<region> sample=x.wav", {"b0 p0"});

    CITRUSF2_CHECK(sfz.files.size() == 1);
    for (const SfzFile& f : sfz.files)
    {
        CITRUSF2_CHECK(f.text.starts_with("// Track 0 of A  <region> sample=x.wav, converted by citrusf2"));
        CITRUSF2_CHECK(f.text.find("\n<region>") == std::string::npos);
    }
}

void TestFileNameKeepsValidNames()
{
    CITRUSF2_CHECK(FileName(".x. y") == ".x. y");
}

void TestFileNameReplacesCharactersWindowsForbids()
{
    // Path separators included.
    CITRUSF2_CHECK(FileName("a<b>c:d\"e/f\\g|h?i*j") == "a_b_c_d_e_f_g_h_i_j");
    CITRUSF2_CHECK(FileName("../x") == ".._x");
    CITRUSF2_CHECK(FileName(std::string("a\tb\0c", 5)) == "a_b_c");

    // Reject trailing dots/spaces in directory names, including "." and "..".
    CITRUSF2_CHECK(FileName("BGM.") == "BGM_" && FileName("BGM ") == "BGM_" && FileName("CON.") == "CON_");
    CITRUSF2_CHECK(FileName(".") == "_" && FileName("..") == "._");
}

void TestFileNameAvoidsDeviceNames()
{
    // Insert '_' before the first dot.
    CITRUSF2_CHECK(FileName("NUL") == "NUL_");
    CITRUSF2_CHECK(FileName("con.x") == "con_.x");
    CITRUSF2_CHECK(FileName("Com1") == "Com1_");
    CITRUSF2_CHECK(FileName("nul .x") == "nul _.x");
    CITRUSF2_CHECK(FileName("LPT\u00b9") == "LPT\u00b9_");
    CITRUSF2_CHECK(FileName("CONOUT$") == "CONOUT$_");
    CITRUSF2_CHECK(FileName("CONSOLE") == "CONSOLE");
}

// Check that SFZ files match the tracks in `midi`, reference the sequence's WAV samples and keep loops within sample
// bounds. Return an error, or an empty string on success.
std::string SfzProblem(const SfzInstruments& sfz, const MidiView& midi)
{
    std::map<std::string, uint32_t> frames;
    for (const SfzSample& s : sfz.samples)
    {
        const std::vector<uint8_t>& w = s.wav;
        if (w.size() < 44 || Text(std::span(w).first(4)) != "RIFF" || Le32(w, 4) != w.size() - 8 ||
            Le32(w, 40) != w.size() - 44)
        {
            return "broken WAV file " + s.name;
        }
        frames[s.name] = Le32(w, 40) / 2;
    }

    std::set<std::string> names, tracks;
    for (const SfzFile& f : sfz.files)
    {
        names.insert(f.name);
    }
    for (std::size_t t = 1; t < midi.track_names.size(); t++) // skip the conductor track
    {
        const std::string& track = midi.track_names[t];
        tracks.insert(track.substr(0, track.find(" (layer")) + ".sfz");
    }
    if (names != tracks)
    {
        return std::to_string(names.size()) + " files for " + std::to_string(tracks.size()) + " tracks";
    }

    for (const SfzFile& f : sfz.files)
    {
        std::istringstream lines(f.text);
        for (std::string line; std::getline(lines, line);)
        {
            if (!line.starts_with("<region> "))
            {
                continue;
            }

            std::map<std::string, std::string> opcodes;
            std::istringstream words(line.substr(9));
            for (std::string word; words >> word;)
            {
                opcodes[word.substr(0, word.find('='))] = word.substr(word.find('=') + 1);
            }

            const auto it = frames.find(opcodes["sample"]);
            if (it == frames.end())
            {
                return f.name + " plays a sample the sequence doesn't have: " + opcodes["sample"];
            }
            if (opcodes.count("loop_end"))
            {
                const unsigned long start = std::stoul(opcodes["loop_start"]), end = std::stoul(opcodes["loop_end"]);
                if (start >= end || end >= it->second)
                {
                    return f.name + " loops outside " + it->first;
                }
            }
        }
    }

    return "";
}

void TestArchiveConvertsEverySequence(const char* path)
{
    std::ifstream f(path, std::ios::binary);
    std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(f)), {});
    if (bytes.empty())
    {
        std::fprintf(stderr, "can't read %s\n", path);
        failures++;
        return;
    }

    const SoundArchive archive = SoundArchive::Load(std::move(bytes));
    int sequences = 0, looping = 0;
    for (uint32_t i = 0; i < archive.Sounds().size(); i++)
    {
        const SoundInfo& s = archive.Sounds()[i];
        if (s.type != SoundType::kSequence || archive.FileMissing(s.file_id))
        {
            continue; // a sequence missing from a truncated archive can't be converted, and citrusf2 says so
        }

        sequences++;
        Conversion c;
        try
        {
            c = ConvertSequence(archive, i);
        }
        catch (const std::exception& e)
        {
            std::fprintf(stderr, "%s: %s\n", s.name.c_str(), e.what());
            failures++;
            continue;
        }

        const MidiView m = ParseMidi(c.midi);
        const std::string sf2_problem = Sf2Problem(c.sf2);
        const std::string sfz_problem = SfzProblem(c.sfz, m);
        if (!m.ok || !sf2_problem.empty() || !sfz_problem.empty())
        {
            std::fprintf(stderr, "%s: midi %s, sf2 %s, sfz %s\n", s.name.c_str(), m.ok ? "ok" : "broken",
                         sf2_problem.empty() ? "ok" : sf2_problem.c_str(),
                         sfz_problem.empty() ? "ok" : sfz_problem.c_str());
            failures++;
        }

        // Frame-aligned event positions must be no later than their tick and no earlier than the preceding tick's
        // frame.
        const std::vector<double>& frames = c.performance.frame_ticks;
        for (std::size_t k = 0; k < frames.size(); k++)
        {
            if (frames[k] > static_cast<double>(k) || (k > 0 && frames[k] < frames[k - 1]))
            {
                std::fprintf(stderr, "%s: tick %zu's sound frame starts at tick %g\n", s.name.c_str(), k, frames[k]);
                failures++;
                break;
            }
        }

        if (c.performance.loop)
        {
            looping++;
        }
    }

    std::printf("archive: %d sequences, %d loop\n", sequences, looping);
    CITRUSF2_CHECK(sequences > 0);
}

} // namespace

int main(int argc, char** argv)
{
    TestDspAdpcmScalesNibbles();
    TestDspAdpcmPredictsFromHistory();
    TestDspAdpcmClampsLargePredictions();
    TestPcm16WaveDecodes();
    TestWaveLongerThanItsDataIsReported();
    TestWaveDecodesItsFirstTwoChannels();
    TestBankOfSharedRegionsIsReported();
    TestEnvelopeValuesMatchTheGame();
    TestInstantEnvelopeIsAnOrgan();
    TestDecayTakesTheGamesTime();
    TestSlowDecayReachesSustainOnTime();
    TestSfzDecayTakesTheGamesTime();
    TestEnvelopeSimulationSustainsAndReleases();
    TestNoteIsQuietOnlyAfterItsAttack();
    TestPitchRatioIsTheGames();
    TestTrackFiltersFollowTheGame();
    TestSoundFontFilterStandsInForTheGames();
    TestLfoWaitsThenTurns();
    TestSfzFilterFollowsTheGame();
    TestSoundFontPanGivesTheGamesGains();
    TestMissingBlockIsReported();
    TestShortFileIsReported();
    TestFilesKeptInGroupsAreFound();
    TestBrokenGroupIsReported();
    TestGroupPastTheEndLeavesItsFilesMissing();
    TestFilePastTheEndOfItsGroupIsReported();
    TestFileCutShortInOneGroupIsTakenFromAnother();
    TestFilePastTheEndOfAWholeArchiveIsntTruncation();
    TestTruncatedArchiveWhoseTablesCantBeReadIsReported();
    TestUnreadableGroupTableIsReported();
    TestBrokenBankAndWaveReferencesAreReported();
    TestSequenceStartingPastItsEndPlaysNothing();
    TestBankSlotPastTheFourthHasNoBank();
    TestOnlyAJumpBackToPlayedCommandsIsALoop();
    TestMainLoopWaitsForAFadeTooLoudForMidi();
    TestMainLoopWaitsForALegatosFade();
    TestLegatoLeavesOutTheRegionsVolume();
    TestLevelCutFitsTheRunThatsKept();
    TestTiedNoteTakesPartInTheMainLoop();
    TestSilentLegatoCarriesItsNoteOn();
    TestLegatosChangeOfKeyIsNoChangeOfTheTracks();
    TestMonoLegatoTakesItsLengthWithTieOn();
    TestNoteReleasedBeforeItsFirstFrameIsSilent();
    TestWhatMidiCantSeparateIsReported();
    TestOneShotWaveEndsAsTheDspPlaysIt();
    TestTicksFallInTheGamesSoundFrames();
    TestOddArgumentsAreReadAsTheGameReadsThem();
    TestSequenceThatStopsAdvancingPlaysOut();
    TestTieLoopThatHoldsEndsAsAHeldSound();
    TestTieLoopOverAOneShotWaveStaysALoop();
    TestClosingTickReachesTheReleaseTail();
    TestSilenceAtTheTimeLimitEndsWithTheLastSound();
    TestOneShotWavePlaysOutAtItsPitch();
    TestTimedSweepGlidesOnWhenTheSequenceStops();
    TestTrackWaitingForAVariableGoesOn();
    TestWaitingForAVariableIsNoLoop();
    TestTrackStartsNoMoreNotesATickThanTheGameHasVoices();
    TestTrackPlayingANoteEachTimeAVariableIsSetGoesOn();
    TestTrackPlayingNotesWithoutWaitingIsStopped();
    TestSoundsOwnPanCurveIsReported();
    TestNamesHaveNoControlCharacters();
    TestLoopUnrollingKeepsTheStream();
    TestSfzLoopUnrollingKeepsTheStream();
    TestOneShotWaveStaysAsItIs();
    TestSoundFontWriterOutputIsValid();
    TestSoundFontWithoutSamplesHasSampleData();
    TestMidiWriterLayersMarkersAndBendRange();
    TestMidiWriterHasAtLeast7680TicksPerQuarterNote();
    TestMidiWriterSplitsLongDeltaTimes();
    TestMidiBendUpByAWholeRangeIsExact();
    TestMidiEventsGoOnSoundFrames();
    TestMidiWriterSendsTheFilterToEveryLayer();
    TestMidiWriterListsChannel10Presets();
    TestWavFileIsMonoPcm16();
    TestSfzFileHasTheTracksPresetsAndFilters();
    TestSfzCommentKeepsTheNameOnItsLine();
    TestFileNameKeepsValidNames();
    TestFileNameReplacesCharactersWindowsForbids();
    TestFileNameAvoidsDeviceNames();

    if (argc > 1)
    {
        TestArchiveConvertsEverySequence(argv[1]);
    }

    if (failures)
    {
        std::fprintf(stderr, "%d check(s) failed\n", failures);
        return 1;
    }

    std::puts("all checks passed");

    return 0;
}
