#!/usr/bin/env python3
"""
Falcon 3.0 Gold Native Host — Game Data & SHA-256 Verification Tool

Verifies that a user-supplied Falcon Gold CD installation in `game_data/`
matches the exact executable SHA-256 hashes supported by this project,
and checks for required native 3D theater files and optional CM-32L ROMs.
"""

import argparse
import hashlib
import pathlib
import sys

EXPECTED_EXES = {
    "FALCONCD.EXE": {
        "title": "Falcon 3.0 (F-16C)",
        "size": 1373671,
        "sha256": "0c51f01bb167fe6bf48d8d386482258fba7dbab920625d589352321ce44298f3",
        "required": True,
    },
    "HORNETCD.EXE": {
        "title": "Hornet: Naval Strike (F/A-18)",
        "size": 1412635,
        "sha256": "4322ca4cdd838b15fb5e4d5d8f4cee762b7eaaf75792bbd6ca6c80f5751c5d72",
        "required": False,
    },
    "MIGCD.EXE": {
        "title": "MiG-29: Deadly Adversary",
        "size": 1369383,
        "sha256": "ae67a76fb4b43856d701bd1018f2931f9b55b59926f320fb90b46ff47868a80a",
        "required": False,
    },
}

REQUIRED_ASSETS = [
    "VGRAPH",
    "OBJECTS",
    "MAPOBJ",
    "KUWAIT.MAP",
    "ALT.IRQ",
    "ISLDAT.IRQ",
    "MAPOBJ.IRQ",
    "ILS.IRQ",
    "ALTPAL.IRQ",
    "MUSIC/Y.MID",
]


def sha256_file(path: pathlib.Path) -> str:
    h = hashlib.sha256()
    with path.open("rb") as f:
        for chunk in iter(lambda: f.read(65536), b""):
            h.update(chunk)
    return h.hexdigest()


def find_ci(root: pathlib.Path, rel: str) -> pathlib.Path | None:
    cur = root
    for part in pathlib.PurePosixPath(rel).parts:
        if not cur.is_dir():
            return None
        match = None
        for entry in cur.iterdir():
            if entry.name.lower() == part.lower():
                match = entry
                break
        if match is None:
            return None
        cur = match
    return cur


def main() -> int:
    ap = argparse.ArgumentParser(description="Verify Falcon Gold game_data/ files & SHA-256 hashes.")
    ap.add_argument("game_dir", nargs="?", default="game_data", help="Path to your Falcon Gold directory (default: game_data)")
    ap.add_argument("--rom-dir", default=None, help="Optional path to Roland CM-32L / MT-32 ROM folder")
    args = ap.parse_args()

    root = pathlib.Path(args.game_dir)
    if not root.is_dir():
        print(f"[ERROR] Directory not found: {root.resolve()}")
        print("        Copy your legally owned Falcon Gold CD files into 'game_data/' and re-run.")
        return 1

    print(f"Checking Falcon Gold game data in: {root.resolve()}\n")
    ok_all = True

    print("=== Executables (SHA-256) ===")
    for name, info in EXPECTED_EXES.items():
        p = find_ci(root, name)
        if p is None:
            tag = "[FAIL]" if info["required"] else "[INFO]"
            print(f"  {tag} {name:14s} — not found ({info['title']})")
            if info["required"]:
                ok_all = False
            continue
        actual_sha = sha256_file(p)
        actual_sz = p.stat().st_size
        if actual_sha == info["sha256"]:
            print(f"  [ OK ] {name:14s} — EXACT MATCH ({info['title']}, {actual_sz:,} B)")
        else:
            print(f"  [WARN] {name:14s} — SHA-256 MISMATCH ({info['title']})")
            print(f"         Expected: {info['sha256']} ({info['size']:,} B)")
            print(f"         Actual:   {actual_sha} ({actual_sz:,} B)")
            if info["required"]:
                ok_all = False

    print("\n=== Core Assets ===")
    for rel in REQUIRED_ASSETS:
        p = find_ci(root, rel)
        if p is not None:
            print(f"  [ OK ] {rel}")
        else:
            print(f"  [FAIL] {rel} — missing")
            ok_all = False

    if args.rom_dir:
        rdir = pathlib.Path(args.rom_dir)
        print(f"\n=== Roland CM-32L / MT-32 ROMs ({rdir}) ===")
        ctrl = find_ci(rdir, "CM32L_CONTROL.ROM") or find_ci(rdir, "MT32_CONTROL.ROM")
        pcm = find_ci(rdir, "CM32L_PCM.ROM") or find_ci(rdir, "MT32_PCM.ROM")
        if ctrl and pcm:
            print(f"  [ OK ] Found {ctrl.name} + {pcm.name} (64-Voice CM-32L Sample Proxy supported!)")
        else:
            print("  [WARN] Missing CONTROL/PCM ROMs — CM-32L music & 64-voice sample proxy require user-supplied ROMs.")

    print()
    if ok_all:
        print("SUCCESS: Falcon Gold game data verified! You are ready to build and run falcon30_native.")
        return 0
    print("FAILED: One or more required files are missing or have mismatched SHA-256 hashes.")
    return 1


if __name__ == "__main__":
    sys.exit(main())
