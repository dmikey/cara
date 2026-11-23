"""Summarize CPU residency and overlap from an Android atrace sched capture."""
import argparse
from bisect import bisect_left
from collections import Counter, defaultdict
import re

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("trace")
parser.add_argument("producer_tid", type=int)
parser.add_argument("worker_tid", type=int)
parser.add_argument("--from-epoch-ms", type=int)
parser.add_argument("--to-epoch-ms", type=int)
args = parser.parse_args()
targets = (args.producer_tid, args.worker_tid)
switch = re.compile(r"\[(\d+)\]\s+\S+\s+([\d.]+): sched_switch: .*?prev_pid=(\d+) .*?prev_state=(\S+) ==> .*?next_pid=(\d+)")
wake = re.compile(r"^\s*(.+?)-(\d+)\s+.*? ([\d.]+): sched_waking: .*? pid=(\d+) ")
running = {}
first = {}
segments = defaultdict(list)
wakeups = defaultdict(list)
epoch_offset = None
with open(args.trace) as trace:
    for line in trace:
        if "trace_event_clock_sync: realtime_ts=" in line:
            sync = re.search(r"([\d.]+): tracing_mark_write: trace_event_clock_sync: realtime_ts=(\d+)", line)
            if sync:
                epoch_offset = int(sync[2]) * 1000 - round(float(sync[1]) * 1e6)
        match = switch.search(line)
        if match:
            cpu, timestamp, prev, state, nxt = match.groups()
            cpu, prev, nxt = int(cpu), int(prev), int(nxt)
            t = round(float(timestamp) * 1e6)
            if cpu in running:
                start, owner = running[cpu]
                if owner != prev:
                    raise ValueError(f"Missing context switch on CPU {cpu} at {timestamp}")
                if prev in targets:
                    segments[prev].append((start, t, cpu, state))
            else:
                first[cpu] = t
            running[cpu] = (t, nxt)
        elif "sched_waking:" in line:
            match = wake.search(line)
            if match and int(match[4]) in targets:
                wakeups[int(match[4])].append((round(float(match[3]) * 1e6), match[1]))
start = max(first.values())
end = min(t for t, owner in running.values())
if args.from_epoch_ms is not None or args.to_epoch_ms is not None:
    if epoch_offset is None:
        raise ValueError("No realtime clock sync marker")
    if args.from_epoch_ms is not None:
        start = max(start, args.from_epoch_ms * 1000 - epoch_offset)
    if args.to_epoch_ms is not None:
        end = min(end, args.to_epoch_ms * 1000 - epoch_offset)
if end <= start:
    raise ValueError("No shared interval across traced CPUs")

edges = []
for tid in targets:
    cpus = Counter()
    switch_out = Counter()
    gaps = []
    previous = None
    wakes = sorted(wakeups[tid])
    parked = Counter()
    for_wake = [event[0] for event in wakes]
    for a, b, cpu, state in sorted(segments[tid]):
        a, b = max(a, start), min(b, end)
        if b <= a:
            continue
        if previous and a < previous[0]:
            raise ValueError(f"Overlapping CPU residency for TID {tid}")
        if previous:
            gaps.append((a - previous[0], previous[1]))
            wake_index = bisect_left(for_wake, previous[0])
            if previous[1] == "S" and wake_index < len(wakes):
                when, name = wakes[wake_index]
                if when <= a:
                    parked[name] += when - previous[0]
        previous = b, state
        edges.extend([(a, 1), (b, -1)])
        cpus[cpu] += b - a
        if b < end:
            switch_out[state] += 1
    print(f"TID {tid} running {sum(cpus.values()) / 1000:.2f} ms,"
          f" CPU ms {dict(sorted((k, round(v/1000, 2)) for k,v in cpus.items()))}")
    print("Switch-out states", dict(switch_out))
    print("Longest deschedules (ms, switch-out state)",
          [(round(t/1000, 2), state) for t, state in sorted(gaps, reverse=True)[:5]])
    print("Sleep until wake request (ms)", [(name, round(us/1000, 2)) for name, us in parked.most_common(5)])
    print("Top waking threads", Counter(name for t, name in wakeups[tid] if start <= t < end).most_common(5))

elapsed = Counter()
previous = start
active = 0
for timestamp, delta in sorted(edges):
    elapsed[active] += timestamp - previous
    previous = timestamp
    active += delta
elapsed[active] += end - previous
print(f"Trace duration {(end-start)/1000:.2f} ms")
for count in range(3):
    print(f"{count} threads running: {elapsed[count]/1000:.2f} ms ({elapsed[count]*100/(end-start):.2f}%)")
