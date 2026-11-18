#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause-Patent
"""Fetch the checkouts UniXNU-piano pins, into $UNIXNU_RESEARCH (default ./unixnu-research).

Reads unixnu.lock's [pin."name"] tables — the same list tooling/unixnu.sh expects under
~/Downloads/XiaomiPad8Pro-Ubuntu/unixnu-research — and clones each one at its commit.

    python3 scripts/fetch-pins.py [--only NAME[,NAME]] [--depth N] [--dry-run]

Why this exists: every build path in the repository assumes those checkouts already exist locally
(the author's Mac). A CI runner starts empty, so the pins have to be materialised from the lockfile.
Nothing here is a fork: the deltas stay in the device repository (firmware/patches, patches/, dts/).

Special cases, matching tooling/unixnu.sh:
  Mu-Silicium   -> cloned as Mu-Silicium-piano (the checkout name the script and piano-uefi's
                   setup-deps.sh expect); its submodules are pinned in the lockfile too.
  piano-uefi    -> cloned as piano-uefi-UniXNU-piano, branch "unixnu" (the lockfile records it).
  k27           -> skipped: it is a *local* tree ("local:" in the lockfile), not a repository.
                   See ci/README.md §"the three unknowns".
"""

import argparse
import os
import subprocess
import sys
import tomllib

RENAMES = {
    "Mu-Silicium": "Mu-Silicium-piano",
    "piano-uefi": "piano-uefi-UniXNU-piano",
}

def sh(cmd, cwd=None):
    print("+", " ".join(cmd), flush=True)
    subprocess.run(cmd, cwd=cwd, check=True)

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--only")
    ap.add_argument("--depth", default="0", help="0 = full history (needed for --branch pins)")
    ap.add_argument("--dry-run", action="store_true")
    ap.add_argument("--lock", default="unixnu.lock")
    args = ap.parse_args()

    root = os.environ.get("UNIXNU_RESEARCH",
                          os.path.join(os.getcwd(), "unixnu-research"))
    with open(args.lock, "rb") as f:
        lock = tomllib.load(f)
    pins = lock.get("pin", {})
    only = set(args.only.split(",")) if args.only else None

    os.makedirs(root, exist_ok=True)
    missing = []
    for name, spec in pins.items():
        if only and name not in only:
            continue
        if "git" not in spec:
            print(f"skip {name}: {spec.get('role','').splitlines()[0][:60]} (no git: local tree)")
            continue
        dest = os.path.join(root, RENAMES.get(name, name))
        if os.path.isdir(os.path.join(dest, ".git")):
            head = subprocess.run(["git", "-C", dest, "rev-parse", "HEAD"],
                                  capture_output=True, text=True).stdout.strip()
            if head == spec["commit"]:
                print(f"ok   {name} @ {head[:10]}")
                continue
            print(f"stale {name}: {head[:10]} != {spec['commit'][:10]}")
        if args.dry_run:
            print(f"would clone {name} -> {dest} @ {spec['commit'][:10]}")
            continue
        cmd = ["git", "clone", "--no-tags", spec["git"], dest]
        if args.depth != "0":
            cmd[2:2] = ["--depth", args.depth]
        sh(cmd)
        sh(["git", "-C", dest, "checkout", "--quiet", spec["commit"]])
        for sub, commit in (spec.get("submodules") or {}).items():
            sh(["git", "-C", dest, "submodule", "update", "--init", "--depth", "1", sub])
            sh(["git", "-C", os.path.join(dest, sub), "checkout", "--quiet", commit])

    # the k27 tree is local-only; it is the one input a public runner cannot clone
    k27 = pins.get("k27", {})
    if k27 and "git" not in k27:
        missing.append("k27 (local tree, " + k27.get("xnu", "?") + ")")
    if missing:
        print("\nNOT FETCHED:")
        for m in missing:
            print("  -", m)
        print("See ci/README.md — these are the inputs the build cannot reconstruct by itself.")
        return 0
    return 0

if __name__ == "__main__":
    sys.exit(main())
