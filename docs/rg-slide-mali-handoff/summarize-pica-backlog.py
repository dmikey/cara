"""Rank work that was executing on the GLES worker while its producer needed a result."""
import argparse
import gzip
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
source_apis = Counter()
per_request = Counter()
waits = Counter()
observations = set()
seen_windows = set()
sync_keys = set()
open_log = gzip.open if args.log.endswith(".gz") else open
with open_log(args.log, "rt") as log:
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
            key = (end, data["api"])
            if key in sync_keys:
                parser.error("Duplicate synchronous API for a window")
            sync_keys.add(key)
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
            source_apis[command[:2]] += ns
            per_request[(request, command)] += ns
if ({int(end) for end in seen_windows} != ends or
        {int(end) for end, _ in sync_keys} != ends or
        {int(end) for end, _, _ in observations} != ends):
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
show("Earlier command by origin", source_apis, 15)
show("Earlier command by origin and vertex entry", commands, 15)
show("Largest individual handoffs", per_request, 15)
