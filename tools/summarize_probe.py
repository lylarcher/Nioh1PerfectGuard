"""Summarise a probe.log: per-target hit counts, distinct call chains, sample args.

The probe logs one line per hit::

    HIT#12 target=2 n=5 rva=0x7B4C40 tid=1234 rcx=... rdx=... r8=... r9=... rsp=... chain=[0x749712,0x70ED33,...]

Grouping by target and by the *first* .text frame in `chain` is what turns a raw
log into "who calls this, how often".

Usage:
    python summarize_probe.py <probe.log> [--top 12]
"""
from __future__ import annotations

import argparse
import collections
import re
import sys

HIT = re.compile(
    r"^(\d\d:\d\d:\d\d\.\d\d\d)\s+HIT#(\d+)\s+target=(\d+)\s+n=(\d+)\s+rva=0x([0-9A-Fa-f]+)"
    r"\s+tid=(\d+)\s+rcx=([0-9A-Fa-f]+)\s+rdx=([0-9A-Fa-f]+)\s+r8=([0-9A-Fa-f]+)\s+r9=([0-9A-Fa-f]+)"
    r"\s+rsp=([0-9A-Fa-f]+)\s+chain=\[([^\]]*)\]"
)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("log")
    ap.add_argument("--top", type=int, default=12)
    args = ap.parse_args()

    per_target = collections.defaultdict(lambda: dict(
        count=0, n=0, rva=None, first=None, last=None, tids=collections.Counter(),
        chains=collections.Counter(), immediate=collections.Counter(), samples=[]))

    probe_lines = []
    with open(args.log, "r", encoding="utf-8", errors="replace") as fh:
        for line in fh:
            line = line.rstrip("\r\n")
            if line.startswith("HIT#") or " HIT#" in line:
                m = HIT.match(line)
                if not m:
                    continue
                (_ts, _h, tgt, n, rva, tid, rcx, rdx, r8, r9, _rsp, chain) = m.groups()
                tgt = int(tgt)
                d = per_target[tgt]
                d["count"] += 1
                d["n"] = max(d["n"], int(n))
                d["rva"] = rva
                d["tids"][tid] += 1
                frames = [f for f in chain.split(",") if f]
                # first frame after the target itself is the immediate caller
                if frames:
                    d["immediate"][frames[0]] += 1
                    d["chains"][",".join(frames[:4])] += 1
                if len(d["samples"]) < 5:
                    d["samples"].append((rcx, rdx, r8, r9))
            else:
                probe_lines.append(line)

    if not per_target:
        print("no HIT lines found in %s" % args.log)
        for l in probe_lines[-8:]:
            print("  |", l)
        return 1

    print("=" * 78)
    print("PROBE SESSION SUMMARY  (%s)" % args.log)
    print("=" * 78)
    total = sum(d["count"] for d in per_target.values())
    print("targets hit: %d    total hits: %d\n" % (len(per_target), total))

    for tgt in sorted(per_target):
        d = per_target[tgt]
        print("-" * 78)
        print("target %d   rva=0x%s   hits=%d" % (tgt, d["rva"], d["count"]))
        print("  threads: %s" % ", ".join("%s(%d)" % (t, c)
                                           for t, c in d["tids"].most_common(4)))
        print("  immediate callers (first .text frame):")
        for fr, c in d["immediate"].most_common(args.top):
            print("      %-14s %6d hits" % (fr, c))
        if len(d["chains"]) > 1:
            print("  distinct shallow chains: %d ; top:" % len(d["chains"]))
            for ch, c in d["chains"].most_common(min(4, args.top)):
                print("      %-52s %6d" % (ch, c))
        print("  sample args (rcx, rdx, r8, r9):")
        for s in d["samples"]:
            print("      0x%s 0x%s 0x%s 0x%s" % s)
        print()

    print("=" * 78)
    print("probe lifecycle lines:")
    for l in probe_lines[-10:]:
        print("  |", l)
    return 0


if __name__ == "__main__":
    sys.exit(main())
