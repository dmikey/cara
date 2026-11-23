#!/usr/bin/env python3
"""Compare original and paired LDR state/timer traces; no ROM is required."""
import argparse
import os
import re
import subprocess

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("binary")
args = parser.parse_args()
pattern = re.compile(r"LDR pair cases=(\d+) selected=(\d+) fused=(\d+) fallback=(\d+) differential_digest=(\d+) budget_trace=(\d+)")
for chain in (0, 1):
    receipts = []
    for paired in (0, 1):
        env = dict(os.environ, CARA_FASTINTERP_LDR_PAIR_TEST="1",
                   CARA_FASTINTERP_LDR_PAIRS=str(paired),
                   CARA_FASTINTERP_CHAIN_BUDGET=str(chain),
                   CARA_FASTINTERP_AL_DISPATCH="1", CARA_FASTINTERP_ALU_OPERANDS="1",
                   CARA_FASTINTERP_RAM_BURST="1", CARA_FASTINTERP_LDR_PAIR_CENSUS="1",
                   CARA_FASTINTERP_LDR_SP_STATIC="0")
        env.pop("CARA_FASTINTERP_LDR_PAIR_MICROBENCH", None)
        result = subprocess.run([args.binary], env=env, text=True, capture_output=True, timeout=180)
        if result.returncode:
            raise SystemExit(result.stdout + result.stderr)
        match = pattern.search(result.stdout)
        if not match:
            raise SystemExit("LDR fixture did not produce a test receipt")
        cases, selected, fused, fallback, state, timing = map(int, match.groups())
        if cases < 20000 or (selected > 0) != bool(paired) or (paired and (not fused or not fallback)):
            raise SystemExit("LDR fixture did not exercise the requested decode path")
        receipts.append((cases, state, timing))
        print(f"chain={chain} paired={paired}: {match.group(0)}")
    if receipts[0] != receipts[1]:
        raise SystemExit(f"LDR pairing changed state or timing: {receipts}")
print("LDR original/paired state and timing traces match in both chaining modes")
