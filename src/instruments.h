// SPDX-License-Identifier: MIT

// The banks, instruments and decoded waves a sound uses.

#pragma once

#include <array>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "csar.h"
#include "wave.h"

namespace citrusf2
{

// Effective key and velocity ranges for one velocity region. nw::snd chooses the first matching key region, then the
// first matching velocity region within it. Earlier entries can therefore mask part of a later region.
struct PlayedRanges
{
    const KeyRegion* key_region = nullptr;
    const VelocityRegion* region = nullptr;
    std::vector<std::pair<int, int>> keys, velocities;
};

// Return effective ranges for `inst` in bank order, omitting velocity entries without a region.
std::vector<PlayedRanges> RegionRanges(const Instrument& inst);

// A sequence's banks, with waves decoded on demand. Read failures are collected in Errors() and silence only the
// affected notes.
class BankSet
{
public:
    BankSet(const SoundArchive& archive, const SoundInfo& sound);

    // Return the bank in `slot`, or nullptr. Only four slots exist. nw::snd accepts higher slots and reads past its
    // bank table; those slots are silent here, as in 3SF's model.
    const Bank* GetBank(int slot) const;

    // Return the bank name, BANK_<index> for unnamed banks, or an empty string for an empty slot.
    const std::string& BankName(int slot) const;

    // An instrument of the bank in a slot, or nullptr.
    const Instrument* GetInstrument(int slot, uint32_t program) const;

    // The velocity region that plays `key` at `velocity` on an instrument, or nullptr.
    const VelocityRegion* FindRegion(int slot, uint32_t program, int key, int velocity) const;

    // Decode and cache a bank wave entry, or return nullptr. Invalid references are added to Errors(); intentional
    // no-wave entries are not (see NoWave).
    const Pcm* GetWave(int slot, uint32_t wave_id_index);

    // Identify a shared wave across banks by (wave archive index, wave index).
    std::optional<std::pair<uint32_t, uint32_t>> WaveKey(int slot, uint32_t wave_id_index) const;

    // True for the no-wave sentinel, wave index 0xFFFFFFFF. Such regions are silent in the game too.
    bool NoWave(int slot, uint32_t wave_id_index) const;

    // Read failures for banks, wave archives and waves.
    const std::vector<std::string>& Errors() const
    {
        return errors_;
    }

private:
    // Report an invalid wave reference that WaveKey could not resolve. Report each entry only once.
    void ReportBrokenWaveId(int slot, uint32_t wave_id_index);

    const SoundArchive& archive_;
    std::array<std::optional<Bank>, 4> banks_;
    std::array<std::string, 4> bank_names_;
    std::map<uint32_t, std::optional<WaveArchive>> wave_archives_; // by wave archive index
    std::map<std::pair<uint32_t, uint32_t>, std::unique_ptr<Pcm>> waves_;
    std::set<std::pair<int, uint32_t>> broken_wave_ids_; // bank slot and wave table index
    std::vector<std::string> errors_;
};

} // namespace citrusf2
