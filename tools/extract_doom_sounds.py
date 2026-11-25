#!/usr/bin/env python3
"""
Extracts the original Doom 3 weapon-fire samples from the user's own game data,
for the local Wwise test project (Phase 4, PROJECT_NOTES.md section 8.4, D-052).

Licensing: the samples are ID Software content. They are written to deps/doom3-sounds/
(git-ignored) and are never committed. The .pk4 files are only read.

What it does:
  1. Reads every def/weapon_*.def in the .pk4 files and collects the sound shaders
     named by animation frame commands ("frame N sound_weapon player_pistol_fire")
     whose name contains "fire". These are the weapon-fire shaders.
  2. Reads the sound shader declarations (sound/*.sndshd) for those shaders:
     sample files, minDistance, maxDistance, volume and flags.
  3. Extracts each sample. Doom 3 falls back to .ogg when a .wav is missing; Wwise only
     imports .wav, so .ogg files are converted with ffmpeg when it is on PATH (skipped otherwise).
  4. Writes a manifest (JSON) used by tools/wwise_weapon_fire.js.

Usage:
  py tools/extract_doom_sounds.py [--doom-base <Doom 3 base folder>] [--out deps/doom3-sounds]
"""

import argparse
import glob
import json
import os
import re
import shutil
import subprocess
import sys
import zipfile

DEFAULT_BASE = r"C:\Program Files (x86)\Steam\steamapps\common\Doom 3\base"
REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
FLAG_WORDS = {"looping", "no_dups", "play_once", "no_occlusion", "global", "private",
              "antiprivate", "unclamped", "omnidirectional", "no_flicker", "no_efx"}


def event_name(shader):
    """Same rule as the game (snd_wwise_emitter.cpp): Play_ + shader, non [A-Za-z0-9_] -> _."""
    return "Play_" + re.sub(r"[^A-Za-z0-9_]", "_", shader)


def index_pk4(base):
    """Maps lower-case file names to (pk4, name). Later .pk4 files override earlier ones, as in the game."""
    files = {}
    paks = sorted(glob.glob(os.path.join(base, "pak*.pk4")))
    if not paks:
        sys.exit(f"no pak*.pk4 files in {base}")
    for pk in paks:
        with zipfile.ZipFile(pk) as z:
            for n in z.namelist():
                files[n.lower()] = (pk, n)
    return files


def read_bytes(files, name):
    pk, real = files[name.lower()]
    with zipfile.ZipFile(pk) as z:
        return z.read(real)


def weapon_fire_shaders(files):
    shaders = set()
    for n in files:
        if re.match(r"def/weapon_\w+\.def$", n):
            text = read_bytes(files, n).decode("latin-1")
            for _cmd, shader in re.findall(r"frame\s+\d+\s+(sound\w*)\s+([\w/.]+)", text):
                if "fire" in shader.lower():
                    shaders.add(shader)
    return sorted(shaders)


def shader_bodies(files):
    bodies = {}
    for n in files:
        if n.startswith("sound/") and n.endswith(".sndshd"):
            text = re.sub(r"//[^\n]*", "", read_bytes(files, n).decode("latin-1"))
            text = re.sub(r"/\*.*?\*/", "", text, flags=re.S)
            for m in re.finditer(r"([\w/]+)\s*\{([^{}]*)\}", text):
                bodies[m.group(1).lower()] = m.group(2)
    return bodies


def parse_shader(body):
    tokens = body.split()
    info = {"samples": [], "flags": [], "minDistance": None, "maxDistance": None, "volume": None}
    i = 0
    while i < len(tokens):
        t = tokens[i]
        low = t.lower()
        if low in ("mindistance", "maxdistance", "volume") and i + 1 < len(tokens):
            key = {"mindistance": "minDistance", "maxdistance": "maxDistance", "volume": "volume"}[low]
            info[key] = float(tokens[i + 1])
            i += 2
            continue
        if low in FLAG_WORDS:
            info["flags"].append(low)
        elif re.search(r"\.(wav|ogg)$", low):
            info["samples"].append(t.replace("\\", "/"))
        i += 1
    return info


def extract_sample(files, sample, out_dir, ffmpeg):
    """Returns the absolute path of the extracted .wav, or None (with a reason)."""
    target = os.path.join(out_dir, os.path.splitext(sample)[0] + ".wav")
    os.makedirs(os.path.dirname(target), exist_ok=True)
    wav = os.path.splitext(sample)[0] + ".wav"
    ogg = os.path.splitext(sample)[0] + ".ogg"
    if wav.lower() in files:
        with open(target, "wb") as f:
            f.write(read_bytes(files, wav))
        return target, None
    if ogg.lower() in files:
        if not ffmpeg:
            return None, "only .ogg in the game data and ffmpeg is not on PATH"
        ogg_path = os.path.splitext(target)[0] + ".ogg"
        with open(ogg_path, "wb") as f:
            f.write(read_bytes(files, ogg))
        r = subprocess.run([ffmpeg, "-y", "-loglevel", "error", "-i", ogg_path, target], capture_output=True, text=True)
        os.remove(ogg_path)
        if r.returncode != 0:
            return None, "ffmpeg failed: " + r.stderr.strip()[:200]
        return target, None
    return None, "not found in the game data"


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--doom-base", default=DEFAULT_BASE)
    ap.add_argument("--out", default=os.path.join(REPO, "deps", "doom3-sounds"))
    args = ap.parse_args()

    out_dir = os.path.abspath(args.out)
    ffmpeg = shutil.which("ffmpeg")
    files = index_pk4(args.doom_base)
    bodies = shader_bodies(files)
    manifest = []
    problems = 0
    for shader in weapon_fire_shaders(files):
        body = bodies.get(shader.lower())
        if body is None:
            print(f"SKIP {shader}: no shader declaration")
            problems += 1
            continue
        info = parse_shader(body)
        samples = []
        for s in info["samples"]:
            path, why = extract_sample(files, s, out_dir, ffmpeg)
            if path:
                samples.append(path)
            else:
                print(f"  {shader}: {s}: {why}")
                problems += 1
        if not samples:
            print(f"SKIP {shader}: no usable samples")
            continue
        manifest.append({"shader": shader, "event": event_name(shader), "samples": samples,
                         "minDistance": info["minDistance"], "maxDistance": info["maxDistance"],
                         "volume": info["volume"], "flags": info["flags"]})
        print(f"{shader}: {len(samples)} sample(s) -> {event_name(shader)}")

    os.makedirs(out_dir, exist_ok=True)
    manifest_path = os.path.join(out_dir, "weapon_fire.json")
    with open(manifest_path, "w", encoding="utf-8") as f:
        json.dump(manifest, f, indent=1)
    print(f"manifest: {manifest_path} ({len(manifest)} shaders, {problems} problem(s))")


if __name__ == "__main__":
    main()
