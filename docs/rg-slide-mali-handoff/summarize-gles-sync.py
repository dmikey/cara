"""Rank producer waits using one Cara profile window per requested refresh end."""
import argparse
from collections import Counter, defaultdict
import re

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("log")
parser.add_argument("ends", nargs="+", type=int)
args = parser.parse_args()
ends = set(args.ends)
totals = defaultdict(Counter)
seen = set()
pattern = re.compile(r"CaraPerf: sync_time end=(\d+) api=(\S+) calls=(\d+) before_ns=(\d+) driver_ns=(\d+) after_ns=(\d+)")
with open(args.log) as stream:
    for line in stream:
        row = pattern.search(line)
        if row is None or int(row[1]) not in ends:
            continue
        key = int(row[1]), row[2]
        if key in seen:
            parser.error("Duplicate window/API pair. Supply one boot at a time.")
        seen.add(key)
        totals[row[2]].update(calls=int(row[3]), before=int(row[4]), driver=int(row[5]), after=int(row[6]))
if ends != {end for end, api in seen}:
    parser.error("Missing a requested timing window")
refreshes = 120 * len(ends)
def duration(row):
    return row["before"] + row["driver"] + row["after"]

print("API\tcalls/refresh\tbefore driver ms\tdriver wall ms\treturn ms\ttotal ms/refresh")
for api, row in sorted(totals.items(), key=lambda entry: -duration(entry[1])):
    print(f"{api}\t{row['calls']/refreshes:.2f}\t" + "\t".join(
        f"{n/(1e6*refreshes):.4f}" for n in (row['before'], row['driver'], row['after'], duration(row))))
