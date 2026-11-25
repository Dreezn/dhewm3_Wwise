#!/usr/bin/env python3
"""Generate the test tone used by the Wwise test project (wwise/DhewmTest).

The tone is generated here, so it is not Doom 3 audio and not a licensed file.
The .wav is git-ignored; run this script to create it locally.

Usage: python tools/make_test_tone.py
Standard library only.
"""
import math
import pathlib
import struct
import wave

ROOT = pathlib.Path(__file__).resolve().parent.parent
OUT = ROOT / "wwise" / "DhewmTest" / "Sources" / "test_tone.wav"
RATE = 48000
SECONDS = 1.0
FREQ = 440.0


def main():
    OUT.parent.mkdir(parents=True, exist_ok=True)
    frames = bytearray()
    for i in range(int(RATE * SECONDS)):
        # short fade in and out avoids clicks
        env = min(1.0, i / 480.0, (RATE * SECONDS - i) / 480.0)
        sample = int(0.4 * env * 32767 * math.sin(2 * math.pi * FREQ * i / RATE))
        frames += struct.pack("<h", sample)
    with wave.open(str(OUT), "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(RATE)
        w.writeframes(bytes(frames))
    print(f"wrote {OUT.relative_to(ROOT)} ({len(frames)} bytes of PCM)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
