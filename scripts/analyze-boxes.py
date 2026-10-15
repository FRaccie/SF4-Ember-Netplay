#!/usr/bin/env python3
"""Reads what the in-game hitboxes test wrote (scripts/run-selftest.ps1 -Test
hitboxes -KeepDir <folder>) and says which of the game's body boxes explain
the outcomes: a box a fighter is really hit on overlaps the attack's live box
in tries that hit and never in a try that whiffed.

    python scripts/analyze-boxes.py <folder>

The frame layout is DumpFrame's in src/sf4e/sf4e__NetplayRuntime__SelfTest.cxx.
"""
import re, struct, sys
from collections import defaultdict
from pathlib import Path

BODY, RECORD, ATTACK = 120, 0xA0, 0xC0
LIVE_AT, LIVE = 0xAC, 2

def frames(data):
    at = 0
    while at + 24 <= len(data):
        magic, probe, trial, _, _, _ = struct.unpack_from('<IIIfIi', data, at)
        if magic != 0x46584F42: raise SystemExit(f'not a frame at {at:#x}')
        at += 24
        sides = []
        for _ in range(2):
            at += 28 + 0x200
            body = data[at:at + BODY * RECORD]; at += BODY * RECORD
            lists = []
            for _ in range(6):
                _, _, kept = struct.unpack_from('<III', data, at); at += 12
                lists.append([data[at + i * ATTACK:at + (i + 1) * ATTACK] for i in range(kept)]); at += kept * ATTACK
            sides.append((body, lists))
        # Explored blocks, which a dump written before they existed does not have.
        if at + 4 <= len(data) and struct.unpack_from('<I', data, at)[0] != 0x46584F42: at += 4 + struct.unpack_from('<I', data, at)[0] * (12 + 0x400)
        yield probe, trial, sides

def box(record):
    xs = [struct.unpack_from('<f', record, i * 0x10)[0] for i in range(4)]
    ys = [struct.unpack_from('<f', record, i * 0x10 + 4)[0] for i in range(4)]
    if any(v != v or abs(v) > 1e4 for v in xs + ys) or max(xs) - min(xs) < 1e-3 or max(ys) - min(ys) < 1e-3: return None
    return min(xs), min(ys), max(xs), max(ys)

def overlap(a, b): return a[0] < b[2] and b[0] < a[2] and a[1] < b[3] and b[1] < a[3]

def main(folder):
    folder = Path(folder)
    outcome, names = {}, []
    for line in (folder / 'selftest-hitboxes.txt').read_text().splitlines():
        found = re.match(r'INFO (.+? \(.+?\)) try (\d+) at [\d.]+: (hit|whiff)', line)
        if not found: continue
        if found.group(1) not in names: names.append(found.group(1))
        outcome[names.index(found.group(1)), int(found.group(2))] = found.group(3) == 'hit'
    touched = defaultdict(set)   # (probe, list) -> {(trial, body box)}
    for probe, trial, (attacker, defender) in frames((folder / 'selftest-hitboxes.txt.boxes').read_bytes()):
        for which in (1, 2):
            live = [b for b in (box(r) for r in attacker[1][which] if r[LIVE_AT] == LIVE) if b]
            if not live: continue
            for index in range(BODY):
                b = box(defender[0][index * RECORD:(index + 1) * RECORD])
                if b and any(overlap(b, h) for h in live): touched[probe, which].add((trial, index))
    for (probe, which), pairs in sorted(touched.items()):
        hits = {t for (p, t), hit in outcome.items() if p == probe and hit}
        whiffs = {t for (p, t), hit in outcome.items() if p == probe and not hit}
        if len(hits) + len(whiffs) < 2: continue
        print(f'{names[probe]}, attack list {which}: {len(hits)} hits, {len(whiffs)} whiffs')
        for index in sorted({i for _, i in pairs}):
            in_hits = len({t for t, i in pairs if i == index and t in hits})
            in_whiffs = len({t for t, i in pairs if i == index and t in whiffs})
            verdict = 'not hit there' if in_whiffs else 'explains every hit' if in_hits == len(hits) else 'possible'
            print(f'  box {index:3d}: touched in {in_hits} hits, {in_whiffs} whiffs: {verdict}')

if __name__ == '__main__':
    main(sys.argv[1] if len(sys.argv) > 1 else '.')
