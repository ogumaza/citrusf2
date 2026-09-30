#!/usr/bin/env python3
# SPDX-License-Identifier: MIT

"""Compare citrusf2 output with the same sequences rendered by 3SF.

usage: compare_with_3sf.py [options] --firmware <dspaudio.cdc> <citrusf2 binary> <archive.bcsar>

FluidSynth renders the converted MIDI and SoundFont at gain 1, with reverb and chorus disabled. Use a
single-tempo MIDI copy to remove the player's cumulative rounding at tempo changes (write_one_tempo). With
--sfizz, render each MIDI track through its SFZ file using the library, placing events at their exact
samples.

The game and MIDI files align events to 160-sample sound frames. FluidSynth instead starts notes on
64-sample block boundaries; this alone can change the combined waveform of rapid repeated notes. 3SF uses
archive mode, running its model of the sequence player against the game's DSP firmware.

Align the renders in two-second windows, tracking lag between windows so timing differences do not dominate
sound comparisons. Analyse up to the MIDI duration in 2048-sample frames (62.6 ms). Ignore game frames more
than 60 dB below the loudest frame and clamp quieter values to that floor. Skip renders peaking at 16 or
fewer 16-bit sample steps, and MIDI files shorter than two overlapping analysis frames (94 ms).

Report fields:

- lag: delay of the game render over the first ten seconds, including the one-frame (4.9 ms) difference
  between game playback and MIDI events.
- drift: change in lag from start to end; positive means the game falls behind. Alignment follows loudness,
  so different attacks/releases can shift it by 10-20 ms without actual timing drift. Short sequences have
  fewer windows to average this out.
- gain: overall citrusf2 level relative to the game, including player gain.
- level: mean absolute frame-level difference after removing gain.
- corr: correlation between the two level curves.
- spectrum: mean RMS difference across octave bands after removing gain.
- balance: mean absolute difference in left/right balance.

--timeline reports lag, relative level and octave-band differences each second, useful for finding missing
filters. Both synths render at the DSP rate of 32728 Hz. At this rate FluidSynth's low-pass attenuates the
16k band (11-16 kHz) more than at 44100 Hz, so filtered tracks may look darker here than they sound in a
typical player. --json saves results for comparison between runs.

Requires Python 3, numpy, FluidSynth (or the sfizz library with --sfizz), plus 3sfrip and 3sfplay on PATH or
in the directory supplied to --3sf. Supply the archive and DSP firmware locally; neither citrusf2 nor
3SF distributes these game files.
"""
import argparse
import bisect
import concurrent.futures
import ctypes
import json
import os
import re
import shutil
import struct
import subprocess
import sys
import tempfile
import threading
from pathlib import Path

try:
    import numpy as np
except ImportError:
    sys.exit("compare_with_3sf.py needs numpy (pip install numpy)")

RATE = 32728  # Hz; DSP and 3sfplay output rate
FRAME = 2048  # samples in an analysis frame
HOP = 1024  # samples between analysis frame starts
FLOOR_DB = 60.0  # dB below the loudest game frame
# Skip game renders peaking at 16 sample steps or less (about -66 dBFS); 16-bit rounding dominates this low.
QUIET = 16 / 32768
BLOCK = 32  # samples per block for loudness alignment
# Smooth loudness over about 31 ms. Bass power ripples at twice the note frequency and can shift alignment
# by one or two ripples (15 ms apart at 34 Hz).
SMOOTH = 32
START = 10 * RATE // BLOCK  # blocks in the initial ten-second alignment window
MAX_LAG = 256  # initial lag search limit, about 250 ms in blocks
WINDOW = 2 * RATE // BLOCK  # blocks per two-second alignment window
TRACK = 51  # maximum lag change between windows, about 50 ms in blocks
TIE = 0.002  # correlation tolerance for preferring the peak nearest the previous lag
TAIL = 5.0  # extra 3SF render time to allow for lag beyond the MIDI duration

# Octave bands from 31 Hz to 16 kHz, capped at DSP Nyquist. Start at 24 Hz to exclude the 0 and 16 Hz bins:
# the DSP's inaudible, varying DC offset affects those and cannot be removed by subtracting a constant.
BAND_NAMES = ["31", "63", "125", "250", "500", "1k", "2k", "4k", "8k", "16k"]
BAND_EDGES = [24.0] + [1000.0 * 2 ** (k - 0.5) for k in range(-4, 5)] + [RATE / 2 + 1]


def ecma_escape(text):
    """Escape ECMAScript regex metacharacters for citrusf2 and 3sfrip."""
    return re.sub(r"([\\^$.*+?()\[\]{}|/])", r"\\\1", text)


def safe_name(label):
    """Make a 3sfrip file name from a sound label. Replace every byte except ASCII letters, digits, '_', '-'
    and '.' with '_' (SafeName in 3SF's src/rip/rip.cpp)."""
    return "".join(chr(b) if chr(b).isascii() and (chr(b).isalnum() or chr(b) in "_-.") else "_"
                   for b in label.encode("utf-8"))


def run(args, what):
    """Run a program and return stdout. On failure, raise RuntimeError with `what` and the program's output."""
    p = subprocess.run([str(a) for a in args], stdin=subprocess.DEVNULL, capture_output=True)
    if p.returncode != 0:
        message = (p.stderr or p.stdout).decode("utf-8", "replace").strip().splitlines()
        raise RuntimeError(f"{what} failed ({message[-1] if message else f'exit code {p.returncode}'})")

    return p.stdout.decode("utf-8", "replace")


def read_wav(path):
    """Read WAV samples as floats in -1..1, one column per channel. Accept 3sfplay's 16-bit PCM and
    FluidSynth's 32-bit float output."""
    data = Path(path).read_bytes()
    if data[:4] != b"RIFF" or data[8:12] != b"WAVE":
        raise RuntimeError(f"{path.name} isn't a WAV file")

    fmt = None
    pos = 12
    while pos + 8 <= len(data):
        chunk, size = struct.unpack_from("<4sI", data, pos)
        body = data[pos + 8:pos + 8 + size]
        if chunk == b"fmt ":
            tag, channels, rate = struct.unpack_from("<HHI", body)
            bits = struct.unpack_from("<H", body, 14)[0]
            if tag == 0xFFFE:  # WAVE_FORMAT_EXTENSIBLE, whose sub-format GUID starts with the real tag
                tag = struct.unpack_from("<H", body, 24)[0]
            fmt = (tag, channels, rate, bits)
        elif chunk == b"data" and fmt:
            tag, channels, rate, bits = fmt
            if rate != RATE:
                raise RuntimeError(f"{path.name} is at {rate} Hz, not {RATE} Hz")
            if tag == 1 and bits == 16:
                x = np.frombuffer(body[:len(body) // 2 * 2], "<i2") / 32768.0
            elif tag == 3 and bits == 32:
                x = np.frombuffer(body[:len(body) // 4 * 4], "<f4").astype(np.float64)
            else:
                raise RuntimeError(f"{path.name}: unsupported sample format")
            x = x[:len(x) // channels * channels].reshape(-1, channels)
            return np.repeat(x, 2, axis=1) if channels == 1 else x[:, :2]
        pos += 8 + size + (size & 1)

    raise RuntimeError(f"{path.name} has no samples")


def write_wav(path, x):
    """Write stereo samples to a 32-bit float WAV at RATE."""
    data = x.astype("<f4").tobytes()
    Path(path).write_bytes(b"RIFF" + struct.pack("<I", 36 + len(data)) + b"WAVEfmt " +
                           struct.pack("<IHHIIHH", 16, 3, 2, RATE, RATE * 8, 8, 32) + b"data" +
                           struct.pack("<I", len(data)) + data)


def to_db(power):
    """Convert power to decibels."""
    return 10.0 * np.log10(power + 1e-20)


def vlq(data, pos):
    """Read a MIDI variable-length integer at `pos`; return its value and the next position."""
    value = 0
    while True:
        byte = data[pos]
        pos += 1
        value = value << 7 | byte & 0x7F
        if not byte & 0x80:
            return value, pos


def to_vlq(value):
    """Encode a MIDI variable-length integer."""
    out = bytearray([value & 0x7F])
    while value > 0x7F:
        value >>= 7
        out.insert(0, value & 0x7F | 0x80)
    return bytes(out)


def read_midi(path):
    """Read citrusf2 MIDI into format, division and tracks of (tick, bytes) events."""
    data = Path(path).read_bytes()
    form, count, division = struct.unpack_from(">HHH", data, 8)
    tracks, pos = [], 14
    for _ in range(count):
        size = struct.unpack_from(">I", data, pos + 4)[0]
        p, stop, tick, events = pos + 8, pos + 8 + size, 0, []
        while p < stop:
            delta, p = vlq(data, p)
            tick += delta
            if data[p] == 0xFF:  # meta event (including tempo)
                length, q = vlq(data, p + 2)
                end = q + length
            else:  # citrusf2 writes every channel message with its status byte
                end = p + (2 if data[p] >> 4 in (0xC, 0xD) else 3)
            events.append((tick, data[p:end]))
            p = end
        tracks.append(events)
        pos = stop
    return form, division, tracks


def is_tempo(event):
    """True for a MIDI tempo meta event."""
    return event[:2] == b"\xff\x51"


def tick_seconds(division, tracks):
    """Build a tick-to-seconds function from the tempo events in `tracks`."""
    changes = sorted(((tick, int.from_bytes(e[3:6], "big")) for events in tracks for tick, e in events if is_tempo(e)),
                     key=lambda change: change[0])
    ticks, starts, tempos = [0], [0.0], [500000]  # microseconds per quarter note, MIDI's default
    for tick, tempo in changes:
        starts.append(starts[-1] + (tick - ticks[-1]) * tempos[-1] / division / 1e6)
        ticks.append(tick)
        tempos.append(tempo)

    def seconds(tick):
        i = bisect.bisect_right(ticks, tick) - 1
        return starts[i] + (tick - ticks[i]) * tempos[i] / division / 1e6
    return seconds


def midi_seconds(path):
    """Return MIDI duration in seconds, using the last track end and the file's tempo map."""
    _, division, tracks = read_midi(path)
    return tick_seconds(division, tracks)(max((events[-1][0] for events in tracks if events), default=0))


def write_one_tempo(path, out):
    """Rewrite citrusf2 MIDI at one quarter note per second and one tick per sample, preserving event times to
    the nearest sample.

    FluidSynth rounds its current tick and restarts its clock at each tempo change. This can advance the
    rest of the file by half a tick per change. citrusf2's high resolution limits each error to a fraction
    of a millisecond; removing tempo changes prevents accumulation entirely."""
    form, division, tracks = read_midi(path)
    seconds = tick_seconds(division, tracks)
    data = bytearray(b"MThd" + struct.pack(">IHHH", 6, form, len(tracks), RATE))  # RATE fits the 15 bits MIDI allows
    for i, events in enumerate(tracks):
        body, last = bytearray(), 0
        if i == 0:
            body += b"\x00\xff\x51\x03" + (1000000).to_bytes(3, "big")
        for tick, e in events:
            if not is_tempo(e):
                at = round(seconds(tick) * RATE)
                body += to_vlq(at - last) + e
                last = at
        data += b"MTrk" + struct.pack(">I", len(body)) + body
    Path(out).write_bytes(data)


class Sfizz:
    """Offline SFZ rendering through sfizz's C API (sfizz.h)."""

    BLOCK = 1024  # the most samples rendered at once
    QUALITY = 2  # Hermite interpolation, which sfizz plays with

    def __init__(self, path):
        lib = ctypes.CDLL(str(path))
        synth, fp = ctypes.c_void_p, ctypes.POINTER(ctypes.c_float)
        signatures = {
            "sfizz_create_synth": (synth, []),
            "sfizz_free": (None, [synth]),
            "sfizz_load_file": (ctypes.c_bool, [synth, ctypes.c_char_p]),
            "sfizz_set_sample_rate": (None, [synth, ctypes.c_float]),
            "sfizz_set_samples_per_block": (None, [synth, ctypes.c_int]),
            "sfizz_set_num_voices": (None, [synth, ctypes.c_int]),
            "sfizz_set_sample_quality": (None, [synth, ctypes.c_int, ctypes.c_int]),
            "sfizz_enable_freewheeling": (None, [synth]),
            "sfizz_send_note_on": (None, [synth, ctypes.c_int, ctypes.c_int, ctypes.c_int]),
            "sfizz_send_note_off": (None, [synth, ctypes.c_int, ctypes.c_int, ctypes.c_int]),
            "sfizz_send_cc": (None, [synth, ctypes.c_int, ctypes.c_int, ctypes.c_int]),
            "sfizz_send_program_change": (None, [synth, ctypes.c_int, ctypes.c_int]),
            "sfizz_send_pitch_wheel": (None, [synth, ctypes.c_int, ctypes.c_int]),
            "sfizz_render_block": (None, [synth, ctypes.POINTER(fp), ctypes.c_int, ctypes.c_int]),
        }
        for name, (restype, argtypes) in signatures.items():
            getattr(lib, name).restype = restype
            getattr(lib, name).argtypes = argtypes
        self.lib = lib

    def render(self, sfz, events, frames):
        """Render time-ordered `events` (sample, MIDI message) through `sfz`, returning `frames` stereo
        samples. Start a block at each event so sfizz applies it at the correct sample; controller changes
        otherwise take effect from the block start."""
        lib = self.lib
        synth = lib.sfizz_create_synth()
        try:
            lib.sfizz_set_sample_rate(synth, RATE)
            lib.sfizz_set_samples_per_block(synth, self.BLOCK)
            lib.sfizz_set_num_voices(synth, 256)
            with quiet_output():  # suppress sfizz diagnostics
                lib.sfizz_enable_freewheeling(synth)
                lib.sfizz_set_sample_quality(synth, 1, self.QUALITY)  # 1: freewheeling
                if not lib.sfizz_load_file(synth, str(sfz).encode("utf-8")):
                    raise RuntimeError(f"sfizz can't load {sfz.name}")
            lib.sfizz_set_sample_rate(synth, RATE)  # Set rate after loading: sfizz 1.2.3 initially configures filters for 48 kHz.

            out = np.zeros((frames, 2))
            buffers = [np.zeros(self.BLOCK, np.float32) for _ in range(2)]
            pointers = (ctypes.POINTER(ctypes.c_float) * 2)(
                *[b.ctypes.data_as(ctypes.POINTER(ctypes.c_float)) for b in buffers])
            pos, i = 0, 0
            while pos < frames:
                while i < len(events) and events[i][0] <= pos:
                    self.send(synth, events[i][1])
                    i += 1
                n = min(self.BLOCK, frames - pos, events[i][0] - pos if i < len(events) else self.BLOCK)
                lib.sfizz_render_block(synth, pointers, 2, n)
                out[pos:pos + n, 0], out[pos:pos + n, 1] = buffers[0][:n], buffers[1][:n]
                pos += n
            return out
        finally:
            lib.sfizz_free(synth)

    def send(self, synth, message):
        """Send a MIDI channel message. Ignore the channel: each SFZ file is one instrument."""
        kind, lib = message[0] & 0xF0, self.lib
        if kind == 0x90 and message[2] > 0:
            lib.sfizz_send_note_on(synth, 0, message[1], message[2])
        elif kind in (0x80, 0x90):
            lib.sfizz_send_note_off(synth, 0, message[1], 0)
        elif kind == 0xB0:
            lib.sfizz_send_cc(synth, 0, message[1], message[2])
        elif kind == 0xC0:
            lib.sfizz_send_program_change(synth, 0, message[1])
        elif kind == 0xE0:
            lib.sfizz_send_pitch_wheel(synth, 0, (message[1] | message[2] << 7) - 8192)


# Serialize report printing and temporary stdout redirection.
OUTPUT = threading.Lock()


class quiet_output:
    """Temporarily suppress stdout and stderr, including sfizz's diagnostics."""

    def __enter__(self):
        OUTPUT.acquire()
        sys.stdout.flush()
        sys.stderr.flush()
        self.saved = [os.dup(1), os.dup(2)]
        with open(os.devnull, "wb") as null:
            os.dup2(null.fileno(), 1)
            os.dup2(null.fileno(), 2)

    def __exit__(self, *exc):
        for fd, saved in zip((1, 2), self.saved):
            os.dup2(saved, fd)
            os.close(saved)
        OUTPUT.release()


def render_sfz(sfizz, midi, folder, frames):
    """Render MIDI using SFZ files in `folder`. Route each track and its layers (e.g. "Track 3", "Track 3
    (layer 2)") through the matching "Track 3.sfz". Return the mixed `frames` samples."""
    _, division, tracks = read_midi(midi)
    seconds = tick_seconds(division, tracks)
    mix = np.zeros((frames, 2))
    for events in tracks[1:]:  # the first is the conductor track
        name = next((e for _, e in events if e[:2] == b"\xff\x03"), None)
        messages = [(round(seconds(tick) * RATE), e) for tick, e in events if e[0] != 0xFF]
        if name is None or not messages:
            continue
        length, pos = vlq(name, 2)
        track = name[pos:pos + length].decode("utf-8").split(" (layer")[0]
        mix += sfizz.render(folder / f"{track}.sfz", messages, frames)
    return mix


def loudness(x):
    """Compute block loudness in dB, averaged over SMOOTH neighbouring blocks and clamped to a floor relative
    to the loudest block. Return an empty curve for input shorter than one block."""
    n = len(x) // BLOCK
    if n == 0:
        return np.zeros(0)
    power = (x[:n * BLOCK] ** 2).reshape(n, BLOCK, -1).mean(axis=(1, 2))
    db = to_db(np.convolve(power, np.ones(SMOOTH) / SMOOTH, mode="same"))
    return np.maximum(db, db.max(initial=-200.0) - FLOOR_DB)


def best_lag(a, b, start, stop, lags, last):
    """Find the lag in `lags` that best aligns `b` with `a[start:stop]`, or None if either curve is flat.
    Repeated sounds can produce similar peaks; among peaks within TIE of the best, choose the one nearest
    `last`."""
    stop = min(stop, len(a))
    x = a[start:stop]
    if len(x) < 2 or x.std() == 0:
        return None

    corrs = []
    for lag in lags:
        if start + lag < 0 or stop + lag > len(b):
            continue
        y = b[start + lag:stop + lag]
        if y.std() > 0:
            corrs.append((lag, np.corrcoef(x, y)[0, 1]))
    if not corrs:
        return None

    # Local maxima, including endpoints.
    peaks = [(lag, corr) for i, (lag, corr) in enumerate(corrs)
             if (i == 0 or corr >= corrs[i - 1][1]) and (i == len(corrs) - 1 or corr >= corrs[i + 1][1])]
    best_corr = max(corr for _, corr in peaks)
    return min((lag for lag, corr in peaks if corr >= best_corr - TIE), key=lambda lag: abs(lag - last))


def window_lags(ours, game):
    """Measure the game's lag in blocks over the first ten seconds, then track it per window by searching near
    the preceding lag. A final window shorter than half the normal size inherits the previous lag."""
    a, b = loudness(ours), loudness(game)
    first = best_lag(a, b, MAX_LAG, min(len(a), START), range(-MAX_LAG, MAX_LAG + 1), 0) or 0
    lag, lags = first, []
    for start in range(0, len(a), WINDOW):
        if len(a) - start >= WINDOW // 2:
            found = best_lag(a, b, start, start + WINDOW, range(lag - TRACK, lag + TRACK + 1), lag)
            lag = lag if found is None else found
        lags.append(lag)

    return first, lags


def band_powers(x):
    """Return octave-band power per frame and channel as an array (frames, channels, bands)."""
    frames = (len(x) - FRAME) // HOP + 1
    if frames < 1:
        return np.zeros((0, 2, len(BAND_NAMES)))

    freqs = np.fft.rfftfreq(FRAME, 1.0 / RATE)
    bands = np.stack([(freqs >= lo) & (freqs < hi) for lo, hi in zip(BAND_EDGES, BAND_EDGES[1:])], axis=1)
    window = np.hanning(FRAME)
    views = np.lib.stride_tricks.sliding_window_view(x, FRAME, axis=0)[::HOP]  # (frames, channels, FRAME)
    out = np.empty((frames, x.shape[1], len(BAND_NAMES)))
    for start in range(0, frames, 256):
        spectra = np.abs(np.fft.rfft(views[start:start + 256] * window, axis=2)) ** 2
        out[start:start + 256] = spectra @ bands
    return out


def compare(ours, game, length, timeline):
    """Compare the first `length` samples with the game render. Return report metrics and, if requested, a
    timeline. Set 'skipped' if the game is silent or near 16-bit rounding noise, our render is shorter than
    two frames, or fewer than two game frames exceed the analysis floor."""
    ours = ours[:length]
    if np.abs(game).max(initial=0.0) <= QUIET:
        return {"lag_ms": 0.0, "drift_ms": 0.0, "frames": 0, "skipped": "too quiet"}, []
    if len(ours) < FRAME + HOP:
        return {"lag_ms": 0.0, "drift_ms": 0.0, "frames": 0, "skipped": "too short"}, []

    first, lags = window_lags(ours, game)

    # Shift game frames by each window's lag. Zero-pad both ends to keep all shifted reads in bounds.
    pad = np.zeros((max(abs(lag) for lag in lags) * BLOCK, 2))
    game = np.concatenate([pad, game, pad])
    frames_a, frames_b, times = [], [], []
    for w, lag in enumerate(lags):
        start = w * WINDOW * BLOCK
        x = ours[start:start + WINDOW * BLOCK]
        y = game[len(pad) + start + lag * BLOCK:][:len(x)]
        frames_a.append(band_powers(x[:len(y)]))
        frames_b.append(band_powers(y))
        times.extend((start + np.arange(len(frames_a[-1])) * HOP + FRAME / 2) / RATE)
    a, b = np.concatenate(frames_a), np.concatenate(frames_b)  # (frames, channels, bands)
    times = np.array(times)

    # Combined stereo levels and the mask of frames above the analysis floor.
    level_a, level_b = to_db(a.sum(axis=(1, 2)) / 2), to_db(b.sum(axis=(1, 2)) / 2)
    floor = level_b.max(initial=-200.0) - FLOOR_DB
    active = level_b > floor
    # Estimate drift from median lags at each end. Use at most five windows and at most a third of the total
    # per end, reducing the effect of isolated bad matches.
    lag_ms = [1000.0 * lag * BLOCK / RATE for lag in lags]
    ends = max(1, min(5, len(lags) // 3))
    drift = float(np.median(lag_ms[-ends:]) - np.median(lag_ms[:ends]))
    result = {"lag_ms": 1000.0 * first * BLOCK / RATE, "drift_ms": drift, "frames": int(active.sum())}
    if active.sum() < 2:
        return dict(result, skipped="too quiet"), []

    # Remove overall gain and clamp levels to the analysis floor.
    gain = float(np.median(level_a[active] - level_b[active]))
    level_a, level_b = np.maximum(level_a - gain, floor), np.maximum(level_b, floor)
    difference = level_a - level_b
    bands = np.maximum(to_db(a.sum(axis=1) / 2) - gain, floor) - np.maximum(to_db(b.sum(axis=1) / 2), floor)
    left_a, right_a = (np.maximum(to_db(a[:, c].sum(axis=1)) - gain, floor) for c in (0, 1))
    left_b, right_b = (np.maximum(to_db(b[:, c].sum(axis=1)), floor) for c in (0, 1))
    balance = (left_a - right_a) - (left_b - right_b)

    corr = np.corrcoef(level_a[active], level_b[active])[0, 1] if level_b[active].std() > 0 else float("nan")
    result.update({
        "gain_db": gain,
        "level_db": float(np.abs(difference[active]).mean()),
        "corr": float(corr),
        "spectrum_db": float(np.sqrt((bands[active] ** 2).mean(axis=1)).mean()),
        "balance_db": float(np.abs(balance[active]).mean()),
    })

    # Per-second lag and relative levels/bands, using only frames above the floor.
    seconds = []
    if timeline:
        second_of = times.astype(int)
        for second in range(second_of.max(initial=-1) + 1):
            entry = {"second": second, "lag_ms": lag_ms[min(second * RATE // (WINDOW * BLOCK), len(lags) - 1)]}
            frames = active & (second_of == second)
            if frames.any():
                entry.update({"level_db": float(difference[frames].mean()),
                              "bands_db": [float(v) for v in bands[frames].mean(axis=0)]})
            seconds.append(entry)

    return result, seconds


def compare_sequence(name, index, args, tools, minis, work):
    """Convert, render and compare one sequence. Return its name, metrics and timeline, or its name and a skip
    reason."""
    folder = work / f"{index:05d}"
    try:
        if safe_name(name) not in minis:
            raise RuntimeError("3SF didn't rip it")
        mini = minis[safe_name(name)]
        if not mini:
            raise RuntimeError("another sound's 3SF file has the same name")

        # Isolate each sequence in its own output directory.
        run([args.citrusf2, args.archive, "--quiet", "--output", folder, "--only", f"^{ecma_escape(name)}$"],
            "citrusf2")
        midi, sf2 = next(folder.glob("*.mid")), next(folder.glob("*.sf2"))
        ours_wav, game_wav = folder / "citrusf2.wav", folder / "3sf.wav"
        seconds = midi_seconds(midi)
        if args.sfizz:
            ours = render_sfz(tools["sfizz"], midi, folder / "sfz" / midi.stem, round(seconds * RATE))
            write_wav(ours_wav, ours)
        else:
            one_tempo = folder / "one-tempo.midi"
            write_one_tempo(midi, one_tempo)
            run([tools["fluidsynth"], "-n", "-i", "-q", "-F", ours_wav, "-T", "wav", "-O", "float", "-r", RATE, "-g",
                 1, "-o", "synth.reverb.active=0", "-o", "synth.chorus.active=0", "-o", "synth.polyphony=1024", sf2,
                 one_tempo], "FluidSynth")
            ours = read_wav(ours_wav)
        run([tools["3sfplay"], mini, "-o", game_wav, "--length", f"{seconds + TAIL:.3f}", "--fade", "0"], "3sfplay")

        result, timeline = compare(ours, read_wav(game_wav), round(seconds * RATE), args.timeline)
        result["seconds"] = seconds
        if args.keep:
            for wav, what in ((ours_wav, "citrusf2"), (game_wav, "3sf")):
                shutil.copyfile(wav, args.keep / f"{safe_name(name)}.{what}.wav")
        return name, result, timeline, None
    except (RuntimeError, OSError, StopIteration) as e:
        return name, None, None, str(e) or "citrusf2 made no files"
    finally:
        shutil.rmtree(folder, ignore_errors=True)


def minutes(seconds):
    """Format seconds as minutes:seconds, e.g. 1:05.3."""
    return f"{int(seconds // 60)}:{seconds % 60:04.1f}"


def print_result(name, result, timeline):
    """Print a sequence's metrics and optional timeline."""
    if "skipped" in result:
        print(f"  {name:<28} {minutes(result['seconds']):>7}  {result['skipped']} to compare", flush=True)
        return

    r = result
    print(f"  {name:<28} {minutes(r['seconds']):>7}  lag {r['lag_ms']:4.0f} ms  drift {r['drift_ms']:+4.0f} ms  gain "
          f"{r['gain_db']:+5.1f} dB  level {r['level_db']:4.1f} dB  corr {r['corr']:5.3f}  spectrum "
          f"{r['spectrum_db']:4.1f} dB  balance {r['balance_db']:4.1f} dB", flush=True)
    if timeline:
        print("      time   lag  level " + "".join(f"{b:>6}" for b in BAND_NAMES))
        for s in timeline:
            if "level_db" in s:
                values = f"{s['level_db']:+6.1f}" + "".join(f"{v:+6.1f}" for v in s["bands_db"])
            else:
                values = "     -" + "     -" * len(BAND_NAMES)
            print(f"    {minutes(s['second'])[:-2]:>6}{s['lag_ms']:6.0f}{values}")


def main():
    ap = argparse.ArgumentParser(description="Compare citrusf2 output with 3SF renders of the same sequences.")
    ap.add_argument("citrusf2", type=Path, help="the citrusf2 program")
    ap.add_argument("archive", type=Path, help="the sound archive (.bcsar)")
    ap.add_argument("--firmware", type=Path, required=True, help="a DSP firmware file, such as a game's dspaudio.cdc")
    ap.add_argument("--3sf", dest="threesf", type=Path, help="directory containing 3sfrip and 3sfplay (default: search PATH)")
    ap.add_argument("--only", help="sequence name regex, as accepted by citrusf2 --only")
    ap.add_argument("--timeline", action="store_true", help="also compare each sequence second by second")
    ap.add_argument("--json", type=Path, help="write the results to this file")
    ap.add_argument("--keep", type=Path, help="keep the renders in this folder")
    ap.add_argument("--jobs", type=int, default=os.cpu_count() or 1, help="sequences compared at once")
    ap.add_argument("--sfizz", type=Path, metavar="LIBSFIZZ",
                    help="render SFZ files through this sfizz library instead of using FluidSynth")
    args = ap.parse_args()
    args.citrusf2 = args.citrusf2.resolve()

    tools = {} if args.sfizz else {"fluidsynth": shutil.which("fluidsynth")}
    for tool in ("3sfrip", "3sfplay"):
        tools[tool] = shutil.which(tool, path=str(args.threesf) if args.threesf else None)
    missing = [tool for tool, path in tools.items() if not path]
    if missing:
        sys.exit(f"can't find {' or '.join(missing)}")
    if args.sfizz:
        try:
            tools["sfizz"] = Sfizz(args.sfizz)
        except OSError as e:
            sys.exit(f"can't load {args.sfizz}: {e}")
    if args.keep:
        args.keep.mkdir(parents=True, exist_ok=True)

    only = ["--only", args.only] if args.only else []
    names = run([args.citrusf2, args.archive, "--list", *only], "citrusf2 --list").splitlines()
    results, failed = {}, 0
    with tempfile.TemporaryDirectory() as tmp:
        work = Path(tmp)

        # Rip a .3sflib and "<sound index> <label>.mini3sf" per sequence. Get durations from MIDI instead of 3sfrip.
        rip = work / "3sf"
        run([tools["3sfrip"], args.archive, "--mode", "archive", "--firmware", args.firmware, "--no-length",
             "--output", rip, *only], "3sfrip")
        minis = {}
        for f in rip.glob("*.mini3sf"):
            label = f.stem.split(" ", 1)[1]
            minis[label] = None if label in minis else f  # ambiguous file name shared by two sounds

        with concurrent.futures.ThreadPoolExecutor(max(1, args.jobs)) as pool:
            jobs = [pool.submit(compare_sequence, name, i, args, tools, minis, work) for i, name in enumerate(names)]
            for job in jobs:
                name, result, timeline, error = job.result()
                with OUTPUT:
                    if error:
                        failed += 1
                        print(f"  {name}: {error}", flush=True)
                        continue
                    print_result(name, result, timeline)
                results[name] = dict(result, timeline=timeline) if timeline else result

    if args.json:
        args.json.write_text(json.dumps({"archive": args.archive.name, "sequences": results}, indent=1) + "\n",
                             encoding="utf-8")
    print(f"{len(results)} compared, {failed} failed")

    return 1 if failed else 0


if __name__ == "__main__":
    raise SystemExit(main())
