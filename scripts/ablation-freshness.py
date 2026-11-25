#!/usr/bin/env python3
# Copyright (c) 2026 Alessandro Sangiuliano (Slex) <alex22_7@hotmail.com>
# SPDX-License-Identifier: MIT
#
# ablation-freshness.py -- when each ablation of ablations/ was last caught,
# read from the rows scripts/ablate.sh keeps, and how far the tree has moved
# since (#1010).
#
# An ablation is booted when it is made, and again when a union changes it or
# the files it touches; nothing else boots it.  Code elsewhere can take away
# what its catch stands on, and then nobody sees it: 922-wait-whole-arm was
# caught no longer for 700 commits after a sleep of 10 ms came to last 20
# (#1009).  Each machine keeps a row per boot in its ablate.csv; this reads
# them together and says, for each ablation, its last catch: the date, the
# machine, the tree it was booted over, the accelerator, and the commits of
# the reference tree since that tree.  A union's giro boots the stalest again,
# so that every ablation comes round in turn.
#
# A row counts as a catch of an ablation when it is an ablated boot read
# CAUGHT and that ablation is the only patch of Kind `ablation' in its run: two
# ablations booted together cannot be told apart.  A patch with a Machine:
# line wants a machine of its own, one with Measure: and no Expect: is judged
# by hand, and one whose Run: is no run-x86_64.sh arguments and that has no
# Script: or Screen: is booted some other way; --stalest leaves them out, as a
# giro cannot boot them.
#
# Usage:
#   ablation-freshness.py [--ref REV] [--stalest N] [CSV...]
#
#   CSV          the ablate.csv files, one per machine (default
#                ~/uros-tests/ablations/ablate.csv)
#   --ref REV    the tree the distance is counted to (default HEAD)
#   --stalest N  only the names of the N stalest a giro can boot, one per
#                line: those never caught in these rows first, then those
#                whose tree is the most commits behind, then the oldest
#
# Exit status: 0 · 2 a CSV cannot be read, or the command line is wrong.

import os
import subprocess
import sys

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ABL = os.path.join(REPO, "ablations")


def die(msg):
    print("ablation-freshness: " + msg, file=sys.stderr)
    sys.exit(2)


def header(path):
    """The header's keyed lines, as a dict of lists: everything before the
    first `diff --git', which git apply skips."""
    fields = {}
    with open(path, encoding="utf-8", errors="replace") as f:
        for line in f:
            if line.startswith("diff --git "):
                break
            key, sep, value = line.partition(":")
            if sep and (" " not in key.strip() or key in ("Takes out", "Holds open")):
                fields.setdefault(key.strip(), []).append(value.strip())
    return fields


def git(*args):
    r = subprocess.run(["git", "-C", REPO] + list(args), capture_output=True, text=True)
    return r.stdout.strip() if r.returncode == 0 else None


def main(argv):
    ref = "HEAD"
    stalest = None
    csvs = []
    i = 0
    while i < len(argv):
        a = argv[i]
        if a == "--ref" and i + 1 < len(argv):
            ref = argv[i + 1]
            i += 2
        elif a == "--stalest" and i + 1 < len(argv):
            try:
                stalest = int(argv[i + 1])
            except ValueError:
                die("--stalest takes a number, not " + argv[i + 1])
            i += 2
        elif a.startswith("-"):
            die("usage: ablation-freshness.py [--ref REV] [--stalest N] [CSV...]")
        else:
            csvs.append(a)
            i += 1
    if not csvs:
        csvs = [os.path.expanduser("~/uros-tests/ablations/ablate.csv")]
    if git("rev-parse", "-q", "--verify", ref + "^{commit}") is None:
        die("no commit " + ref)

    kinds, bootable = {}, {}
    for name in sorted(os.listdir(ABL)):
        if not name.endswith(".patch"):
            continue
        h = header(os.path.join(ABL, name))
        n = name[:-len(".patch")]
        kinds[n] = (h.get("Kind") or ["?"])[0]
        # A giro boots what ablate.sh can judge: a Run: line of run-x86_64.sh
        # arguments, or a Script: or Screen: line, with an Expect:, on any
        # machine.  "Run: i386, not run-x86_64.sh -- ..." it cannot.
        runs = "Script" in h or "Screen" in h or \
            any(r.startswith("-") for r in h.get("Run", []))
        bootable[n] = runs and "Expect" in h and "Machine" not in h

    last = {}      # name -> (date time, host, base, accel) of its last catch
    booted = {}    # name -> date time of its last ablated boot, caught or not
    for path in csvs:
        try:
            f = open(path, encoding="utf-8", errors="replace")
        except OSError as e:
            die("cannot read %s: %s" % (path, e))
        with f:
            for line in f:
                col = line.rstrip("\n").split(",")
                # date, time, host, base, patches, arm, boot, accel, run,
                # clock_mhz, power, harness_status, verdict, log
                if len(col) < 14 or col[0] == "date" or col[5] != "ablated":
                    continue
                when = col[0] + " " + col[1]
                names = [n for n in col[4].split("+") if kinds.get(n) == "ablation"]
                if len(names) != 1:
                    continue
                n = names[0]
                if when > booted.get(n, ""):
                    booted[n] = when
                if col[12] == "CAUGHT" and when > last.get(n, ("",))[0]:
                    last[n] = (when, col[2], col[3], col[7])

    behind = {}
    def commits_since(base):
        if base not in behind:
            mb = git("merge-base", base, ref) if git("rev-parse", "-q", "--verify", base + "^{commit}") else None
            behind[base] = int(git("rev-list", "--count", mb + ".." + ref)) if mb else None
        return behind[base]

    rows = []
    for n, kind in kinds.items():
        if kind != "ablation":
            continue
        if n in last:
            when, host, base, accel = last[n]
            rows.append((1, -(commits_since(base) if commits_since(base) is not None else 10**9),
                         when, n, when, host, base, accel, commits_since(base)))
        else:
            rows.append((0, 0, "", n, None, None, None, None, None))
    rows.sort()

    if stalest is not None:
        for r in [r for r in rows if bootable[r[3]]][:stalest]:
            print(r[3])
        return 0
    print("ablation-freshness: %d ablations, read from %s, distances to %s"
          % (len(rows), ", ".join(csvs), git("rev-parse", "--short=8", ref)))
    for r in rows:
        n = r[3]
        if r[4] is None:
            seen = booted.get(n)
            print("%-48s never caught in these rows%s%s" % (n, "; last booted " + seen if seen else "",
                                                             "" if bootable[n] else " (not bootable by a giro)"))
        else:
            dist = "? commits" if r[8] is None else "%d commits" % r[8]
            print("%-48s %s on %s over %s, %s: %s behind%s" % (n, r[4], r[5], r[6], r[7], dist,
                                                              "" if bootable[n] else " (not bootable by a giro)"))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
