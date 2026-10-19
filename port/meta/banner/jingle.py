#!/usr/bin/env python3
"""Synthesises jingle.wav, the sound the HOME Menu plays once when sh23ds is selected.

An original cue in the spirit of the game, nothing taken from it: the pocket radio's static swells
and cuts out, a heart beats twice (at 0.45 s and 0.75 s, when the mark in the banner pulses), and a
low A minor chord on a soft, slightly detuned electric piano rings out with a distant bell over it.

The format and length are exactly those of 3dsDL's jingle, which works on a console: 44.1 kHz,
16-bit stereo, 1.6 s. The HOME Menu's documented limit is 3 seconds, but bannertool stores the
sound uncompressed, and the first sh23ds banner, 2.9 s long (512 KB of sound, 551 KB of banner
against 3dsDL's 315 KB), crashed the HOME Menu the moment it was selected. main() refuses to
write anything longer.

Usage: python3 jingle.py jingle.wav    (needs numpy; the repo's .venv has it)
"""
import sys
import wave

import numpy as np

RATE = 44100
LENGTH = 1.6
MAX_LENGTH = 1.6   # 3dsDL's jingle; see above
rng = np.random.default_rng(2001)
t = np.arange(int(RATE * LENGTH)) / RATE


def env(start, attack, decay, end=None):
    """0 before start, rising over attack, then falling exponentially with time constant decay."""
    x = t - start
    e = np.where(x < 0, 0.0, np.where(x < attack, x / max(attack, 1e-6), np.exp(-(x - attack) / decay)))
    if end is not None:
        e = e * np.clip((end - t) / 0.02, 0, 1)
    return e


def static():
    """Radio noise: band-limited hiss with crackles, swelling for 0.38 s, then cut."""
    out = []
    for _ in range(2):
        n = rng.standard_normal(t.size)
        spec = np.fft.rfft(n)
        f = np.fft.rfftfreq(t.size, 1 / RATE)
        spec *= np.exp(-((np.log(f + 1) - np.log(2600)) / 0.9) ** 2)    # a radio's narrow band
        hiss = np.fft.irfft(spec, t.size)
        hiss /= np.abs(hiss).max()
        crack = np.zeros(t.size)
        for p in rng.integers(0, int(0.38 * RATE), 35):
            crack[p:p + 40] += rng.choice([-1, 1]) * np.exp(-np.arange(min(40, t.size - p)) / 6)
        flutter = 0.6 + 0.4 * np.sin(2 * np.pi * 9 * t + rng.random() * 6)  # the dial wavering
        swell = np.clip(t / 0.38, 0, 1) ** 1.6 * np.clip((0.4 - t) / 0.02, 0, 1)
        out.append((0.55 * hiss * flutter + 0.35 * crack) * swell)
    return np.stack(out, 1)


def heartbeat():
    """Two low thumps: a sine whose pitch falls, short and heavy."""
    s = np.zeros(t.size)
    for start, level in ((0.45, 1.0), (0.75, 0.75)):
        x = np.clip(t - start, 0, None)
        pitch = 48 + 40 * np.exp(-x / 0.03)
        phase = 2 * np.pi * np.cumsum(pitch) / RATE
        s += level * np.sin(phase) * env(start, 0.004, 0.09)
    return np.stack([s, s], 1)


def piano(freq, start, level, detune):
    """A soft electric piano note: a few sine partials, a slow tremolo, each ear a few cents apart."""
    out = []
    for cents in (-detune, detune):
        f = freq * 2 ** (cents / 1200)
        tone = (np.sin(2 * np.pi * f * t) + 0.35 * np.sin(2 * np.pi * 2 * f * t) * env(start, 0.01, 0.35)
                + 0.12 * np.sin(2 * np.pi * 3 * f * t) * env(start, 0.01, 0.2))
        trem = 1 + 0.12 * np.sin(2 * np.pi * 4.5 * t)
        out.append(level * tone * trem * env(start, 0.03, 0.9))
    return np.stack(out, 1)


def bell(freq, start, level):
    """A small bell far off: inharmonic partials, long ring, a little more in the right ear."""
    s = sum(a * np.sin(2 * np.pi * freq * r * t) * env(start, 0.002, d)
            for r, a, d in ((1.0, 1.0, 1.2), (2.76, 0.4, 0.5), (5.4, 0.2, 0.25)))
    return np.stack([0.8 * level * s, level * s], 1)


def main(path):
    assert LENGTH <= MAX_LENGTH, "the HOME Menu crashed on a longer banner sound"
    mix = 2.4 * static() + 0.75 * heartbeat()
    for freq, level in ((110.0, 0.32), (130.81, 0.24), (164.81, 0.22), (246.94, 0.12)):   # A2 C3 E3 B3
        mix += piano(freq, 0.78, level, 6)
    mix += bell(880.0, 1.0, 0.16)
    mix *= np.clip((LENGTH - t) / 0.3, 0, 1)[:, None]           # fade out before the end
    mix *= 10 ** (-3 / 20) / np.abs(mix).max()                  # peak at -3 dBFS
    data = (mix * 32767).astype("<i2").tobytes()
    with wave.open(path, "wb") as w:
        w.setnchannels(2)
        w.setsampwidth(2)
        w.setframerate(RATE)
        w.writeframes(data)
    print("%s: %.2f s, %d Hz stereo" % (path, LENGTH, RATE))


if __name__ == "__main__":
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    main(sys.argv[1])
