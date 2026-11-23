"""Read a Cara draw-break trace and verify each requested 120-refresh window."""
import argparse
from collections import Counter
import gzip
from pathlib import Path
import re

NAMES = (
    "frame", "list", "visibility", "attachments", "shader", "primitive", "raster",
    "samplers", "common_uniforms", "light_lut", "vertex_uniforms", "layout", "payload",
    "storage", "feedback",
)
ORDER = (0, 1, 2, 14, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13)


def summarize(path, ends):
    frames = {}
    guest = {}
    open_log = gzip.open if path.suffix == ".gz" else open
    with open_log(path, "rt") as stream:
        for line in stream:
            packet = re.search(r"\bbatch frame=(\d+) offset=(\d+) masks=([0-9a-f]+)", line)
            if packet:
                frame, offset, data = packet.groups()
                masks = frames.setdefault(int(frame), [])
                if len(masks) != int(offset) or len(data) % 4:
                    raise ValueError(f"Missing or malformed chunk in refresh {frame}")
                masks.extend(int(data[i:i+4], 16) for i in range(0, len(data), 4))
            window = re.search(r"\bwindow end=(\d+) .*?\bdraws=(\d+)", line)
            if window:
                guest[int(window[1])] = int(window[2])

    totals = Counter()
    for end in ends:
        masks = Counter(m for f in range(end-120, end) for m in frames.get(f, []))
        count = sum(masks.values())
        if count != guest[end]:
            raise ValueError(f"Refresh {end}: {count} captured draws, {guest[end]} guest triggers")
        print(f"Window {end}: {count} draws, {masks[1 << 13]} storage-only transitions")
        totals.update(masks)
    first = Counter()
    for mask, count in totals.items():
        reason = next((NAMES[i] for i in ORDER if mask & (1 << i)), "identical")
        first[reason] += count
    for i in ORDER:
        matches = sum(n for mask, n in totals.items() if mask & (1 << i))
        print(f"{NAMES[i]:16} first={first[NAMES[i]]:6} any={matches:6}")


if __name__ == "__main__":
    args = argparse.ArgumentParser(description=__doc__)
    args.add_argument("log", type=Path)
    args.add_argument("ends", nargs="+", type=int)
    opts = args.parse_args()
    summarize(opts.log, opts.ends)
