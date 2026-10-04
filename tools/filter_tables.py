#!/usr/bin/env python3
# SPDX-License-Identifier: MIT

"""Generate SoundFont and SFZ filter tables for src/filter.cpp.

usage: filter_tables.py <code.bin>

Read a locally supplied Pokemon X code.bin; the executable is not distributed with citrusf2 or 3SF.
Extract kLpfFreq (0x56e358), nn::math's cosine table (0x560ac0), and the five biquad preset tables in Q14
(from 0x574e7c).

The track low-pass (0xd8) is a one-pole filter, y = b0 x + a1 y1. CalcMonoFilterCoefficients (0x17f210) uses
the table cosine to compute coefficients, raising the lowest cutoffs above the exact-cosine result. The
biquad presets are Robert Bristow-Johnson cookbook filters: low/high-pass at about -0.5 dB gain and
band-pass at 0 dB peak gain. Each coefficient set reduces to a cutoff or centre frequency, Q and gain.

SFZ: sfizz's lpf_1p has pole exp(-2 pi f / rate); lpf_2p, hpf_2p and bpf_2p use cookbook filters with Q
expressed in dB. Match the game's coefficients at 32728 Hz.

SoundFont: FluidSynth supplies one cookbook low-pass (fluid_iir_filter.c), with Q = 10^((resonance / 10 -
3.01) / 20). It matches the biquad low-pass. Approximate the one-pole by fitting cutoff at 44100 Hz against
the game's response at 32728 Hz. Minimize RMS dB error relative to 20 Hz over 400 log-spaced frequencies
from 20 Hz to 14 kHz, flooring both responses at -30 dB.

Requires Python 3 and numpy.
"""
import argparse
import math
import struct
import sys
from pathlib import Path

try:
    import numpy as np
except ImportError:
    sys.exit("filter_tables.py needs numpy (pip install numpy)")

CODE_BASE = 0x100000  # code.bin's load address
LPF_FREQ = 0x56e358  # kLpfFreq: 24 uint16 cutoffs in Hz, the last (16000) switching the filter off
COS_TABLE = 0x560ac0  # nn::math: 256 entries of sine, cosine and their differences to the next entry, as floats
BIQUADS = {  # type: (address, sets, name)
    1: (0x574e7c, 112, "low-pass"),
    2: (0x5752dc, 97, "high-pass"),
    3: (0x5756a6, 122, "band-pass 512"),
    4: (0x575b6a, 93, "band-pass 1024"),
    5: (0x575f0c, 93, "band-pass 2048"),
}
GAME_RATE = 32728.0  # Hz, the DSP's output rate
OUT_RATE = 44100.0  # Hz, the rate FluidSynth usually plays at
FREQS = np.geomspace(20.0, 14000.0, 400)
FLOOR_DB = -30.0
CUTOFFS = np.arange(1500, 13501, 10)  # the SoundFont cutoffs tried, in absolute cents
SFZ_LOW_PASS_BASE = 20000.0  # Hz: reference cutoff for the SFZ low-pass table (filter open)
SFZ_BIQUAD_BASE = 1000.0  # Hz: reference cutoff for the SFZ biquad table
f32 = np.float32


def read(code, fmt, address):
    return struct.unpack_from(fmt, code, address - CODE_BASE)


def one_pole(code, freq):
    """Compute one-pole b0 and a1 in single precision, as nn::snd does. Obtain cos(2 pi freq / 32000) by linear
    interpolation in nn::math's table (CosFIdx, 0x172d44). Then b = 2 - cos, c = sqrt(b^2 - 1) - b, b0 = (1
    + c) * 32768, a1 = -c * 32768. Truncate coefficients to integers."""
    x = f32(min(freq, 16000)) * f32(0.008)
    i = int(x)
    _, cos, _, cos_delta = read(code, "<4f", COS_TABLE + 16 * i)
    w = f32(cos) + (x - f32(i)) * f32(cos_delta)
    b = f32(2.0) - w
    c = f32(np.sqrt(b * b - f32(1.0))) - b
    return int(np.int32(f32((c + f32(1.0)) * f32(32768.0)))), -int(np.int32(f32(c * f32(32768.0))))


def response_db(b, a, rate, freqs=FREQS):
    """Compute the response of y = b0 x + b1 x1 + b2 x2 - a1 y1 - a2 y2 at `rate`. Return dB relative to the
    first frequency, clamped to the floor."""
    z = np.exp(-2j * np.pi * freqs / rate)
    h = (b[0] + b[1] * z + b[2] * z * z) / (1.0 + a[0] * z + a[1] * z * z)
    db = 20.0 * np.log10(np.maximum(np.abs(h), 1e-12))
    return np.maximum(db - db[0], FLOOR_DB)


def fluidsynth_db(cents, resonance):
    """Compute FluidSynth filter coefficients for cutoff in absolute cents and resonance in cB."""
    fres = min(max(8.176 * 2.0 ** (cents / 1200.0), 5.0), 0.45 * OUT_RATE)
    q = 10.0 ** ((min(max(resonance / 10.0, 0.0), 96.0) - 3.01) / 20.0)
    w = 2.0 * np.pi * fres / OUT_RATE
    alpha = np.sin(w) / (2.0 * q)
    b1 = (1.0 - np.cos(w)) / (1.0 + alpha)
    return response_db((b1 / 2.0, b1, b1 / 2.0), (-2.0 * np.cos(w) / (1.0 + alpha), (1.0 - alpha) / (1.0 + alpha)),
                       OUT_RATE)


def best_cutoff(target):
    """Find the zero-resonance SoundFont cutoff closest to `target`. Return cutoff in absolute cents and RMS
    error in dB."""
    errors = [np.sqrt(np.mean((fluidsynth_db(c, 0) - target) ** 2)) for c in CUTOFFS]
    i = int(np.argmin(errors))
    return int(CUTOFFS[i]), float(errors[i])


def decode_biquad(kind, coefs):
    """Decode a game biquad's cutoff/centre (Hz at DSP rate), Q and gain (dB) from Q14 coefficients in y = b0 x
    + b1 x1 + b2 x2 + a1 y1 + a2 y2. Measure gain at cookbook unity: DC for low-pass, Nyquist for high-pass,
    centre for band-pass."""
    b0, b1, b2, a1, a2 = (v / 16384.0 for v in coefs)
    alpha = (1.0 + a2) / (1.0 - a2)  # a2 = -(1 - alpha) / (1 + alpha)
    cos_w0 = a1 * (1.0 + alpha) / 2.0  # a1 = 2 cos w0 / (1 + alpha)
    w0 = math.acos(max(-1.0, min(1.0, cos_w0)))
    q = math.sin(w0) / (2.0 * alpha)
    if kind == 1:
        z = 1.0
    elif kind == 2:
        z = -1.0
    else:
        z = complex(math.cos(w0), -math.sin(w0))
    gain = abs((b0 + b1 * z + b2 * z * z) / (1.0 - a1 * z - a2 * z * z))
    return w0 * GAME_RATE / (2.0 * math.pi), q, 20.0 * math.log10(gain)


def cents(ratio):
    return int(round(1200.0 * math.log2(ratio)))


def table(ctype, name, comment, values, per_line=10):
    """Format a C++ table with `per_line` values per line, followed by a blank line."""
    lines = [f"// {comment}", f"constexpr {ctype} {name}[{len(values)}] = {{"]
    for i in range(0, len(values), per_line):
        lines.append("    " + ", ".join(str(v) for v in values[i:i + per_line]) + ",")
    lines.append("};\n")
    return "\n".join(lines)


def main():
    ap = argparse.ArgumentParser(description="Generate the filter tables for src/filter.cpp.")
    ap.add_argument("code_bin", type=Path, help="Pokemon X's code.bin")
    args = ap.parse_args()
    code = args.code_bin.read_bytes()

    lpf_freq = read(code, "<24H", LPF_FREQ)
    if lpf_freq[0] != 80 or lpf_freq[-1] != 16000:
        sys.exit(f"{args.code_bin} isn't Pokemon X's code.bin (kLpfFreq reads {lpf_freq[:3]}...)")

    # Derive the track low-pass pole and fit the SoundFont cutoff.
    low_pass_sf2, low_pass_sfz, sf2_errors = [], [], []
    for freq in lpf_freq[:-1]:
        b0, a1 = one_pole(code, freq)
        cutoff, error = best_cutoff(response_db((b0 / 32768.0, 0.0, 0.0), (-a1 / 32768.0, 0.0), GAME_RATE))
        low_pass_sf2.append(cutoff)
        sf2_errors.append(error)
        low_pass_sfz.append(cents(-GAME_RATE * math.log(a1 / 32768.0) / (2.0 * math.pi) / SFZ_LOW_PASS_BASE))

    print(table("uint16_t", "kLowPassCutoff", "The SoundFont cutoffs, kLpfFreq[0] to kLpfFreq[22].", low_pass_sf2))
    print(table("int16_t", "kSfzLowPassCutoff", "The SFZ cutoffs, kLpfFreq[0] to kLpfFreq[22].", low_pass_sfz))

    # The biquad presets: cutoff (cents from SFZ_BIQUAD_BASE), Q (0.01 dB) and gain (0.01 dB) of each step.
    for kind, (address, sets, name) in BIQUADS.items():
        steps = [decode_biquad(kind, read(code, "<5h", address + 10 * i)) for i in range(sets)]
        values = []
        for freq, q, gain in steps:
            cutoff, resonance = cents(freq / SFZ_BIQUAD_BASE), round(2000.0 * math.log10(q))
            values.append(f"{{{cutoff}, {resonance}, {round(100.0 * gain)}}}")
        print(table("SfzBiquad", f"kSfzBiquad{kind}", f"Type {kind}, {name}: steps 0 to {sets - 1}.", values, 5))
        if kind == 1:
            sf2 = [f"{{{cents(freq / 8.176)}, {round(10.0 * (20.0 * math.log10(q) + 3.01))}}}" for freq, q, _ in steps]
            print(table("SoundFontFilter", "kBiquadLowPass", "The SoundFont filters, steps 0 to 111.", sf2, 6))

    print(f"// RMS difference, SoundFont low-pass: {min(sf2_errors):.2f} to {max(sf2_errors):.2f} dB", file=sys.stderr)


if __name__ == "__main__":
    main()
