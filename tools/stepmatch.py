#!/usr/bin/env python3
"""Finds which footstep sounds play in an lr3ds --wav capture.

  stepmatch.py <pack/sound/se/se.bnk> <capture.wav> <id> [id ...]

Each SE id's sample (se.bnk, mono PCM16) is resampled to the capture rate and slid over the capture
with a normalised cross-correlation. Both are high-passed first (a double difference), which keeps
the steps' clicks and drops most of the music under them; every peak above 0.5 counts as one play.
Prints, per id, the number of plays and their times in seconds.
"""
import struct
import sys
import wave

import numpy as np


def bank_samples(path):
    d = open(path, 'rb').read()
    magic, ver, n_s, n_ids, n_doors, o_ids, o_doors, o_data = struct.unpack_from('<4s7I', d, 0)
    samples = [struct.unpack_from('<4I', d, 32 + 16 * i) for i in range(n_s)]
    ids = dict(struct.unpack_from('<2I', d, o_ids + 8 * i) for i in range(n_ids))

    def get(game_id):
        off, frames, rate, _ = samples[ids[game_id]]
        pcm = np.frombuffer(d, '<i2', frames, o_data + off).astype(np.float64)
        return pcm, rate
    return get


def ncc(x, t):
    """Normalised cross-correlation of template t along signal x (valid part)."""
    n = len(t)
    t = t - t.mean()
    tn = np.sqrt((t * t).sum()) or 1.0
    size = 1 << int(np.ceil(np.log2(len(x) + n)))
    c = np.fft.irfft(np.fft.rfft(x, size) * np.conj(np.fft.rfft(t, size)), size)[:len(x) - n + 1]
    cs = np.concatenate([[0.0], np.cumsum(x)])
    cs2 = np.concatenate([[0.0], np.cumsum(x * x)])
    s = cs[n:] - cs[:-n]
    s2 = cs2[n:] - cs2[:-n]
    var = np.maximum(s2 - s * s / n, 1e-9)
    return c / (np.sqrt(var) * tn)


def main():
    get = bank_samples(sys.argv[1])
    w = wave.open(sys.argv[2])
    rate = w.getframerate()
    raw = np.frombuffer(w.readframes(w.getnframes()), '<i2').astype(np.float64)
    x = np.diff(raw.reshape(-1, w.getnchannels()).mean(1), 2)
    for a in sys.argv[3:]:
        pcm, r = get(int(a))
        n = int(len(pcm) * rate / r)
        t = np.interp(np.linspace(0, len(pcm) - 1, n), np.arange(len(pcm)), pcm)
        t = np.diff(t[:min(len(t), rate // 6)], 2)   # the attack is enough and keeps it fast
        c = ncc(x, t)
        hits, i = [], 0
        while True:
            j = np.argmax(c[i:] > 0.5) + i if (c[i:] > 0.5).any() else -1
            if j < 0:
                break
            k = j + np.argmax(c[j:j + rate // 10])
            hits.append(k / rate)
            i = k + rate // 5
        print('%s: %d plays %s' % (a, len(hits), ' '.join('%.2f' % h for h in hits)))


if __name__ == '__main__':
    main()
