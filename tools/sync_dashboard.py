#!/usr/bin/env python3
"""Generate docs/dashboard-data.json from PROJECT_NOTES.md.

PROJECT_NOTES.md is the single source of truth for the step log (§12.1),
decisions (§13) and open questions (§11). The dashboard reads the JSON
produced here.

Usage:
  python tools/sync_dashboard.py          write docs/dashboard-data.json
  python tools/sync_dashboard.py --check  exit 1 if the JSON is out of date

Only standard library is used.
"""
import json
import pathlib
import re
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent
NOTES = ROOT / "PROJECT_NOTES.md"
OUT = ROOT / "docs" / "dashboard-data.json"

# Column names for each table row kind, in the order they appear in the notes.
TABLES = {
    "goals": (re.compile(r"G-\d{2}"), ["id", "goal", "status"]),
    "next": (re.compile(r"N-\d{2}"), ["id", "step", "status"]),
    "phases": (re.compile(r"P-\d{2}"), ["id", "name", "status", "steps"]),
    "steps": (re.compile(r"S-\d{3}"), ["id", "date", "phase", "action", "result", "decision", "notes"]),
    "decisions": (re.compile(r"D-\d{3}"), ["id", "date", "category", "decision", "reason", "status", "revisit"]),
    "questions": (re.compile(r"Q-\d{2}"), ["id", "question", "status", "answer"]),
}


def split_row(line):
    """'| a | b |' -> ['a', 'b'] (empty cells kept)."""
    return [cell.strip() for cell in line.strip().strip("|").split("|")]


def parse(text):
    data = {name: [] for name in TABLES}
    seen = set()
    for line in text.splitlines():
        if not line.startswith("|"):
            continue
        parts = line.split("|")
        if len(parts) < 3:
            continue
        first = parts[1].strip()
        for name, (pattern, columns) in TABLES.items():
            if not pattern.fullmatch(first):
                continue
            values = split_row(line)
            if len(values) != len(columns):
                raise SystemExit(
                    f"{NOTES.name}: row {first} has {len(values)} cells, expected {len(columns)}"
                )
            if first in seen:
                raise SystemExit(f"{NOTES.name}: duplicate ID {first}")
            seen.add(first)
            data[name].append(dict(zip(columns, values)))
            break
    return data


STEP_STATE = re.compile(r"^(.*?)\s*\[(done|blocked|todo)\]\s*$")


def split_steps(data):
    """'Step one [done]; Step two [todo]' -> [{'text': ..., 'state': ...}, ...]."""
    for phase in data["phases"]:
        steps = []
        for part in phase["steps"].split(";"):
            part = part.strip()
            if not part:
                continue
            m = STEP_STATE.match(part)
            if not m:
                raise SystemExit(f"Phase {phase['id']}: step without [done|blocked|todo]: {part}")
            steps.append({"text": m.group(1), "state": m.group(2)})
        phase["steps"] = steps
    return data


def dashboard_view(data):
    """Only open questions go to the dashboard. Answered ones stay in the notes."""
    data = dict(data)
    data = split_steps(data)
    data["questions"] = [q for q in data["questions"] if q["status"].startswith("Open")]
    return data


def render(data):
    return json.dumps(data, indent=2, ensure_ascii=False) + "\n"


def main(argv):
    data = dashboard_view(parse(NOTES.read_text(encoding="utf-8")))
    text = render(data)
    if "--check" in argv:
        current = OUT.read_text(encoding="utf-8") if OUT.exists() else ""
        if current != text:
            print(f"{OUT.relative_to(ROOT)} is out of date. Run: python tools/sync_dashboard.py")
            return 1
        print("dashboard data is up to date")
        return 0
    OUT.parent.mkdir(parents=True, exist_ok=True)
    OUT.write_text(text, encoding="utf-8", newline="\n")
    counts = ", ".join(f"{len(v)} {k}" for k, v in data.items())
    print(f"wrote {OUT.relative_to(ROOT)} ({counts})")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
