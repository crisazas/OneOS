#!/usr/bin/env python3
"""Tone energy per 3 s block of an s16le mono 48 kHz recording made with X400_GATE=1 (3 s tone / 3 s silence).
usage: measure-gated.py file.raw [tone_hz] [skip_s]   -> prints rms and the Goertzel magnitude of each block and the ratio
tone blocks / silent blocks (blocks start at the first playback sample; skip_s drops the stream start)."""
import array, math, sys
f = sys.argv[1]
hz = float(sys.argv[2]) if len(sys.argv) > 2 else 1000.0
skip = float(sys.argv[3]) if len(sys.argv) > 3 else 0.5
R = 48000
a = array.array('h'); a.frombytes(open(f, 'rb').read())
x = [v / 32768.0 for v in a]

def g(b, h):
    w = 2 * math.pi * h / R; c = 2 * math.cos(w); s1 = s2 = 0.0
    for v in b: s1, s2 = v + c * s1 - s2, s1
    return math.sqrt(max(s1 * s1 + s2 * s2 - c * s1 * s2, 0)) * 2 / len(b)

blocks = []
for k in range(len(x) // (3 * R)):
    b = x[k * 3 * R + int(skip * R):(k + 1) * 3 * R - int(0.2 * R)]
    rms = math.sqrt(sum(v * v for v in b) / len(b))
    blocks.append((k, 'TONE' if k % 2 == 0 else 'sil ', rms, g(b, hz), sum(g(b, h) for h in (hz * .7, hz * 1.3, hz * 1.7)) / 3))
for k, kind, rms, t, nb in blocks:
    print('block %d %s rms=%.6f tone=%.7f neighbours=%.7f ratio=%.1f' % (k, kind, rms, t, nb, t / (nb + 1e-12)))
tt = [b[3] for b in blocks if b[1] == 'TONE']; ss = [b[3] for b in blocks if b[1] != 'TONE']
if tt and ss: print('mean tone-block / silent-block magnitude at %.0f Hz = %.2f' % (hz, (sum(tt) / len(tt)) / (sum(ss) / len(ss) + 1e-12)))
