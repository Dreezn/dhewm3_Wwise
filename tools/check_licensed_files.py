#!/usr/bin/env python3
"""Fail if licensed game data, audio or prebuilt libraries are in this repo or its history.

Checks the files tracked by this repo (not the dhewm3 submodule, which is upstream's)
and every file name that ever appeared in any commit.

Usage: python tools/check_licensed_files.py
Standard library only.
"""
import pathlib
import re
import subprocess
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent

FORBIDDEN = [
    (re.compile(r"\.pk4$", re.I), "Doom 3 game archive"),
    (re.compile(r"\.(wav|ogg|mp3|wem|bnk)$", re.I), "audio or Wwise bank"),
    (re.compile(r"(^|/)Originals/", re.I), "Wwise Originals folder"),
    (re.compile(r"\.(dll|lib|a|exe)$", re.I), "binary or prebuilt library"),
    (re.compile(r"^deps/", re.I), "local dependency folder"),
]


def git(*args):
    return subprocess.run(["git", *args], cwd=ROOT, capture_output=True, text=True, check=True).stdout


def main():
    tracked = set(git("ls-files").splitlines())
    history = set(git("log", "--all", "--name-only", "--format=").splitlines())
    # Files inside the dhewm3 submodule are upstream's, not ours.
    names = {n for n in (tracked | history) if n and not n.startswith("dhewm3/")}
    problems = []
    for name in sorted(names):
        for pattern, reason in FORBIDDEN:
            if pattern.search(name):
                where = "tracked" if name in tracked else "in history only"
                problems.append(f"{name}: {reason} ({where})")
                break
    if problems:
        print("Forbidden files found:")
        print("\n".join("  " + p for p in problems))
        return 1
    print(f"ok: {len(names)} file names checked, none forbidden")
    return 0


if __name__ == "__main__":
    sys.exit(main())
