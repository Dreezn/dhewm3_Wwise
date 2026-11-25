#!/usr/bin/env python3
"""Fail if licensed game data, audio or prebuilt libraries are in this repo or its history.

Checks the files tracked by this repo, including the copied dhewm3 source,
and every file name that ever appeared in any commit.
Binaries that come with dhewm3 upstream are listed in ALLOWED_UPSTREAM_BINARIES.

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


# Binary files that dhewm3 upstream ships in its own source tree (ImGui examples,
# GLFW libraries, MIT licence). Reviewed and accepted; see PROJECT_NOTES.md D-033.
ALLOWED_UPSTREAM_BINARIES = {
    "dhewm3/neo/libs/imgui/examples/libs/glfw/lib-vc2010-32/glfw3.lib",
    "dhewm3/neo/libs/imgui/examples/libs/glfw/lib-vc2010-64/glfw3.lib",
}


def git(*args):
    return subprocess.run(["git", *args], cwd=ROOT, capture_output=True, text=True, check=True).stdout


def main():
    tracked = set(git("ls-files").splitlines())
    history = set(git("log", "--all", "--name-only", "--format=").splitlines())
    names = {n for n in (tracked | history) if n}
    problems = []
    for name in sorted(names):
        if name in ALLOWED_UPSTREAM_BINARIES:
            continue
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
