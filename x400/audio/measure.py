#!/usr/bin/env python3
"""RMS and tone energy of an s16le mono 48 kHz raw file: measure.py file.raw [tone_hz] [skip_s]"""
import array, math, sys
f = sys.argv[1]
hz = float(sys.argv[2]) if len(sys.argv) > 2 else 1000.0
skip = float(sys.argv[3]) if len(sys.argv) > 3 else 0.5
a = array.array('h'); a.frombytes(open(f, 'rb').read())
x = [v / 32768.0 for v in a][int(skip * 48000):]
n = len(x)
rms = math.sqrt(sum(v * v for v in x) / n)

def goertzel(x, hz, rate=48000):
    w = 2 * math.pi * hz / rate
    c = 2 * math.cos(w)
    s1 = s2 = 0.0
    for v in x:
        s1, s2 = v + c * s1 - s2, s1
    return math.sqrt(max(s1 * s1 + s2 * s2 - c * s1 * s2, 0)) * 2 / n

tone = goertzel(x, hz)
ref = [goertzel(x, h) for h in (hz * 0.7, hz * 1.3, hz * 1.7, hz * 2.3)]
print("%s: frames=%d rms=%.5f tone@%.0fHz=%.7f neighbours(avg)=%.7f ratio=%.1f" %
      (f, n, rms, hz, tone, sum(ref) / len(ref), tone / (sum(ref) / len(ref) + 1e-12)))
