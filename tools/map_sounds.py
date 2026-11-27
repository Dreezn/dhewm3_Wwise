"""
Lists the sound shaders one Doom 3 map uses, for a test set of banks (PROJECT_NOTES.md S-088).

Reads the user's own Doom 3 data (read-only) and the manifest of tools/extract_doom_sounds.py.
Sources of a shader:
  speaker_auto       a speaker that starts by itself (ambience)
  speaker_triggered  a speaker that waits for a trigger (s_waitfortrigger 1: scenes, events)
  entity             a snd_* key on a map entity, or on the entity def it spawns (with inherit and def_*)
  anim               a sound frame command in the model def of such an entity (frame N sound_voice <shader>):
                     cinematics start their voice lines this way
  player             the player's def (player_doommarine)
  script             named in the map script (script/map_<name>.script): scenes
Writes deps/doom3-sounds/map_<name>.json ({"map", "shaders": {shader: [sources]}}), which
tools/wwise_doom_sounds.js --map reads, and prints the media count per group. Names only, no audio.

Usage: python3 tools/map_sounds.py <map name, e.g. mc_underground> [--doom-base <Doom 3/base>]
"""
import argparse, json, os, re
from collections import defaultdict

import extract_doom_sounds as ex

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DEFAULT_BASE = "C:/Program Files (x86)/Steam/steamapps/common/Doom 3/base"


def entity_defs(files):
    """entityDef name -> list of (key, value)."""
    defs = {}
    for n in files:
        if n.endswith(".def"):
            for m in re.finditer(r'entityDef\s+(\S+)\s*\{(.*?)\n\}', ex.read_text(files, n), re.S):
                defs.setdefault(m.group(1).lower(), re.findall(r'"([^"]+)"\s+"([^"]*)"', m.group(2)))
    return defs


def model_defs(files):
    """model def name -> its text (anims with their frame commands), braces matched."""
    defs = {}
    for n in files:
        if not n.endswith(".def"):
            continue
        text = ex.read_text(files, n)
        for m in re.finditer(r'(?m)^\s*model\s+(\S+)\s*\{', text):
            depth, i = 1, m.end()
            while depth and i < len(text):
                depth += {"{": 1, "}": -1}.get(text[i], 0)
                i += 1
            defs.setdefault(m.group(1).lower(), text[m.end():i])
    return defs


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("map")
    ap.add_argument("--doom-base", default=DEFAULT_BASE)
    args = ap.parse_args()

    files = ex.index_pk4(args.doom_base)
    out_dir = os.path.join(REPO, "deps", "doom3-sounds")
    manifest = json.load(open(os.path.join(out_dir, "doom_sounds.json"), encoding="utf-8"))
    group_of, media_of = {}, {}
    for g in manifest["groups"]:
        for s in g["shaders"]:
            k = s["shader"].lower()
            group_of[k] = g["name"]
            media_of[k] = {x["file"].lower() for x in s["samples"] + s["leadins"]}

    def key(name):
        return name.lower() if name.lower() in group_of else ex.canonical_name(name)

    defs = entity_defs(files)
    models = model_defs(files)

    def model_sounds(name, seen):
        name = name.lower()
        if name in seen or name not in models:
            return set()
        seen.add(name)
        text = models[name]
        out = {key(v) for v in re.findall(r'\bframe\s+\d+\s+sound\w*\s+"?([^"\s]+)', text)}
        for v in re.findall(r'\binherit\s+"?([^"\s]+)', text):
            out |= model_sounds(v, seen)
        return out

    def def_sounds(name, seen):
        name = name.lower()
        if name in seen or name not in defs:
            return set()
        seen.add(name)
        out = set()
        for k, v in defs[name]:
            kl = k.lower()
            if kl.startswith("snd_") or kl == "s_shader":
                out.add(key(v))
            if kl == "model":
                anim_models.add(v)
            if kl == "inherit" or kl.startswith("def_"):
                out |= def_sounds(v, seen)
        return out

    sources = defaultdict(set)
    anim_models = set()   # model defs of the map's entities, filled by def_sounds
    map_text = ex.read_text(files, f"maps/game/{args.map}.map")
    for ent in re.findall(r'\{\s*\n(.*?)\n\}', map_text, re.S):
        kv = re.findall(r'"([^"]+)"\s+"([^"]*)"', ent)
        kd = {k.lower(): v for k, v in kv}
        for k, v in kv:
            kl = k.lower()
            if kl == "s_shader":
                sources[key(v)].add("speaker_triggered" if kd.get("s_waitfortrigger") == "1" else "speaker_auto")
            elif kl.startswith("snd_"):
                sources[key(v)].add("entity")
        if "classname" in kd:
            for s in def_sounds(kd["classname"], set()):
                sources[s].add("entity")
        if "model" in kd:
            anim_models.add(kd["model"])
    for s in def_sounds("player_doommarine", set()):
        sources[s].add("player")
    for m in anim_models:
        for s in model_sounds(m, set()):
            sources[s].add("anim")
    # The map script (script/map_<name>.script) starts sounds by shader name in its scenes.
    script = f"script/map_{args.map}.script"
    if script in files:
        text = ex.read_text(files, script)
        for v in re.findall(r'(?:startSound(?:Shader)?|playSound|sys\.\w*[Ss]ound\w*)\s*\(\s*"([^"]+)"', text):
            if not v.lower().startswith("snd_"):	# "snd_x" keys of entities are already listed
                sources[key(v)].add("script")
        for v in re.findall(r'"([\w/\\.]+)"', text):
            if key(v) in group_of:
                sources[key(v)].add("script")

    known = {k: sorted(v) for k, v in sources.items() if k in group_of}
    path = os.path.join(out_dir, f"map_{args.map}.json")
    with open(path, "w", encoding="utf-8") as f:
        json.dump({"map": args.map, "shaders": dict(sorted(known.items()))}, f, indent=1)

    per_group = defaultdict(lambda: [set(), set()])   # all media, media without self-starting-only shaders
    for k, src in known.items():
        per_group[group_of[k]][0].update(media_of[k])
        if src != ["speaker_auto"]:
            per_group[group_of[k]][1].update(media_of[k])
    print(f"{path}: {len(known)} shaders ({len(sources) - len(known)} names not in the manifest)")
    print(f"  {'group':24s} media  without ambience")
    for g, (a, b) in sorted(per_group.items(), key=lambda x: -len(x[1][0])):
        print(f"  {g:24s} {len(a):5d}  {len(b):5d}")


if __name__ == "__main__":
    main()
