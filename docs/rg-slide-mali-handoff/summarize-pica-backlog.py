"""Rank work that was executing on the GLES worker while its producer needed a result."""
import argparse
from collections import Counter
import re

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("log", help="CaraPerf log for one boot")
parser.add_argument("ends", type=int, nargs="+", help="ends of 120-refresh windows")
args = parser.parse_args()
ends = set(args.ends)
owners = Counter()
sources = Counter()
commands = Counter()
per_request = Counter()
waits = Counter()
observations = set()
seen_windows = set()
with open(args.log) as log:
    for line in log:
        match = re.search(r"CaraPerf: (backlog|sync_time|window) end=(\d+) (.*)", line)
        if not match or int(match[2]) not in ends:
            continue
        kind, end, body = match.groups()
        data = dict(pair.split("=", 1) for pair in body.split() if "=" in pair)
        if kind == "window":
            if end in seen_windows:
                parser.error("Found repeated window, supply a single boot")
            seen_windows.add(end)
        elif kind == "sync_time":
            waits[data["api"]] += int(data["before_ns"])
        else:
            request = data['requester'], data['query']
            command = data['source'], data['command'], data.get('vs_entry', '-1')
            key = (end, request, command)
            if key in observations:
                parser.error("Duplicate backlog row")
            observations.add(key)
            ns = int(data['ns'])
            owners[request] += ns
            sources[command[0]] += ns
            commands[command] += ns
            per_request[(request, command)] += ns
if {int(end) for end in seen_windows} != ends or not observations or not waits:
    parser.error("Missing complete window or backlog data")
scale = len(ends) * 120 * 1e6
def show(title, counter, limit):
    print(title)
    for key, ns in counter.most_common(limit):
        print(f"  {ns/scale:.3f} ms/refresh  {key}")
print(f"Pre-driver wait {sum(waits.values())/scale:.3f} ms/refresh")
print(f"Inside preceding recorded worker commands {sum(commands.values())/scale:.3f} ms/refresh")
show("Waiting requester and query", owners, 10)
show("Origin of earlier command", sources, 10)
show("Earlier command by origin", commands, 15)
show("Largest individual handoffs", per_request, 15)
