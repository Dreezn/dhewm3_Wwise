#!/usr/bin/env python3
"""Level over time of a recorded .wav (the Wwise capture), for tests without listening.

Prints one line per window: start time (ms), left and right RMS in dBFS, peak, and the
zero-crossing rate (a rough pitch measure: half the pitch gives about half the rate).
Also prints the number of clipped samples (at full scale) for the whole file.

Usage: python3 tools/analyze_wav_windows.py <file.wav> [--window 250] [--from ms] [--to ms]
Standard library only, 16-bit PCM.
"""
import argparse
import array
import math
import wave


def db(x):
    return 20 * math.log10(x) if x > 1e-9 else -180.0


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("wav")
    ap.add_argument("--window", type=int, default=250, help="window length in ms")
    ap.add_argument("--from", dest="start", type=int, default=0)
    ap.add_argument("--to", dest="end", type=int, default=None)
    args = ap.parse_args()

    with wave.open(args.wav, "rb") as w:
        ch, width, rate, n = w.getnchannels(), w.getsampwidth(), w.getframerate(), w.getnframes()
        raw = w.readframes(n)
    if width != 2:
        raise SystemExit("16-bit PCM only")
    s = array.array("h")
    s.frombytes(raw)
    clipped = sum(1 for v in s if v >= 32767 or v <= -32768)
    print(f"{args.wav}: {ch} ch, {rate} Hz, {n / rate:.2f} s, clipped samples: {clipped}")
    print("    ms   L dBFS   R dBFS    peak   zc/s")
    per = rate * args.window // 1000
    end = n if args.end is None else min(n, args.end * rate // 1000)
    for f0 in range(args.start * rate // 1000, end, per):
        f1 = min(end, f0 + per)
        sums = [0.0] * ch
        peak = 0
        zc = 0
        prev = 0
        for f in range(f0, f1):
            for c in range(ch):
                v = s[f * ch + c]
                sums[c] += v * v
                if abs(v) > peak:
                    peak = abs(v)
            v0 = s[f * ch]
            if (v0 >= 0) != (prev >= 0):
                zc += 1
            prev = v0
        cnt = max(1, f1 - f0)
        rms = [math.sqrt(x / cnt) / 32768 for x in sums]
        right = rms[1] if ch > 1 else rms[0]
        print(f"{f0 * 1000 // rate:6d}  {db(rms[0]):7.1f}  {db(right):7.1f}  {peak / 32768:6.3f}  {zc * rate // cnt:5d}")


if __name__ == "__main__":
    main()
