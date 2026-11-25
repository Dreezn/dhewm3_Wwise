#!/usr/bin/env python3
"""Check whether a recorded .wav contains sound.

Used to "listen" to the game's audio output without a human: the game writes
its Wwise output to a file (cvar s_wwiseCapture), and this script reports
peak level and RMS, and whether the level is above the silence threshold.

Usage: python tools/analyze_wav.py <file.wav> [--threshold 0.001]
Exit code 0 if sound is present, 1 if silent or unreadable.
Standard library only.
"""
import array
import math
import sys
import wave


def main(argv):
    if not argv:
        print(__doc__)
        return 1
    path = argv[0]
    threshold = 0.001
    if "--threshold" in argv:
        threshold = float(argv[argv.index("--threshold") + 1])
    try:
        with wave.open(path, "rb") as w:
            channels = w.getnchannels()
            width = w.getsampwidth()
            rate = w.getframerate()
            frames = w.getnframes()
            raw = w.readframes(frames)
    except (wave.Error, OSError) as e:
        print(f"unreadable: {path}: {e}")
        return 1
    if width != 2:
        print(f"unsupported sample width {width} bytes (expected 16-bit)")
        return 1
    samples = array.array("h")
    samples.frombytes(raw)
    if not samples:
        print(f"{path}: empty (0 samples)")
        return 1
    peak = max(abs(s) for s in samples) / 32768.0
    rms = math.sqrt(sum((s / 32768.0) ** 2 for s in samples) / len(samples))
    duration = frames / float(rate) if rate else 0.0
    loud = peak > threshold
    print(f"{path}: {channels} ch, {rate} Hz, {duration:.2f} s, peak {peak:.4f}, rms {rms:.4f}")
    print("SOUND PRESENT" if loud else "SILENT")
    return 0 if loud else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
