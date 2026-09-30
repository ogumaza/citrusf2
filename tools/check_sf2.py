#!/usr/bin/env python3
# SPDX-License-Identifier: MIT

"""Validate SoundFont 2 structure: RIFF chunks, record counts and indexes, generator order, sample bounds,
stereo links and loop padding (8 samples before and after, at least 32 within the loop).

usage: check_sf2.py <file.sf2>...
"""
import argparse
import struct
import sys
from pathlib import Path


def chunks(data, off, end):
    while off < end:
        cid, size = struct.unpack_from("<4sI", data, off)
        yield cid.decode("latin1"), off + 8, size
        off += 8 + size + (size & 1)


def check(path):
    errors = []
    data = Path(path).read_bytes()
    rid, rsize, form = struct.unpack_from("<4sI4s", data, 0)
    if rid != b"RIFF" or form != b"sfbk" or rsize + 8 != len(data):
        return [f"bad RIFF header ({rid}, {rsize + 8} vs {len(data)} bytes, {form})"]
    lists = {}
    for cid, off, size in chunks(data, 12, len(data)):
        if cid != "LIST":
            errors.append(f"unexpected top-level chunk {cid}")
            continue
        lists[data[off:off + 4].decode()] = {c: (o, s) for c, o, s in chunks(data, off + 4, off + size)}
    for name in ("INFO", "sdta", "pdta"):
        if name not in lists:
            return errors + [f"missing LIST {name}"]
    if "ifil" not in lists["INFO"] or "isng" not in lists["INFO"] or "INAM" not in lists["INFO"]:
        errors.append("INFO lacks ifil, isng or INAM")
    smpl_off, smpl_size = lists["sdta"]["smpl"]
    nsamples_data = smpl_size // 2
    p = lists["pdta"]
    sizes = {"phdr": 38, "pbag": 4, "pmod": 10, "pgen": 4, "inst": 22, "ibag": 4, "imod": 10, "igen": 4, "shdr": 46}
    rec = {}
    for k, sz in sizes.items():
        if k not in p:
            return errors + [f"missing {k}"]
        o, s = p[k]
        if s % sz:
            errors.append(f"{k} size {s} isn't a multiple of {sz}")
        rec[k] = [data[o + i * sz:o + (i + 1) * sz] for i in range(s // sz)]
    phdr = [struct.unpack("<20sHHHIII", r) for r in rec["phdr"]]
    inst = [struct.unpack("<20sH", r) for r in rec["inst"]]
    pbag = [struct.unpack("<HH", r) for r in rec["pbag"]]
    ibag = [struct.unpack("<HH", r) for r in rec["ibag"]]
    pgen = [struct.unpack("<HH", r) for r in rec["pgen"]]
    igen = [struct.unpack("<HH", r) for r in rec["igen"]]
    shdr = [struct.unpack("<20sIIIIIBbHH", r) for r in rec["shdr"]]
    terminals = (phdr[-1][0], inst[-1][0], shdr[-1][0])
    if tuple(name.rstrip(b"\0") for name in terminals) != (b"EOP", b"EOI", b"EOS"):
        errors.append("missing terminal records")
    if pbag[-1][0] != len(pgen) - 1 or ibag[-1][0] != len(igen) - 1:
        errors.append("terminal bag doesn't point at the terminal generator")
    if pbag[-1][1] != len(rec["pmod"]) - 1 or ibag[-1][1] != len(rec["imod"]) - 1:
        errors.append("terminal bag doesn't point at the terminal modulator")
    nsamp = len(shdr) - 1
    ninst = len(inst) - 1

    def zones(headers, bags, gens, bag_index, terminal_oper, limit, what):
        for i in range(len(headers) - 1):
            b0, b1 = headers[i][bag_index], headers[i + 1][bag_index]
            if b1 < b0:
                errors.append(f"{what} {i}: bag indexes go backwards")
                continue
            for b in range(b0, b1):
                g0, g1 = bags[b][0], bags[b + 1][0]
                opers = [gens[g][0] for g in range(g0, g1)]
                if 43 in opers and opers.index(43) != 0:
                    errors.append(f"{what} {i} zone {b - b0}: keyRange isn't first")
                if 44 in opers and opers.index(44) > (1 if 43 in opers else 0):
                    errors.append(f"{what} {i} zone {b - b0}: velRange out of place")
                if terminal_oper in opers:
                    if opers.index(terminal_oper) != len(opers) - 1:
                        errors.append(f"{what} {i} zone {b - b0}: generator {terminal_oper} isn't last")
                    target = gens[g0 + opers.index(terminal_oper)][1]
                    if target >= limit:
                        errors.append(f"{what} {i} zone {b - b0}: index {target} out of range")
                elif b != b0:
                    errors.append(f"{what} {i} zone {b - b0}: missing {terminal_oper} in a non-global zone")

    zones(phdr, pbag, pgen, 3, 41, ninst, "preset")
    zones(inst, ibag, igen, 1, 53, nsamp, "instrument")
    presets = set()
    for h in phdr[:-1]:
        key = (h[2], h[1])
        if key in presets:
            errors.append(f"duplicate preset bank {h[2]} program {h[1]}")
        presets.add(key)
    for i, s in enumerate(shdr[:-1]):
        name, start, end, ls, le, rate, key, corr, link, typ = s
        if not (start < end <= nsamples_data):
            errors.append(f"sample {i}: bounds {start}-{end} outside {nsamples_data}")
        if end + 46 > nsamples_data or any(struct.unpack_from("<46h", data, smpl_off + end * 2)):
            errors.append(f"sample {i}: not followed by 46 zero samples")
        if le > ls:
            if not (start <= ls and le <= end):
                errors.append(f"sample {i}: loop {ls}-{le} outside {start}-{end}")
        if typ in (2, 4):
            if link >= nsamp or shdr[link][9] != (6 - typ) or shdr[link][8] != i:
                errors.append(f"sample {i}: stereo link broken")
        elif typ != 1:
            errors.append(f"sample {i}: type {typ}")
        if rate < 400 or rate > 200000:
            errors.append(f"sample {i}: rate {rate}")
    # Check loop padding for looping zones (sampleModes 1).
    for i in range(len(inst) - 1):
        for b in range(inst[i][1], inst[i + 1][1]):
            gl = dict(igen[g] for g in range(ibag[b][0], ibag[b + 1][0]))
            if gl.get(54, 0) & 1 and 53 in gl:
                name, start, end, ls, le = shdr[gl[53]][:5]
                if ls - start < 8 or end - le < 8 or le - ls < 32:
                    errors.append(f"instrument {i}: looping sample {gl[53]} breaks the 8/32/8 rule")
    return errors


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("files", nargs="+")
    a = ap.parse_args()
    bad = 0
    for path in a.files:
        try:
            errs = check(path)
        except OSError as e:
            errs = [f"can't read it ({e.strerror})"]
        except (IndexError, KeyError, struct.error) as e:
            # Catch out-of-bounds indexes or sizes missed by the structural checks.
            errs = [f"broken structure ({type(e).__name__}: {e})"]
        if errs:
            bad += 1
            print(path)
            for e in errs[:10]:
                print("  " + e)
    print(f"{len(a.files)} files, {bad} with problems")
    sys.exit(1 if bad else 0)


if __name__ == "__main__":
    main()
