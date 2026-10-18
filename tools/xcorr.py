"""xcorr.py capture.wav id[,id...] : find each SE (game id) in an lr3ds capture.
Reports every place the SE's source PCM (from se.bnk, resampled to the capture rate) matches the
capture with normalised correlation above 0.5, per channel (L+R mixed)."""
import sys, struct, wave
import numpy as np

def fftconvolve(a, b, mode='valid'):
    n = len(a) + len(b) - 1
    m = 1 << (n - 1).bit_length()
    full = np.fft.irfft(np.fft.rfft(a, m) * np.fft.rfft(b, m), m)[:n]
    return full[len(b) - 1:len(a)]

def resample(s, rate, R):
    t = np.arange(0, len(s) - 1, rate / R)
    return np.interp(t, np.arange(len(s)), s)

BANK = '/Users/weeblet/Developer/sh23ds/pack/sound/se/se.bnk'
b = open(BANK, 'rb').read()
_, _, ns, ni, nd, io, do, da = struct.unpack_from('<4s7I', b, 0)
ids = dict(struct.unpack_from('<2I', b, io + 8 * k) for k in range(ni))

def sample(gid):
    o, fr, rate, fl = struct.unpack_from('<4I', b, 32 + 16 * ids[gid])
    return rate, np.frombuffer(b, '<i2', fr, da + o).astype(float)

w = wave.open(sys.argv[1]); R = w.getframerate()
x = np.frombuffer(w.readframes(w.getnframes()), '<i2').reshape(-1, 2).astype(float)
mono = x.mean(1)
for gid in [int(v) for v in sys.argv[2].split(',')]:
    rate, s = sample(gid)
    t = resample(s, rate, R)
    if len(sys.argv) > 3: t = t[:int(float(sys.argv[3]) * R)]
    num = fftconvolve(mono, t[::-1], 'valid')
    e = np.sqrt(fftconvolve(mono ** 2, np.ones(len(t)), 'valid').clip(1e-9) * (t ** 2).sum())
    c = num / e
    hits = []
    order = np.argsort(-c)
    for i in order:
        if c[i] < 0.5 or len(hits) >= 40:
            break
        if all(abs(i - j) > len(t) // 2 for j, _ in hits):
            hits.append((i, c[i]))
    best = c.argmax()
    seg = x[best:best + len(t)]
    gl = np.sqrt((seg[:, 0] ** 2).mean()) / max(np.sqrt((t ** 2).mean()), 1)
    gr = np.sqrt((seg[:, 1] ** 2).mean()) / max(np.sqrt((t ** 2).mean()), 1)
    print('id %d (%d Hz, %.2f s): best corr %.4f at %.3f s, gain L %.3f R %.3f; hits %s' % (
        gid, rate, len(s) / rate, c[best], best / R, gl, gr,
        ', '.join('%.2fs:%.3f/g%.2f' % (i / R, v, np.sqrt((mono[i:i+len(t)]**2).mean())/np.sqrt((t**2).mean())) for i, v in sorted(hits))))
