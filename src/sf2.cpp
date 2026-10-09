// SPDX-License-Identifier: MIT

// SoundFont 2 writer (see sf2.h).

#include "sf2.h"

#include <algorithm>
#include <cstddef>
#include <stdexcept>
#include <utility>

namespace citrusf2::sf2
{

namespace
{

// Zero padding required after each SoundFont sample.
constexpr uint32_t kSamplePadding = 46;

// RIFF chunk buffer with little-endian numeric writes.
struct Buffer
{
    void U8(uint8_t v)
    {
        data.push_back(v);
    }

    void U16(uint16_t v)
    {
        U8(static_cast<uint8_t>(v));
        U8(static_cast<uint8_t>(v >> 8));
    }

    void U32(uint32_t v)
    {
        U16(static_cast<uint16_t>(v));
        U16(static_cast<uint16_t>(v >> 16));
    }

    void Fourcc(const char* id)
    {
        data.insert(data.end(), id, id + 4);
    }

    // pdta names occupy 20 bytes: zero-padded, with a terminating zero.
    void Name(const std::string& s)
    {
        constexpr std::size_t kSize = 20;
        for (std::size_t i = 0; i < kSize; i++)
        {
            U8(i + 1 < kSize && i < s.size() ? static_cast<uint8_t>(s[i]) : 0);
        }
    }

    void Bytes(const std::vector<uint8_t>& b)
    {
        data.insert(data.end(), b.begin(), b.end());
    }

    // Write the chunk's ID, size and payload. An odd payload size gets a zero pad byte.
    void Chunk(const char* id, const std::vector<uint8_t>& payload)
    {
        Fourcc(id);
        U32(static_cast<uint32_t>(payload.size()));
        Bytes(payload);
        if (payload.size() & 1)
        {
            U8(0);
        }
    }

    std::vector<uint8_t> data;
};

// A zero-terminated string, padded to an even length.
std::vector<uint8_t> ZString(const std::string& s)
{
    std::vector<uint8_t> v(s.begin(), s.end());
    v.push_back(0);
    if (v.size() & 1)
    {
        v.push_back(0);
    }

    return v;
}

// Generator order: key range, velocity range, other generators, then instrument (preset zones) or sample (instrument
// zones).
int Rank(uint16_t oper, bool preset)
{
    if (oper == kKeyRange)
    {
        return 0;
    }
    if (oper == kVelRange)
    {
        return 1;
    }
    if (oper == (preset ? kInstrument : kSampleId))
    {
        return 3;
    }

    return 2;
}

// Writes the bag, modulator and generator lists of a set of zones.
struct ZoneLists
{
    void Add(const Zone& zone, bool preset)
    {
        bags.U16(static_cast<uint16_t>(gen_count));
        bags.U16(static_cast<uint16_t>(mod_count));
        bag_count++;

        for (const Modulator& m : zone.modulators)
        {
            mods.U16(m.source);
            mods.U16(m.dest);
            mods.U16(static_cast<uint16_t>(m.amount));
            mods.U16(m.amount_source);
            mods.U16(0); // linear transform
            mod_count++;
        }

        auto by_rank = [&](const Generator& a, const Generator& b)
        {
            return Rank(a.oper, preset) < Rank(b.oper, preset);
        };
        std::vector<Generator> g = zone.generators;
        std::stable_sort(g.begin(), g.end(), by_rank);
        for (const Generator& x : g)
        {
            gens.U16(x.oper);
            gens.U16(x.amount);
            gen_count++;
        }

        if (gen_count > 0xffff || mod_count > 0xffff || bag_count > 0xffff)
        {
            throw std::runtime_error("SoundFont too large (more than 65535 generators, modulators or zones)");
        }
    }

    // Append the required terminal record to each list.
    void Terminate()
    {
        bags.U16(static_cast<uint16_t>(gen_count));
        bags.U16(static_cast<uint16_t>(mod_count));
        for (int i = 0; i < 5; i++)
        {
            mods.U16(0);
        }
        gens.U32(0);
    }

    Buffer bags, mods, gens;
    uint32_t bag_count = 0, mod_count = 0, gen_count = 0;
};

// INFO metadata: version, sound engine, name, comment and generating software.
Buffer InfoList(const SoundFont& sf)
{
    Buffer ifil;
    ifil.U16(2);
    ifil.U16(1);

    Buffer info;
    info.Fourcc("INFO");
    info.Chunk("ifil", ifil.data);
    info.Chunk("isng", ZString("EMU8000"));
    info.Chunk("INAM", ZString(sf.name.substr(0, 255)));
    if (!sf.comment.empty())
    {
        info.Chunk("ICMT", ZString(sf.comment.substr(0, 65534)));
    }
    info.Chunk("ISFT", ZString("citrusf2"));

    return info;
}

// Write sdta with 46 zero samples after each sample. Include padding even when there are no samples. FluidSynth rejects
// SoundFonts with empty sample data.
Buffer SdtaList(const SoundFont& sf)
{
    Buffer smpl;
    for (const Sample& s : sf.samples)
    {
        for (int16_t v : s.data)
        {
            smpl.U16(static_cast<uint16_t>(v));
        }
        for (uint32_t i = 0; i < kSamplePadding; i++)
        {
            smpl.U16(0);
        }
    }
    if (sf.samples.empty())
    {
        smpl.data.resize(kSamplePadding * 2);
    }

    Buffer sdta;
    sdta.Fourcc("sdta");
    sdta.Chunk("smpl", smpl.data);

    return sdta;
}

// Write pdta: preset, instrument and sample headers, plus zone bags, modulators and generators.
Buffer PdtaList(const SoundFont& sf)
{
    Buffer phdr;
    ZoneLists pz;
    for (const Preset& p : sf.presets)
    {
        phdr.Name(p.name);
        phdr.U16(p.program);
        phdr.U16(p.bank);
        phdr.U16(static_cast<uint16_t>(pz.bag_count));
        phdr.U32(0);
        phdr.U32(0);
        phdr.U32(0);
        for (const Zone& z : p.zones)
        {
            pz.Add(z, true);
        }
    }

    phdr.Name("EOP");
    phdr.U16(0);
    phdr.U16(0);
    phdr.U16(static_cast<uint16_t>(pz.bag_count));
    phdr.U32(0);
    phdr.U32(0);
    phdr.U32(0);
    pz.Terminate();

    Buffer inst;
    ZoneLists iz;
    for (const Instrument& i : sf.instruments)
    {
        inst.Name(i.name);
        inst.U16(static_cast<uint16_t>(iz.bag_count));
        for (const Zone& z : i.zones)
        {
            iz.Add(z, false);
        }
    }

    inst.Name("EOI");
    inst.U16(static_cast<uint16_t>(iz.bag_count));
    iz.Terminate();

    // Positions are sample offsets within smpl. They follow the layout in SdtaList.
    Buffer shdr;
    uint32_t start = 0;
    for (const Sample& s : sf.samples)
    {
        shdr.Name(s.name);
        shdr.U32(start);
        shdr.U32(start + static_cast<uint32_t>(s.data.size()));
        shdr.U32(start + s.loop_start);
        shdr.U32(start + s.loop_end);
        shdr.U32(s.sample_rate);
        shdr.U8(s.original_key);
        shdr.U8(0); // pitch correction
        shdr.U16(s.link);
        shdr.U16(s.type);
        start += static_cast<uint32_t>(s.data.size()) + kSamplePadding;
    }

    shdr.Name("EOS");
    for (int i = 0; i < 5; i++)
    {
        shdr.U32(0);
    }
    shdr.U8(0);
    shdr.U8(0);
    shdr.U16(0);
    shdr.U16(0);

    Buffer pdta;
    pdta.Fourcc("pdta");
    pdta.Chunk("phdr", phdr.data);
    pdta.Chunk("pbag", pz.bags.data);
    pdta.Chunk("pmod", pz.mods.data);
    pdta.Chunk("pgen", pz.gens.data);
    pdta.Chunk("inst", inst.data);
    pdta.Chunk("ibag", iz.bags.data);
    pdta.Chunk("imod", iz.mods.data);
    pdta.Chunk("igen", iz.gens.data);
    pdta.Chunk("shdr", shdr.data);

    return pdta;
}

} // namespace

std::vector<uint8_t> Write(const SoundFont& sf)
{
    const Buffer lists[] = {InfoList(sf), SdtaList(sf), PdtaList(sf)};
    std::size_t size = 4; // "sfbk"
    for (const Buffer& list : lists)
    {
        size += 8 + list.data.size();
    }

    Buffer riff;
    riff.data.reserve(8 + size);
    riff.Fourcc("RIFF");
    riff.U32(static_cast<uint32_t>(size));
    riff.Fourcc("sfbk");
    for (const Buffer& list : lists)
    {
        riff.Fourcc("LIST");
        riff.U32(static_cast<uint32_t>(list.data.size()));
        riff.Bytes(list.data);
    }

    return std::move(riff.data);
}

} // namespace citrusf2::sf2
