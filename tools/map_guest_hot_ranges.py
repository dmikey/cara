#!/usr/bin/env python3
"""Map resident FastInterp census blocks to caller-supplied address ranges.

No disassembly, guest execution, ROM extraction, or CPU-time inference.
An optional private decoded image verifies captured block bytes using explicit
virtual-address/file-offset segments. Output contains no instruction bytes.
"""
import argparse
import hashlib
import json
from pathlib import Path


def numbers(value, count):
    parts = value.split(":")
    if len(parts) != count:
        raise ValueError(f"expected {count} colon-separated integers: {value}")
    result = tuple(int(p, 0) for p in parts)
    if any(n < 0 for n in result):
        raise ValueError("addresses, offsets and lengths must be nonnegative")
    return result


def parse_range(value):
    name, bounds = value.split("=", 1)
    start, end = numbers(bounds, 2)
    if not name or start >= end or end > 1 << 32:
        raise ValueError("range requires a name and nonempty 32-bit bounds")
    return name, start, end


def image_bytes(image, segments, address, size):
    result = bytearray()
    while size:
        matches = [s for s in segments if s[0] <= address < s[0] + s[2]]
        if len(matches) != 1:
            return None
        base, offset, length = matches[0]
        take = min(size, base + length - address)
        position = offset + address - base
        chunk = image[position:position + take]
        if len(chunk) != take:
            return None
        result.extend(chunk)
        address += take
        size -= take
    return bytes(result)


def summarize(census, ranges, image=None, segments=()):
    if census.get("kind") != "resident_block_census_not_time_samples":
        raise ValueError("unsupported census kind; refusing to relabel unknown samples")
    output = {
        "kind": census["kind"], "frame": census.get("frame"),
        "range_assignment": "block_start_in_half_open_range; overlapping ranges are independent",
        "limitations": ["retained decoded entries only; evicted counts unavailable",
                        "input blocks may be truncated to the hottest entries",
                        "executions count retained block entries, not individual instructions",
                        "counts are not CPU time or complete function invocation totals"],
        "image_sha256": hashlib.sha256(image).hexdigest() if image is not None else None,
        "verification": {"matched": 0, "mismatched": 0, "unmapped": 0,
                         "source_unavailable": 0, "not_requested": 0},
        "cores": [],
    }
    for core in census["cores"]:
        blocks = []
        for block in core["blocks"]:
            address, end, executions = (block[k] for k in ("pc", "end_pc", "executions"))
            if not 0 <= address <= end <= 1 << 32 or executions < 0:
                raise ValueError("invalid census block bounds or execution count")
            source = block.get("source_bytes")
            status = "not_requested"
            if image is not None:
                if not source:
                    status = "source_unavailable"
                else:
                    captured = bytes(source)
                    if len(captured) > end - address:
                        raise ValueError("captured bytes extend beyond block end")
                    actual = image_bytes(image, segments, address, len(captured))
                    status = "unmapped" if actual is None else (
                        "matched" if actual == captured else "mismatched")
            output["verification"][status] += 1
            blocks.append({"pc": address, "end_pc": end,
                           "executions": executions, "thumb": block["thumb"],
                           "source_verification": status})
        grouped = []
        for name, start, end in ranges:
            selected = [b for b in blocks if start <= b["pc"] < end]
            grouped.append({"name": name, "start": start, "end_exclusive": end,
                            "retained_block_executions": sum(b["executions"] for b in selected),
                            "blocks": selected})
        output["cores"].append({"core": core["core"],
                                "resident_entries": core.get("resident_entries"),
                                "input_block_count": len(blocks), "ranges": grouped})
    return output


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("census", type=Path)
    parser.add_argument("--range", action="append", required=True, type=parse_range)
    parser.add_argument("--image", type=Path, help="private decoded image; never executed")
    parser.add_argument("--segment", action="append", default=[],
                        help="virtual_address:file_offset:byte_length (hex accepted)")
    args = parser.parse_args()
    if bool(args.image) != bool(args.segment):
        parser.error("--image and --segment must be provided together")
    try:
        segments = [numbers(s, 3) for s in args.segment]
        segments.sort()
        for i, (base, offset, length) in enumerate(segments):
            if not length or base + length > 1 << 32:
                raise ValueError("segment must have nonempty 32-bit virtual bounds")
            if i and segments[i - 1][0] + segments[i - 1][2] > base:
                raise ValueError("overlapping virtual segments are ambiguous")
        result = summarize(json.loads(args.census.read_text()), args.range,
                           args.image.read_bytes() if args.image else None, segments)
    except (ValueError, KeyError, TypeError) as error:
        parser.error(str(error))
    print(json.dumps(result, indent=2))
    if result["verification"]["mismatched"] or result["verification"]["unmapped"]:
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
