#!/usr/bin/env python3
"""
Extracts the Doom 3 sound shaders and their samples from the user's own game data,
for the local Wwise test project (Phase 5, PROJECT_NOTES.md section 8.5, D-052 to D-055).

Licensing: the samples are ID Software content. They are written to deps/doom3-sounds/
(git-ignored) and are never committed. The .pk4 files are only read.

What it does:
  1. Reads every sound/*.sndshd file, in the order the game reads them (sorted by name;
     a file in a later .pk4 replaces the same file in an earlier one). The parser follows
     idSoundShader::ParseShader (neo/sound/snd_shader.cpp). As in idDeclManager, the
     first definition of a shader name wins and later ones are skipped.
  2. Finds implicit shaders: sound names used by defs, maps and scripts that are not
     declared in any .sndshd file but name a sample file. The game turns such a name into
     a one-sample shader (idSoundShader::SetDefaultText).
  3. Extracts each sample to deps/doom3-sounds/<same path>.wav. Doom 3 falls back to .ogg
     when a .wav is missing; Wwise imports .wav only, so .ogg files are converted with
     ffmpeg when it is on PATH (reported and skipped otherwise).
  4. Writes the manifest deps/doom3-sounds/doom_sounds.json for tools/wwise_doom_sounds.js:
     one group per .sndshd file (one Wwise bank each, D-055), plus the group "implicit".

Usage:
  python tools/extract_doom_sounds.py [--doom-base <Doom 3 base folder>] [--out deps/doom3-sounds] [--no-audio]
  --no-audio writes the manifest only (no sample extraction).
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
SOUND_MAX_LIST_WAVS = 32            # neo/sound/sound.h

# Keywords of idSoundShader::ParseShader and how many arguments each one reads.
KEYWORD_ARGS = {
    "minsamples": 1, "description": 1, "mindistance": 1, "maxdistance": 1, "reverb": 2,
    "volume": 1, "leadinvolume": 1, "soundclass": 1, "altsound": 1,
    "mask_center": 0, "mask_left": 0, "mask_right": 0, "mask_backright": 0, "mask_backleft": 0, "mask_lfe": 0,
    "ordered": 0, "plain": 0, "ondemand": 0,
}
FLAGS = {
    "no_dups": "no_dups", "no_flicker": "no_flicker", "looping": "looping", "no_occlusion": "no_occlusion",
    "private": "private", "antiprivate": "antiprivate", "playonce": "playonce", "global": "global",
    "unclamped": "unclamped", "omnidirectional": "omnidirectional",
}


def event_name(shader):
    """Same rule as the game (idSoundEmitterWwise::EventNameForShader): Play_ + shader, non [A-Za-z0-9_] -> _."""
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


def read_text(files, name):
    return read_bytes(files, name).decode("latin-1")


def tokens(text):
    """idLexer-like tokens: quoted strings (without quotes), braces, and other words. Comments removed."""
    out = []
    for m in re.finditer(r'"([^"]*)"|//[^\n]*|/\*.*?\*/|([{}])|([^\s{}"]+)', text, re.S):
        if m.group(1) is not None:
            out.append(m.group(1))
        elif m.group(2) is not None:
            out.append(m.group(2))
        elif m.group(3) is not None:
            out.append(m.group(3))
    return out


def is_number(tok):
    return re.fullmatch(r"-?\d+(\.\d*)?|-?\.\d+", tok) is not None


def is_sample(tok):
    low = tok.lower()
    return ".wav" in low or ".ogg" in low


def parse_shader(body):
    """Shader body tokens -> values, following idSoundShader::ParseShader (defaults included)."""
    info = {"samples": [], "leadins": [], "minDistance": 1.0, "maxDistance": 10.0, "volume": 1.0,
            "leadinVolume": None, "shakes": 0.0, "soundClass": 0, "flags": [], "altSound": None}
    i = 0
    while i < len(body):
        t = body[i]
        low = t.lower()
        if low == "shakes":
            # optional value: "shakes" alone means 1.0
            if i + 1 < len(body) and is_number(body[i + 1]):
                info["shakes"] = float(body[i + 1])
                i += 2
            else:
                info["shakes"] = 1.0
                i += 1
            continue
        if low == "leadin":
            if i + 1 < len(body) and len(info["leadins"]) < SOUND_MAX_LIST_WAVS:
                info["leadins"].append(body[i + 1].replace("\\", "/"))
            i += 2
            continue
        if low in KEYWORD_ARGS:
            n = KEYWORD_ARGS[low]
            arg = body[i + 1] if n and i + 1 < len(body) else None
            if low == "mindistance":
                info["minDistance"] = float(arg)
            elif low == "maxdistance":
                info["maxDistance"] = float(arg)
            elif low == "volume":
                info["volume"] = float(arg)
            elif low == "leadinvolume":
                info["leadinVolume"] = float(arg)
            elif low == "soundclass":
                info["soundClass"] = int(float(arg))
            elif low == "altsound":
                info["altSound"] = arg
            i += 1 + n
            continue
        if low in FLAGS:
            if FLAGS[low] not in info["flags"]:
                info["flags"].append(FLAGS[low])
        elif is_sample(t):
            if len(info["samples"]) < SOUND_MAX_LIST_WAVS:
                info["samples"].append(t.replace("\\", "/"))
        i += 1
    return info


def declared_shaders(files, duplicates):
    """[(file, name, body tokens)] in game order; the first definition of a name wins."""
    seen = {}
    order = []
    for n in sorted(f for f in files if f.startswith("sound/") and f.endswith(".sndshd")):
        toks = tokens(read_text(files, n))
        i = 0
        while i < len(toks):
            name = toks[i]
            if name.lower() == "sound" and i + 1 < len(toks) and toks[i + 1] != "{":
                i += 1
                name = toks[i]
            if name == "{" or i + 1 >= len(toks) or toks[i + 1] != "{":
                i += 1
                continue
            depth, j = 0, i + 1
            while j < len(toks):
                if toks[j] == "{":
                    depth += 1
                elif toks[j] == "}":
                    depth -= 1
                    if depth == 0:
                        break
                j += 1
            body = toks[i + 2:j]
            key = name.lower()
            if key in seen:
                duplicates.append(f"duplicate {name} in {n} (first defined in {seen[key]}): skipped, as in the game")
            else:
                seen[key] = n
                order.append((n, name, body))
            i = j + 1
    return order


def implicit_shaders(files, declared):
    """Sound names used in defs, maps and scripts that are sample files and not declared shaders."""
    names = set()
    for n in files:
        if n.endswith((".def", ".map", ".script")):
            text = read_text(files, n)
            for key, value in re.findall(r'"(snd_\w+|s_shader)"\s+"([^"]+)"', text):
                names.add(value)
            for value in re.findall(r'startSound(?:Shader)?\s*\(\s*"([^"]+)"', text, re.I):
                names.add(value)
    result, seen = [], set()
    for name in sorted(names, key=str.lower):
        # The game names the shader as idDeclManagerLocal::MakeNameCanonical does: lower case,
        # "/" for "\", extension removed. Its sample is that name + ".wav" (idSoundShader::SetDefaultText;
        # the sound loader falls back to .ogg). A name with or without ".wav" is the same shader.
        key = canonical_name(name)
        if key in declared or key in seen or sample_source(files, key + ".wav") is None:
            continue
        seen.add(key)
        result.append(key)
    return result


def speaker_loops(files):
    """Shaders that an entity loops through s_looping (idEntity::ParseSpawnArgsToRefSound sets
    SSF_LOOPING), per D-068: canonical name -> "only" when nothing plays it without loop, "both" when
    something does: an entity with s_shader and no s_looping, a snd_* key, a script startSound, or a
    sound frame command in a model def."""
    looped, plain = set(), set()
    for n in files:
        if n.endswith(".map"):
            blocks = [b.split("// primitive")[0] for b in re.split(r"// entity \d+", read_text(files, n))[1:]]
        elif n.endswith(".def"):
            text = read_text(files, n)
            blocks = [m.group(1) for m in re.finditer(r'entityDef\s+\S+\s*\{(.*?)\n\}', text, re.S)]
            for m in re.finditer(r'\bframe\s+\d+\s+sound\w*\s+"?([^"\s]+)', text):
                plain.add(canonical_name(m.group(1)))
        elif n.endswith(".script"):
            for value in re.findall(r'startSound(?:Shader)?\s*\(\s*"([^"]+)"', read_text(files, n), re.I):
                plain.add(canonical_name(value))
            continue
        else:
            continue
        for block in blocks:
            kv = dict((k.lower(), v) for k, v in re.findall(r'"([^"]+)"\s+"([^"]*)"', block))
            for k, v in kv.items():
                if k.startswith("snd_") and v:
                    plain.add(canonical_name(v))
            shader = kv.get("s_shader")
            if shader:
                loops = kv.get("s_looping", "0").strip() not in ("", "0")
                (looped if loops else plain).add(canonical_name(shader))
    return {name: ("both" if name in plain else "only") for name in looped}


def canonical_name(name):
    """Decl name as the game stores it (idDeclManagerLocal::MakeNameCanonical)."""
    name = name.replace("\\", "/")
    dot = name.rfind(".")
    if dot != -1:
        name = name[:dot]
    return name.lower()


def sample_source(files, sample):
    """The game data file for a sample name (.wav, or the .ogg fallback), or None."""
    base = os.path.splitext(sample)[0]
    for ext in (".wav", ".ogg"):
        if (base + ext).lower() in files:
            return base + ext
    return None


def extract_sample(files, sample, out_dir, ffmpeg, cache):
    """Returns (absolute .wav path, None) or (None, reason). Each sample is extracted once."""
    key = sample.lower()
    if key in cache:
        return cache[key]
    src = sample_source(files, sample)
    target = os.path.join(out_dir, os.path.splitext(sample)[0] + ".wav")
    if src is None:
        result = (None, "not found in the game data")
    elif os.path.exists(target):
        result = (target, None)
    else:
        os.makedirs(os.path.dirname(target), exist_ok=True)
        if src.lower().endswith(".wav"):
            with open(target, "wb") as f:
                f.write(read_bytes(files, src))
            result = (target, None)
        elif not ffmpeg:
            result = (None, "only .ogg in the game data and ffmpeg is not on PATH")
        else:
            ogg_path = os.path.splitext(target)[0] + ".ogg"
            with open(ogg_path, "wb") as f:
                f.write(read_bytes(files, src))
            r = subprocess.run([ffmpeg, "-y", "-loglevel", "error", "-i", ogg_path, target], capture_output=True, text=True)
            os.remove(ogg_path)
            result = (target, None) if r.returncode == 0 else (None, "ffmpeg failed: " + r.stderr.strip()[:200])
    cache[key] = result
    return result


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--doom-base", default=DEFAULT_BASE)
    ap.add_argument("--out", default=os.path.join(REPO, "deps", "doom3-sounds"))
    ap.add_argument("--no-audio", action="store_true")
    args = ap.parse_args()

    out_dir = os.path.abspath(args.out)
    ffmpeg = shutil.which("ffmpeg")
    files = index_pk4(args.doom_base)
    duplicates = []
    declared = declared_shaders(files, duplicates)
    declared_names = {name.lower() for _, name, _ in declared}
    implicit = implicit_shaders(files, declared_names)
    loops = speaker_loops(files)

    entries = [(f, name, parse_shader(body)) for f, name, body in declared]
    for name in implicit:
        info = parse_shader([])
        info["samples"] = [name + ".wav"]
        entries.append(("implicit", name, info))

    groups, events, cache = {}, {}, {}
    report = list(duplicates)
    missing_samples = skipped = 0
    for f, name, info in entries:
        group = "implicit" if f == "implicit" else os.path.splitext(os.path.basename(f))[0]
        ev = event_name(name)
        if ev.lower() in events:
            report.append(f"SKIP {name}: event {ev} already used by {events[ev.lower()]}")
            skipped += 1
            continue
        resolved = {}
        for kind in ("samples", "leadins"):
            resolved[kind] = []
            for s in info[kind]:
                if args.no_audio:
                    path, why = (os.path.join(out_dir, os.path.splitext(s)[0] + ".wav"), None) if sample_source(files, s) else (None, "not found in the game data")
                else:
                    path, why = extract_sample(files, s, out_dir, ffmpeg, cache)
                if path:
                    resolved[kind].append({"file": path, "sample": s})
                else:
                    report.append(f"{name}: {s}: {why}")
                    missing_samples += 1
                    if "ffmpeg" in why:
                        print(f"  {name}: {s}: {why}")
        if not resolved["samples"] and not resolved["leadins"]:
            report.append(f"SKIP {name}: no usable samples")
            skipped += 1
            continue
        events[ev.lower()] = name
        entry = dict(info)
        entry.update({"shader": name, "event": ev, "samples": resolved["samples"], "leadins": resolved["leadins"]})
        if "looping" not in entry["flags"] and name.lower() in loops:
            # Looped by an entity at run time (D-068): "only" -> the event loops; "both" -> an extra Play_<shader>_Loop.
            entry["entityLoop"] = loops[name.lower()]
        groups.setdefault(group, []).append(entry)

    manifest = {"source": "Doom 3 base (user's own copy, read-only)", "groups": [
        {"name": g, "shaders": groups[g]} for g in sorted(groups)]}
    os.makedirs(out_dir, exist_ok=True)
    manifest_path = os.path.join(out_dir, "doom_sounds.json")
    with open(manifest_path, "w", encoding="utf-8") as f:
        json.dump(manifest, f, indent=1)
    report_path = os.path.join(out_dir, "doom_sounds_report.txt")
    with open(report_path, "w", encoding="utf-8") as f:
        f.write("\n".join(report) + "\n")
    shaders = sum(len(v) for v in groups.values())
    implicit_used = len(groups.get("implicit", []))
    samples = len({s["file"].lower() for v in groups.values() for e in v for s in e["samples"] + e["leadins"]})
    print(f"manifest: {manifest_path}")
    print(f"{len(groups)} groups, {shaders} shaders ({shaders - implicit_used} declared, {implicit_used} implicit), {samples} sample files")
    print(f"not in the game data: {missing_samples} sample references; skipped: {skipped} shaders; duplicates: {len(duplicates)} (details: {report_path})")


if __name__ == "__main__":
    main()
