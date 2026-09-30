// SPDX-License-Identifier: MIT

// Sound archive and file readers (see csar.h).

#include "csar.h"

#include <algorithm>
#include <cstddef>
#include <map>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>

#include "binary.h"

namespace citrusf2
{

namespace
{

// A region table entry with no target. Notes in this region are silent.
constexpr std::size_t kNoTarget = static_cast<std::size_t>(-1);

// Optional parameters: 32 flag bits followed by one 32-bit value per set bit.
struct OptionParams
{
    static OptionParams Read(const Reader& r, std::size_t off)
    {
        OptionParams p;
        const uint32_t flags = r.U32(off);
        std::size_t q = off + 4;
        for (int bit = 0; bit < 32; bit++)
        {
            if (flags >> bit & 1)
            {
                p.values[bit] = r.U32(q);
                p.value_offset[bit] = q;
                q += 4;
            }
        }

        return p;
    }

    std::array<std::optional<uint32_t>, 32> values{};
    std::array<std::size_t, 32> value_offset{};
};

// Block offsets and sizes in a CTR binary file, indexed by type.
using Blocks = std::map<uint16_t, std::pair<uint32_t, uint32_t>>;

// Check the file magic (CSAR, CWAV, etc.), the 3DS byte order and the fixed header size before reading the block table.
void CheckHeader(const Reader& r, const char* magic)
{
    if (r.Size() < 0x14)
    {
        throw FormatError(std::string("too short to be a ") + magic + " file");
    }
    if (std::string(magic) == "CSAR" && r.Magic(0, "FSAR"))
    {
        throw FormatError("unsupported FSAR archive (Wii U/Switch); expected CSAR (3DS)");
    }
    if (!r.Magic(0, magic))
    {
        throw FormatError(std::string("bad magic, expected ") + magic);
    }
    if (r.U16(4) != 0xFEFF)
    {
        throw FormatError("unsupported byte order");
    }
}

// The block table of a CTR binary file: {type, offset, size} entries after a 0x14-byte header.
Blocks BlockTable(const Reader& r, const char* magic)
{
    CheckHeader(r, magic);

    const uint16_t count = r.U16(0x10);
    Blocks blocks;
    for (uint16_t i = 0; i < count; i++)
    {
        const std::size_t e = 0x14 + i * 12;
        blocks[r.U16(e)] = {r.U32(e + 4), r.U32(e + 8)};
    }

    return blocks;
}

// Return a required block's offset and size as std::size_t. On 64-bit builds, derived offsets cannot wrap at 32 bits
// and point back into the file. On 32-bit builds they can, so corrupt files may produce misleading errors.
std::pair<std::size_t, std::size_t> RequiredBlock(const Blocks& blocks, uint16_t type, const char* name)
{
    const auto it = blocks.find(type);
    if (it == blocks.end())
    {
        throw FormatError(std::string("no ") + name + " block");
    }

    return it->second;
}

// Read a bank's velocity region at `p`: its wave and optional playback parameters.
VelocityRegion ReadVelocityRegion(const Reader& r, std::size_t p)
{
    // nw::snd BankFileReader (code.bin 0x48e2f8 and accessors 0x490b28..0x490cc4).
    VelocityRegion v;
    v.wave_id_index = r.U32(p);
    const OptionParams opt = OptionParams::Read(r, p + 4);

    if (opt.values[0])
    {
        v.original_key = static_cast<uint8_t>(*opt.values[0]);
    }

    if (opt.values[1])
    {
        v.volume = static_cast<uint8_t>(*opt.values[1]);
    }

    if (opt.values[2])
    {
        v.pan = static_cast<uint8_t>(*opt.values[2]);
    }

    if (opt.values[3])
    {
        v.pitch = r.F32(opt.value_offset[3]);
    }

    if (opt.values[4])
    {
        v.ignore_note_off = (*opt.values[4] & 0xff) != 0;
        v.key_group = static_cast<uint8_t>(*opt.values[4] >> 8);
        v.interpolation = static_cast<uint8_t>(*opt.values[4] >> 16);
    }

    if (opt.values[9])
    {
        // The value points to a {type, offset} reference. The ADSHR data is at that reference plus its offset.
        const std::size_t ref_at = p + *opt.values[9];
        const Reference ref = ReadRef(r, ref_at);
        const std::size_t a = ref_at + ref.offset;
        v.adshr = {r.U8(a), r.U8(a + 1), r.U8(a + 2), r.U8(a + 3), r.U8(a + 4)};
    }

    return v;
}

// Read a direct, range or index region table. Return each region's lower and upper bounds and its location, or
// kNoTarget.
std::vector<std::tuple<uint8_t, uint8_t, std::size_t>> ReadRegionTable(const Reader& r, std::size_t p)
{
    const Reference ref = ReadRef(r, p);
    const std::size_t q = p + ref.offset;

    auto target = [&](std::size_t ref_off, std::size_t base) -> std::size_t
    {
        const Reference t = ReadRef(r, ref_off);
        if (t.offset == -1 || t.type == 0x5903)
        {
            return kNoTarget;
        }

        return base + t.offset;
    };

    std::vector<std::tuple<uint8_t, uint8_t, std::size_t>> out;
    switch (ref.type)
    {
    case 0x6000: // direct
        out.emplace_back(uint8_t{0}, uint8_t{127}, target(q, q));
        break;

    case 0x6001: // range: count, upper keys, refs
        {
            const uint32_t n = r.U32(q);
            const std::size_t refs = q + 4 + ((n + 3) & ~3u);
            uint8_t lo = 0;
            for (uint32_t i = 0; i < n; i++)
            {
                const uint8_t hi = r.U8(q + 4 + i);
                out.emplace_back(lo, hi, target(refs + i * 8, q));
                lo = static_cast<uint8_t>(hi + 1);
            }
            break;
        }

    case 0x6002: // index: min, max, refs
        {
            const uint8_t mn = r.U8(q), mx = r.U8(q + 1);
            for (int k = mn; k <= mx; k++)
            {
                out.emplace_back(static_cast<uint8_t>(k), static_cast<uint8_t>(k), target(q + 4 + (k - mn) * 8, q));
            }
            break;
        }

    default:
        throw FormatError("unknown region table type");
    }

    return out;
}

// Decode the first UTF-8 character in nonempty `s`. Return its code point and byte length, or length 0 for invalid
// UTF-8.
std::pair<uint32_t, std::size_t> Utf8Character(std::string_view s)
{
    const auto byte = [&](std::size_t i)
    {
        return static_cast<uint8_t>(s[i]);
    };

    const uint8_t lead = byte(0);
    const std::size_t length = lead < 0x80   ? 1
                               : lead < 0xc2 ? 0
                               : lead < 0xe0 ? 2
                               : lead < 0xf0 ? 3
                               : lead < 0xf5 ? 4
                                             : 0;
    if (length == 0 || length > s.size())
    {
        return {0, 0};
    }

    uint32_t c = length == 1 ? lead : lead & (0x7f >> length);
    for (std::size_t i = 1; i < length; i++)
    {
        if ((byte(i) & 0xc0) != 0x80)
        {
            return {0, 0};
        }

        c = c << 6 | (byte(i) & 0x3f);
    }

    // Reject overlong encodings, UTF-16 surrogates and code points above U+10FFFF.
    constexpr uint32_t kShortest[] = {0, 0, 0x80, 0x800, 0x10000};
    if (c < kShortest[length] || (c >= 0xd800 && c < 0xe000) || c > 0x10ffff)
    {
        return {0, 0};
    }

    return {c, length};
}

// Replace control characters, line/paragraph separators and invalid UTF-8 bytes with '?'. These can disrupt terminal
// output and the macOS app's parser: AppleScript splits on U+2029 and falls back to another encoding if any byte is
// invalid UTF-8. macOS also requires valid UTF-8 file names.
std::string PrintableName(std::string_view name)
{
    std::string printable;
    for (std::size_t i = 0; i < name.size();)
    {
        const auto [c, length] = Utf8Character(name.substr(i));
        const bool control = c < 0x20 || (c >= 0x7f && c < 0xa0) || c == 0x2028 || c == 0x2029;
        printable += length == 0 || control ? "?" : name.substr(i, length);
        i += std::max<std::size_t>(length, 1);
    }

    return printable;
}

// Read the STRG block at `strg_off`, of length `strg_size`: a string table and a Patricia tree mapping names to item
// IDs.
std::map<uint32_t, std::string> ReadItemNames(const Reader& r, std::size_t strg_off, std::size_t strg_size)
{
    const std::size_t b = strg_off + 8;
    std::size_t st = 0, pt = 0;
    for (int i = 0; i < 2; i++)
    {
        const Reference ref = ReadRef(r, b + i * 8);
        if (ref.type == 0x2400)
        {
            st = b + ref.offset;
        }
        else if (ref.type == 0x2401)
        {
            pt = b + ref.offset;
        }
    }

    // Strings are stored consecutively, one per item. A corrupt table or tree could reference the same long string many
    // times and exhaust memory. Reject it if the total string length exceeds the block size.
    const std::size_t room = std::min(strg_size, r.Size());
    auto check_room = [&](std::size_t used)
    {
        if (used > room)
        {
            throw FormatError("invalid string table (total string length exceeds block size)");
        }
    };

    std::vector<std::string> strings;
    std::size_t string_bytes = 0;
    const uint32_t n = r.U32(st);
    for (uint32_t i = 0; i < n; i++)
    {
        const std::size_t e = st + 4 + i * 12;
        const std::string& name = strings.emplace_back(PrintableName(r.CString(st + r.U32(e + 4), r.U32(e + 8))));
        string_bytes += name.size();
        check_room(string_bytes);
    }

    std::map<uint32_t, std::string> item_names;
    std::size_t name_bytes = 0;
    const uint32_t nodes = r.U32(pt + 4);
    for (uint32_t i = 0; i < nodes; i++)
    {
        const std::size_t e = pt + 8 + i * 20;
        if (r.U16(e) & 1)
        {
            const uint32_t sid = r.U32(e + 12), iid = r.U32(e + 16);
            if (sid < strings.size())
            {
                item_names[iid] = strings[sid];
                name_bytes += strings[sid].size();
                check_room(name_bytes);
            }
        }
    }

    return item_names;
}

// Read sound `index` from the INFO entry at `p`.
SoundInfo ReadSound(const Reader& r, std::size_t p, uint32_t index, std::string name)
{
    SoundInfo s;
    s.name = std::move(name);
    s.file_id = r.U32(p);
    s.volume = r.U8(p + 8);

    // Pan mode and curve share the second optional parameter.
    const OptionParams options = OptionParams::Read(r, p + 20);
    if (options.values[1])
    {
        s.pan_mode = static_cast<uint8_t>(*options.values[1]);
        s.pan_curve = static_cast<uint8_t>(*options.values[1] >> 8);
    }

    const Reference detail = ReadRef(r, p + 12);
    switch (detail.type)
    {
    case 0x2201:
        s.type = SoundType::kStream;
        break;

    case 0x2202:
        s.type = SoundType::kWaveSound;
        break;

    case 0x2203:
        {
            s.type = SoundType::kSequence;
            const std::size_t q = p + detail.offset;

            // nw::snd has four bank slots. A corrupt table may claim many more.
            const Reference banks = ReadRef(r, q);
            const std::size_t bt = q + banks.offset;
            const uint32_t nb = r.U32(bt);
            for (uint32_t k = 0; k < nb && k < 4; k++)
            {
                s.banks.push_back(r.U32(bt + 4 + k * 4));
            }

            s.allocate_track_flags = r.U32(q + 8);
            const OptionParams so = OptionParams::Read(r, q + 12);
            if (so.values[0])
            {
                s.start_offset = *so.values[0];
            }
            break;
        }

    default:
        break;
    }

    // Generate a name from the sound's type and index if the archive has none.
    if (s.name.empty())
    {
        const char* prefix = s.type == SoundType::kSequence    ? "SEQ_"
                             : s.type == SoundType::kStream    ? "STRM_"
                             : s.type == SoundType::kWaveSound ? "WSD_"
                                                               : "SOUND_";
        s.name = prefix + std::to_string(index);
    }

    return s;
}

} // namespace

Wave Wave::Parse(std::span<const uint8_t> file)
{
    Reader r(file);
    const Blocks blocks = BlockTable(r, "CWAV");
    const auto [info_off, info_size] = RequiredBlock(blocks, 0x7000, "INFO");
    const auto [data_off, data_size] = RequiredBlock(blocks, 0x7001, "DATA");

    const std::size_t b = info_off + 8;
    Wave w;
    w.encoding = static_cast<WaveEncoding>(r.U8(b));
    w.loop = r.U8(b + 1) != 0;
    w.sample_rate = r.U32(b + 4);
    w.loop_start = r.U32(b + 8);
    w.loop_end = r.U32(b + 12);

    const std::size_t table = b + 0x14;
    const uint32_t nch = r.U32(table);
    for (uint32_t i = 0; i < nch; i++)
    {
        const Reference ref = ReadRef(r, table + 4 + i * 8);
        const std::size_t ci = table + ref.offset;
        const Reference sample_ref = ReadRef(r, ci);
        const Reference adpcm_ref = ReadRef(r, ci + 8);
        WaveChannel ch;
        const std::size_t sample_off = data_off + 8 + sample_ref.offset;
        ch.data = r.Sub(sample_off, data_off + data_size - sample_off);

        if (adpcm_ref.type == 0x0300)
        {
            const std::size_t a = ci + adpcm_ref.offset;
            DspAdpcmInfo info;
            for (int k = 0; k < 16; k++)
            {
                info.coefs[k] = r.S16(a + k * 2);
            }

            info.yn1 = r.S16(a + 34);
            info.yn2 = r.S16(a + 36);
            ch.adpcm = info;
        }
        else if (adpcm_ref.type == 0x0301)
        {
            const std::size_t a = ci + adpcm_ref.offset;
            ImaAdpcmInfo info;
            info.sample = r.S16(a);
            info.step_index = r.U8(a + 2);
            ch.ima = info;
        }

        w.channels.push_back(ch);
    }

    return w;
}

WaveArchive WaveArchive::Parse(std::span<const uint8_t> file)
{
    Reader r(file);
    const Blocks blocks = BlockTable(r, "CWAR");
    const auto [info_off, info_size] = RequiredBlock(blocks, 0x6800, "INFO");
    const auto [file_off, file_size] = RequiredBlock(blocks, 0x6801, "FILE");

    WaveArchive war;
    const uint32_t n = r.U32(info_off + 8);
    for (uint32_t i = 0; i < n; i++)
    {
        const std::size_t e = info_off + 12 + i * 12;
        const uint32_t off = r.U32(e + 4);
        const uint32_t size = r.U32(e + 8);
        war.waves.push_back(r.Sub(file_off + 8 + off, size));
    }

    return war;
}

const VelocityRegion* Instrument::Find(int key, int velocity) const
{
    for (const auto& k : keys)
    {
        if (key < k.lo || key > k.hi)
        {
            continue;
        }

        // The first matching key region wins, even if none of its velocity ranges match.
        for (const auto& v : k.velocities)
        {
            if (velocity >= v.lo && velocity <= v.hi)
            {
                return v.region ? &*v.region : nullptr;
            }
        }

        return nullptr;
    }

    return nullptr;
}

Bank Bank::Parse(std::span<const uint8_t> file)
{
    Reader r(file);
    const Blocks blocks = BlockTable(r, "CBNK");
    const auto [info_off, info_size] = RequiredBlock(blocks, 0x5800, "INFO");

    const std::size_t b = info_off + 8;
    std::size_t wave_table = 0, inst_table = 0;
    for (int i = 0; i < 2; i++)
    {
        const Reference ref = ReadRef(r, b + i * 8);
        if (ref.type == 0x0100)
        {
            wave_table = b + ref.offset;
        }
        else if (ref.type == 0x0101)
        {
            inst_table = b + ref.offset;
        }
    }

    Bank bank;
    const uint32_t nw = r.U32(wave_table);
    for (uint32_t i = 0; i < nw; i++)
    {
        bank.waves.push_back({r.U32(wave_table + 4 + i * 8), r.U32(wave_table + 8 + i * 8)});
    }

    const uint32_t ni = r.U32(inst_table);
    std::size_t regions = 0;
    for (uint32_t i = 0; i < ni; i++)
    {
        const Reference ref = ReadRef(r, inst_table + 4 + i * 8);
        if (ref.type != 0x5900)
        {
            bank.instruments.emplace_back();
            continue;
        }

        Instrument inst;
        for (auto [lo, hi, key_off] : ReadRegionTable(r, inst_table + ref.offset))
        {
            KeyRegion k;
            k.lo = lo;
            k.hi = hi;

            if (key_off != kNoTarget)
            {
                for (auto [vlo, vhi, vel_off] : ReadRegionTable(r, key_off))
                {
                    KeyRegion::Vel v{vlo, vhi, std::nullopt};
                    if (vel_off != kNoTarget)
                    {
                        v.region = ReadVelocityRegion(r, vel_off);
                    }

                    k.velocities.push_back(v);
                }
            }

            // A bank has fewer regions than bytes. Shared table entries in a corrupt bank could otherwise allocate more
            // regions than fit in memory.
            regions += 1 + k.velocities.size();
            if (regions > file.size())
            {
                throw FormatError("invalid region tables (region count exceeds bank size in bytes)");
            }

            inst.keys.push_back(std::move(k));
        }

        bank.instruments.push_back(std::move(inst));
    }

    return bank;
}

Sequence Sequence::Parse(std::span<const uint8_t> file)
{
    Reader r(file);
    const Blocks blocks = BlockTable(r, "CSEQ");
    const auto [data_off, data_size] = RequiredBlock(blocks, 0x5000, "DATA");

    Sequence s;
    s.data = r.Sub(data_off + 8, data_size - 8);

    return s;
}

SoundArchive SoundArchive::Load(std::vector<uint8_t> bytes)
{
    SoundArchive a;
    a.bytes_ = std::move(bytes);
    const Reader r(a.bytes_);
    CheckHeader(r, "CSAR"); // report other archive formats separately

    // Read as much of a truncated archive as possible. Use the size in its header to detect truncation, and include
    // that in the error if its tables cannot be read.
    const uint32_t declared_size = r.U32(0x0C);
    a.truncated_ = declared_size > a.bytes_.size();
    try
    {
        a.ReadTables(r);
    }
    catch (const FormatError& e)
    {
        if (!a.truncated_)
        {
            throw;
        }

        throw FormatError("truncated archive: " + std::to_string(a.bytes_.size()) + " of " +
                          std::to_string(declared_size) + " bytes; cannot read tables (" + e.what() + ")");
    }

    return a;
}

void SoundArchive::ReadTables(const Reader& r)
{
    const Blocks blocks = BlockTable(r, "CSAR");
    const auto [info_off, info_size] = RequiredBlock(blocks, 0x2001, "INFO");
    const auto [file_off, file_size] = RequiredBlock(blocks, 0x2002, "FILE");

    // Archives without names have a STRG offset of 0xFFFFFFFF.
    std::map<uint32_t, std::string> item_names;
    const auto strg = blocks.find(0x2000);
    if (strg != blocks.end() && strg->second.first != 0xFFFFFFFF && strg->second.first < bytes_.size())
    {
        item_names = ReadItemNames(r, strg->second.first, strg->second.second);
    }

    auto name_of = [&](uint32_t item) -> std::string
    {
        const auto it = item_names.find(item);
        return it == item_names.end() ? std::string() : it->second;
    };

    // INFO starts with references to the sound, bank, player, wave archive, file and other tables.
    const std::size_t b = info_off + 8;
    std::map<uint16_t, std::size_t> tables;
    for (int i = 0; i < 8; i++)
    {
        const Reference ref = ReadRef(r, b + i * 8);
        tables[ref.type] = b + ref.offset;
    }

    // Call `read` with each table entry's index and position.
    auto for_each_entry = [&](uint16_t type, const char* name, auto read)
    {
        const auto table = tables.find(type);
        if (table == tables.end())
        {
            throw FormatError(std::string("no ") + name + " table");
        }

        const std::size_t t = table->second;
        const uint32_t n = r.U32(t);
        for (uint32_t i = 0; i < n; i++)
        {
            read(i, t + ReadRef(r, t + 4 + i * 8).offset);
        }
    };

    auto read_sound = [&](uint32_t i, std::size_t p)
    {
        sounds_.push_back(ReadSound(r, p, i, name_of(0x01000000 | i)));
    };
    for_each_entry(0x2100, "sound", read_sound);

    // Generate missing bank and wave archive names from their indexes, as ReadSound does for sounds.
    auto read_bank = [&](uint32_t i, std::size_t p)
    {
        const std::string name = name_of(0x03000000 | i);
        banks_.push_back({name.empty() ? "BANK_" + std::to_string(i) : name, r.U32(p)});
    };
    for_each_entry(0x2101, "bank", read_bank);

    auto read_wave_archive = [&](uint32_t i, std::size_t p)
    {
        const std::string name = name_of(0x05000000 | i);
        wave_archives_.push_back({name.empty() ? "WARC_" + std::to_string(i) : name, r.U32(p)});
    };
    for_each_entry(0x2103, "wave archive", read_wave_archive);

    // Files can be in the FILE block (0x220C), external (0x220D, a file name), or have no location (a null reference or
    // offset 0xFFFFFFFF). Files without locations may be stored in groups.
    auto read_file = [&](uint32_t, std::size_t p)
    {
        FileEntry f;
        const Reference loc = ReadRef(r, p);
        if (loc.type == 0x220C)
        {
            const std::size_t q = p + loc.offset;
            const uint32_t off = r.U32(q + 4), size = r.U32(q + 8);
            if (off != 0xFFFFFFFF)
            {
                // Keep out-of-bounds file locations so that partial groups can still be read. Only the header
                // determines whether the archive is truncated; a corrupt file table can point outside a complete
                // archive too.
                const uint64_t start = static_cast<uint64_t>(file_off) + 8 + off;
                f.offset = static_cast<std::size_t>(std::min<uint64_t>(start, bytes_.size()));
                f.size = size;
                f.internal = start + size <= bytes_.size();
                f.missing = !f.internal;
            }
        }

        f.grouped = !f.internal && !f.missing && loc.type != 0x220D;
        files_.push_back(f);
    };
    for_each_entry(0x2106, "file", read_file);

    // Each group has a CGRP file listing its contents. Use the first copy of a file found in a group. Copies can differ
    // because banks may reference wave archives specific to their group, but they play the same waves. A group with
    // file ID 0xFFFFFFFF is not stored in the archive.
    std::vector<std::pair<std::string, uint32_t>> cut_short;
    auto read_group = [&](uint32_t i, std::size_t p)
    {
        const std::string name = name_of(0x06000000 | i);
        const uint32_t file_id = r.U32(p);
        if (file_id < files_.size())
        {
            ReadGroup(name.empty() ? "GROUP_" + std::to_string(i) : name, files_[file_id], cut_short);
        }
    };
    if (tables.contains(0x2105))
    {
        try
        {
            for_each_entry(0x2105, "group", read_group);
        }
        catch (const FormatError& e)
        {
            // Keep files from groups read before the bad entry.
            group_errors_.push_back(std::string("cannot read group table (") + e.what() +
                                    "); files stored only in groups may be missing");
        }
    }

    // Omit truncated group files unless another group has a complete copy.
    for (const auto& [group, id] : cut_short)
    {
        if (!files_[id].internal)
        {
            group_errors_.push_back("group " + group + ": file " + std::to_string(id) +
                                    " extends past the group; skipped");
        }
    }

    // Missing groups may contain files absent from the groups we could read.
    if (groups_cut_)
    {
        for (FileEntry& f : files_)
        {
            f.missing = f.missing || (f.grouped && !f.internal);
        }
    }
}

void SoundArchive::ReadGroup(const std::string& name, const FileEntry& group,
                             std::vector<std::pair<std::string, uint32_t>>& cut_short)
{
    if (!group.internal && !group.missing)
    {
        return; // external group file
    }

    // Read the available portion of a group in a truncated archive.
    groups_cut_ = groups_cut_ || group.missing;
    const std::span<const uint8_t> data = std::span<const uint8_t>(bytes_).subspan(
        group.offset, std::min<std::size_t>(group.size, bytes_.size() - group.offset));
    try
    {
        Reader g(data);
        const Blocks blocks = BlockTable(g, "CGRP");
        const auto [info_off, info_size] = RequiredBlock(blocks, 0x7800, "INFO");
        const auto [file_off, file_size] = RequiredBlock(blocks, 0x7801, "FILE");

        // INFO lists file IDs and locations in the group's FILE block. A null reference names a file stored directly in
        // the archive.
        const std::size_t t = info_off + 8;
        const uint32_t n = g.U32(t);
        for (uint32_t i = 0; i < n; i++)
        {
            const std::size_t e = t + ReadRef(g, t + 4 + i * 8).offset;
            const uint32_t id = g.U32(e);
            const Reference ref = ReadRef(g, e + 4);
            const uint32_t size = g.U32(e + 12);
            if (ref.type != 0x1F00 || id >= files_.size() || !files_[id].grouped || files_[id].internal)
            {
                continue;
            }

            const uint64_t start = static_cast<uint64_t>(file_off) + 8 + static_cast<uint32_t>(ref.offset);
            if (start + size <= data.size())
            {
                files_[id].internal = true;
                files_[id].missing = false;
                files_[id].offset = group.offset + static_cast<std::size_t>(start);
                files_[id].size = size;
            }
            else if (group.missing)
            {
                files_[id].missing = true;
            }
            else
            {
                cut_short.emplace_back(name, id);
            }
        }
    }
    catch (const FormatError& e)
    {
        if (!group.missing)
        {
            group_errors_.push_back("group " + name + " is unreadable (" + e.what() +
                                    "); omitting files stored only in this group");
        }
    }
}

std::span<const uint8_t> SoundArchive::FileData(uint32_t file_id) const
{
    if (file_id >= files_.size() || !files_[file_id].internal)
    {
        return {};
    }

    return std::span<const uint8_t>(bytes_).subspan(files_[file_id].offset, files_[file_id].size);
}

} // namespace citrusf2
