#!/usr/bin/env python3
"""Compare original and paired VLDR state/timer traces; no ROM is required."""
import argparse
import os
import re
import subprocess

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("binary")
args = parser.parse_args()
pattern = re.compile(r"VLDR pair cases=(\d+) selected=(\d+) differential_digest=(\d+) budget_trace=(\d+)")
for chain in (0, 1):
    receipts = []
    for paired in (0, 1):
        env = dict(os.environ, CARA_FASTINTERP_VLDR_PAIR_TEST="1",
                   CARA_FASTINTERP_VLDR_PAIRS=str(paired),
                   CARA_FASTINTERP_CHAIN_BUDGET=str(chain),
                   CARA_FASTINTERP_AL_DISPATCH="1", CARA_FASTINTERP_ALU_OPERANDS="1",
                   CARA_FASTINTERP_RAM_BURST="1")
        env.pop("CARA_FASTINTERP_VLDR_PAIR_MICROBENCH", None)
        result = subprocess.run([args.binary], env=env, text=True, capture_output=True, timeout=180)
        if result.returncode:
            raise SystemExit(result.stdout + result.stderr)
        match = pattern.search(result.stdout)
        if not match:
            raise SystemExit("VLDR fixture did not produce a test receipt")
        cases, selected, state, timing = map(int, match.groups())
        if cases < 81920 or (selected > 0) != bool(paired):
            raise SystemExit("VLDR fixture did not exercise the requested decode path")
        receipts.append((cases, state, timing))
        print(f"chain={chain} paired={paired}: {match.group(0)}")
    if receipts[0] != receipts[1]:
        raise SystemExit(f"VLDR pairing changed state or timing: {receipts}")
print("VLDR original/paired state and timing traces match in both chaining modes")
