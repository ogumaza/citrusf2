// SPDX-License-Identifier: MIT

// Bank and wave lookup (see instruments.h).

#include "instruments.h"

#include <cstddef>
#include <cstdio>
#include <utility>
#include <vector>

#include "binary.h"

namespace citrusf2
{

namespace
{

// Contiguous runs of the values 0-127 for which `owner(v)` is true.
template <typename F>
std::vector<std::pair<int, int>> Runs(F owner)
{
    std::vector<std::pair<int, int>> runs;
    for (int v = 0; v < 128; v++)
    {
        if (!owner(v))
        {
            continue;
        }

        if (!runs.empty() && runs.back().second == v - 1)
        {
            runs.back().second = v;
        }
        else
        {
            runs.push_back({v, v});
        }
    }

    return runs;
}

// Sentinel for an absent item: a bank slot with no bank, or a bank wave entry with no wave. nw::snd checks the wave
// index for this value.
constexpr uint32_t kNoItem = 0xffffffff;

// Returns an item id as the archive gives it, in hexadecimal.
std::string Hex(uint32_t item)
{
    char buf[16];
    std::snprintf(buf, sizeof(buf), "0x%08x", item);

    return buf;
}

// Describe why file `file_id` has no data. Either the file extends past the end of `archive`, or `archive` doesn't
// store the file.
std::string NoData(const SoundArchive& archive, uint32_t file_id)
{
    if (!archive.FileMissing(file_id))
    {
        return " is not stored in the archive";
    }

    return archive.Truncated() ? " extends past the truncated archive" : " extends past the archive";
}

} // namespace

std::vector<PlayedRanges> RegionRanges(const Instrument& inst)
{
    std::vector<PlayedRanges> out;
    const std::size_t nkeys = inst.keys.size();
    for (std::size_t ki = 0; ki < nkeys; ki++)
    {
        const KeyRegion& kr = inst.keys[ki];
        auto owns_key = [&](int k)
        {
            for (std::size_t j = 0; j < nkeys; j++)
            {
                if (k >= inst.keys[j].lo && k <= inst.keys[j].hi)
                {
                    return j == ki;
                }
            }

            return false;
        };
        const auto key_runs = Runs(owns_key);

        for (std::size_t vi = 0; vi < kr.velocities.size(); vi++)
        {
            const auto& vr = kr.velocities[vi];
            if (!vr.region)
            {
                continue;
            }

            auto owns_velocity = [&](int v)
            {
                for (std::size_t j = 0; j < kr.velocities.size(); j++)
                {
                    if (v >= kr.velocities[j].lo && v <= kr.velocities[j].hi)
                    {
                        return j == vi;
                    }
                }

                return false;
            };
            out.push_back({&kr, &*vr.region, key_runs, Runs(owns_velocity)});
        }
    }

    return out;
}

BankSet::BankSet(const SoundArchive& archive, const SoundInfo& sound) : archive_(archive)
{
    for (std::size_t slot = 0; slot < sound.banks.size() && slot < 4; slot++)
    {
        const uint32_t item = sound.banks[slot];
        const uint32_t index = item & 0xffffff;
        if (item == kNoItem)
        {
            continue;
        }
        if ((item >> 24) != 0x03 || index >= archive_.Banks().size())
        {
            errors_.push_back("bank slot " + std::to_string(slot) + " has " + Hex(item) + ": invalid bank ID");
            continue;
        }

        const BankInfo& info = archive_.Banks()[index];
        bank_names_[slot] = info.name;
        const auto data = archive_.FileData(info.file_id);
        if (data.empty())
        {
            errors_.push_back("bank " + bank_names_[slot] + NoData(archive_, info.file_id));
            continue;
        }

        try
        {
            banks_[slot] = Bank::Parse(data);
        }
        catch (const FormatError& e)
        {
            errors_.push_back("bank " + bank_names_[slot] + ": " + e.what());
        }
    }
}

const Bank* BankSet::GetBank(int slot) const
{
    return slot >= 0 && slot < 4 && banks_[slot] ? &*banks_[slot] : nullptr;
}

const std::string& BankSet::BankName(int slot) const
{
    static const std::string kNone;
    return slot >= 0 && slot < 4 ? bank_names_[slot] : kNone;
}

const Instrument* BankSet::GetInstrument(int slot, uint32_t program) const
{
    const Bank* bank = GetBank(slot);
    if (!bank || program >= bank->instruments.size() || !bank->instruments[program])
    {
        return nullptr;
    }

    return &*bank->instruments[program];
}

const VelocityRegion* BankSet::FindRegion(int slot, uint32_t program, int key, int velocity) const
{
    const Instrument* inst = GetInstrument(slot, program);
    return inst ? inst->Find(key, velocity) : nullptr;
}

std::optional<std::pair<uint32_t, uint32_t>> BankSet::WaveKey(int slot, uint32_t wave_id_index) const
{
    const Bank* bank = GetBank(slot);
    if (!bank || wave_id_index >= bank->waves.size())
    {
        return std::nullopt;
    }

    const WaveId& id = bank->waves[wave_id_index];
    if ((id.wave_archive_item >> 24) != 0x05 || id.index == kNoItem)
    {
        return std::nullopt;
    }

    return std::make_pair(id.wave_archive_item & 0xffffff, id.index);
}

bool BankSet::NoWave(int slot, uint32_t wave_id_index) const
{
    const Bank* bank = GetBank(slot);
    return bank && wave_id_index < bank->waves.size() && bank->waves[wave_id_index].index == kNoItem;
}

const Pcm* BankSet::GetWave(int slot, uint32_t wave_id_index)
{
    const auto key = WaveKey(slot, wave_id_index);
    if (!key)
    {
        ReportBrokenWaveId(slot, wave_id_index);
        return nullptr;
    }

    auto it = waves_.find(*key);
    if (it != waves_.end())
    {
        return it->second.get();
    }

    // Load wave archives on first use. Cache failures too. Each error is then reported once.
    const auto [war_index, wave_index] = *key;
    auto war_it = wave_archives_.find(war_index);
    if (war_it == wave_archives_.end())
    {
        std::optional<WaveArchive> war;
        if (war_index < archive_.WaveArchives().size())
        {
            const WaveArchiveInfo& info = archive_.WaveArchives()[war_index];
            const auto data = archive_.FileData(info.file_id);
            if (!data.empty())
            {
                try
                {
                    war = WaveArchive::Parse(data);
                }
                catch (const FormatError& e)
                {
                    errors_.push_back("wave archive " + info.name + ": " + e.what());
                }
            }
            else
            {
                errors_.push_back("wave archive " + info.name + NoData(archive_, info.file_id));
            }
        }
        else
        {
            errors_.push_back("wave archive " + std::to_string(war_index) + " doesn't exist (the archive has " +
                              std::to_string(archive_.WaveArchives().size()) + ")");
        }

        war_it = wave_archives_.emplace(war_index, std::move(war)).first;
    }

    std::unique_ptr<Pcm> pcm;
    if (war_it->second && wave_index < war_it->second->waves.size())
    {
        try
        {
            pcm = std::make_unique<Pcm>(DecodeWave(war_it->second->waves[wave_index]));
        }
        catch (const FormatError& e)
        {
            errors_.push_back("wave " + std::to_string(war_index) + "/" + std::to_string(wave_index) + ": " + e.what());
        }
    }
    else if (war_it->second)
    {
        errors_.push_back("wave archive " + archive_.WaveArchives()[war_index].name + " has no wave " +
                          std::to_string(wave_index));
    }

    return waves_.emplace(*key, std::move(pcm)).first->second.get();
}

void BankSet::ReportBrokenWaveId(int slot, uint32_t wave_id_index)
{
    const Bank* bank = GetBank(slot);
    if (!bank || !broken_wave_ids_.emplace(slot, wave_id_index).second)
    {
        return; // unreadable banks and previously checked wave IDs have already been reported
    }

    if (wave_id_index >= bank->waves.size())
    {
        errors_.push_back("bank " + bank_names_[slot] + " has no wave " + std::to_string(wave_id_index));
    }
    else if (!NoWave(slot, wave_id_index))
    {
        errors_.push_back("bank " + bank_names_[slot] + "'s wave " + std::to_string(wave_id_index) + " is in " +
                          Hex(bank->waves[wave_id_index].wave_archive_item) + ": invalid wave archive ID");
    }
}

} // namespace citrusf2
