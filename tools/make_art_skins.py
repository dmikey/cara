#!/usr/bin/env -S uv run --script
# /// script
# requires-python = ">=3.10"
# dependencies = ["reportlab==5.1.0", "pillow==13.0.0"]
# ///
"""Build original vector controller skins using Carbon Duo's touch and display geometry."""

import io
import json
import math
import random
from dataclasses import dataclass
from pathlib import Path
from zipfile import ZIP_DEFLATED, ZipFile, ZipInfo

from PIL import Image, ImageDraw
from reportlab.lib.colors import HexColor
from reportlab.pdfgen.canvas import Canvas

DEST = Path(__file__).resolve().parents[1] / "emulator/src/ios/skins"


@dataclass(frozen=True)
class Finish:
    name: str
    identifier: str
    rim: str
    shell: str
    face: str
    edge: str
    marking: str
    glint: str


FINISHES = (
    Finish(
        "Meadow",
        "meadow",
        "#273c45",
        "#183b36",
        "#305747",
        "#9cbd96",
        "#fffae8",
        "#d9d89f",
    ),
    Finish(
        "Rose Ink",
        "rose-ink",
        "#3f2837",
        "#4d293f",
        "#704358",
        "#dda9b5",
        "#fff4df",
        "#f4d4a1",
    ),
    Finish(
        "Silverline",
        "silverline",
        "#23252a",
        "#2e3035",
        "#44474c",
        "#909398",
        "#fffaf1",
        "#e37e4d",
    ),
    Finish(
        "Tideweave",
        "tideweave",
        "#282e2b",
        "#33403c",
        "#48463a",
        "#bcb494",
        "#fff1d6",
        "#bdac83",
    ),
    Finish(
        "Grin Garden",
        "grin-garden",
        "#172f2a",
        "#234836",
        "#325b3d",
        "#adcd65",
        "#fff3c7",
        "#c38de8",
    ),
    Finish(
        "Orbit Arcade",
        "orbit-arcade",
        "#141b36",
        "#232941",
        "#343354",
        "#94bfc9",
        "#fff0d6",
        "#df8eb2",
    ),
)


def rect(frame):
    return tuple(frame[key] for key in ("x", "y", "width", "height"))


def paint(c, color):
    c.setFillColor(HexColor(color))


def pen(c, color, width):
    c.setStrokeColor(HexColor(color))
    c.setLineWidth(width)
    c.setLineCap(1)
    c.setLineJoin(1)


def dot(c, x, y, radius, color):
    paint(c, color)
    c.circle(x, y, radius, stroke=0, fill=1)


def panel(c, x, y, w, h, radius, color):
    paint(c, color)
    c.roundRect(x, y, w, h, radius, stroke=0, fill=1)


def polygon(c, points, color):
    path = c.beginPath()
    path.moveTo(*points[0])
    for point in points[1:]:
        path.lineTo(*point)
    path.close()
    paint(c, color)
    c.drawPath(path, stroke=0, fill=1)


def gradient(c, w, h, stops, diagonal=False):
    c.linearGradient(0, 0, w if diagonal else 0, h, [HexColor(v) for v in stops])


def meadow(c, w, h):
    rng = random.Random(310)
    gradient(c, w, h, ["#79afc9", "#cce5dd", "#f7e4ba", "#a2b78a"])
    # Broken banks of backlit cumulus, with brush slivers under each bank.
    for cx, cy, scale in [
        (0.07, 0.27, 0.23),
        (0.39, 0.22, 0.14),
        (0.83, 0.32, 0.27),
        (0.61, 0.38, 0.12),
    ]:
        c.saveState()
        c.translate(w * cx, h * cy)
        c.scale(w * scale, w * scale * 0.43)
        for level, color in [(0, "#b9d5d6"), (-0.025, "#e7ece0"), (-0.043, "#f8f0dd")]:
            path = c.beginPath()
            path.moveTo(-0.65, level)
            path.curveTo(-0.54, level - 0.04, -0.44, level - 0.07, -0.31, level - 0.06)
            path.curveTo(-0.25, level - 0.16, -0.18, level - 0.19, -0.11, level - 0.15)
            path.curveTo(-0.02, level - 0.41, 0.23, level - 0.39, 0.29, level - 0.18)
            path.curveTo(0.42, level - 0.2, 0.46, level - 0.12, 0.46, level - 0.08)
            path.curveTo(0.62, level - 0.09, 0.69, level - 0.02, 0.85, level)
            path.curveTo(0.37, level + 0.024, -0.19, level + 0.035, -0.65, level)
            paint(c, color)
            c.drawPath(path, stroke=0, fill=1)
        c.restoreState()

    for y, amp, freq, phase, color in [
        (0.52, 0.05, 6, 0.8, "#83a9a4"),
        (0.57, 0.065, 6.4, 2, "#799b90"),
        (0.64, 0.062, 7.1, 3, "#9eb07f"),
        (0.67, 0.073, 5, 1, "#73916c"),
        (0.72, 0.094, 5, -0.8, "#557e63"),
        (0.80, 0.095, 5.3, 0.4, "#385f50"),
        (0.94, 0.08, 5.9, 1, "#294d44"),
    ]:

        def ridge(x, y=y, amp=amp, freq=freq, phase=phase):
            return (
                y + amp * math.sin(freq * x + phase) + 0.014 * math.sin(16 * x + 0.4)
            ) * h

        polygon(
            c,
            [(w * i / 96, ridge(i / 96)) for i in range(97)] + [(w, h), (0, h)],
            color,
        )
        for _ in range(68):
            xx = rng.random()
            yy = ridge(xx) + rng.uniform(0.007, 0.09) * h
            pen(c, rng.choice(["#c5c19a", "#9cae82", "#728e75"]), w * 0.00052)
            dx = rng.uniform(0.004, 0.018) * w
            c.bezier(
                xx * w,
                yy,
                xx * w + dx * 0.4,
                yy,
                xx * w + dx * 0.8,
                yy - dx * 0.12,
                xx * w + dx,
                yy - dx * 0.23,
            )
    # Clusters of seed heads lean into the prevailing wind.
    for bank in (0.02, 0.18, 0.74, 0.90):
        for _ in range(16):
            x = (bank + rng.uniform(-0.05, 0.07)) * w
            y = rng.uniform(0.93, 1.04) * h
            rise = rng.uniform(0.037, 0.126) * h
            lean = rise * rng.uniform(0.35, 0.77)
            color = rng.choice(["#c6be91", "#a5b597", "#859c77"])
            pen(c, color, w * 0.0007)
            c.bezier(
                x,
                y,
                x + 0.12 * lean,
                y - rise * 0.45,
                x + lean * 0.6,
                y - rise * 0.85,
                x + lean,
                y - rise,
            )
            for j in range(4):
                tx = x + lean * (1 - j * 0.075)
                ty = y - rise * (1 - j * 0.036)
                polygon(
                    c,
                    [
                        (tx, ty),
                        (tx + rise * 0.058, ty - rise * 0.048),
                        (tx + rise * 0.022, ty - rise * 0.009),
                    ],
                    color,
                )


def rose_ink(c, w, h):
    gradient(c, w, h, ["#f9e7df", "#d8aaae", "#b68192", "#e9b7b4"], True)
    u = min(w, h)
    # A fine lozenge ground with a tiny four-petal print.
    spacing = u * 0.061
    for i in range(int((w + h) / spacing) + 2):
        pen(c, "#cea3a8", u * 0.00045)
        c.line(i * spacing, 0, i * spacing - h, h)
        c.line(i * spacing - h, 0, i * spacing, h)
    for row in range(int(h / spacing) + 1):
        for col in range(int(w / spacing) + 1):
            x = (col + 0.5 * (row % 2)) * spacing
            y = row * spacing
            dot(c, x, y, u * 0.00145, "#f7ddd0")

    # Mirrored ink coils, tapered leaves, and pearl edging engraved into the ground.
    for edge in (0, w):
        c.saveState()
        c.translate(edge, 0)
        if edge:
            c.scale(-1, 1)
        length = 0.22 * u
        for j in range(math.ceil(h / length) + 1):
            cy = (j + 0.29) * length
            c.saveState()
            c.translate(0.025 * u, cy)
            c.scale(length, length)
            for offset, color, weight in [
                (0.005, "#f3cbc3", 0.018),
                (0, "#8a536b", 0.011),
            ]:
                pen(c, color, weight)
                c.bezier(
                    0,
                    offset,
                    0.67,
                    -0.46 + offset,
                    0.96,
                    0.31 + offset,
                    0.61,
                    0.32 + offset,
                )
                c.bezier(
                    0.61,
                    0.32 + offset,
                    0.37,
                    0.32 + offset,
                    0.43,
                    0.049 + offset,
                    0.58,
                    0.106 + offset,
                )
                c.bezier(
                    0.58,
                    0.106 + offset,
                    0.62,
                    0.13 + offset,
                    0.61,
                    0.18 + offset,
                    0.57,
                    0.175 + offset,
                )
                c.bezier(
                    0.17,
                    -0.075 + offset,
                    0.52,
                    -0.13 + offset,
                    0.39,
                    -0.50 + offset,
                    0.25,
                    -0.39 + offset,
                )
            for x, y, dx, dy in [
                (0.20, -0.095, 0.20, 0.11),
                (0.35, -0.12, 0.09, -0.16),
                (0.65, -0.06, 0.19, -0.075),
                (0.69, 0.28, 0.14, 0.085),
            ]:
                p = c.beginPath()
                p.moveTo(x, y)
                p.curveTo(
                    x + dx * 0.25 - dy * 0.25,
                    y + dy * 0.25 + dx * 0.25,
                    x + dx * 0.9 - dy * 0.2,
                    y + dy * 0.9 + dx * 0.2,
                    x + dx,
                    y + dy,
                )
                p.curveTo(
                    x + dx * 0.7 + dy * 0.2,
                    y + dy * 0.7 - dx * 0.2,
                    x + dx * 0.2 + dy * 0.2,
                    y + dy * 0.2 - dx * 0.2,
                    x,
                    y,
                )
                paint(c, "#a16b7a")
                c.drawPath(p, stroke=0, fill=1)
            for i in range(12):
                t = i / 11
                x = 0.18 + t * 0.60
                y = 0.43 + 0.09 * math.sin(t * math.pi)
                dot(c, x, y, 0.017, "#a76c7e")
                dot(c, x - 0.002, y - 0.003, 0.013, "#f3dcd0")
                dot(c, x - 0.005, y - 0.006, 0.006, "#fff4e8")
            c.restoreState()
        c.restoreState()


def silverline(c, w, h):
    u = min(w, h)
    gradient(c, w, h, ["#dadcdb", "#faf9f3", "#acb0b1", "#e4e4dd", "#a6a9a9"], True)
    rng = random.Random(52)
    for _ in range(1400):
        x = rng.uniform(0, w)
        y = rng.uniform(0, h)
        pen(c, rng.choice(["#c4c7c6", "#d3d4d0", "#efeee7"]), u * 0.00035)
        c.line(x, y, min(w, x + rng.uniform(0.003, 0.06) * w), y)
    band = h * 0.53
    panel(c, 0, band, w, h * 0.152, 0, "#323539")
    for i in range(15):
        pen(c, "#42464a", u * 0.0006)
        c.line(0, band + (i + 0.5) * h * 0.01, w, band + (i + 0.5) * h * 0.01)
    for y, color in [(band, "#f6f4e9"), (band + h * 0.152, "#777b7e")]:
        pen(c, color, u * 0.0014)
        c.line(0, y, w, y)
    for x in (0.09 * w, 0.82 * w):
        for i in range(8):
            xx = x + i * 0.011 * u
            yy = 0.86 * h
            panel(c, xx, yy, u * 0.0045, u * 0.048, u * 0.0022, "#f7f5ec")
            panel(c, xx, yy - u * 0.001, u * 0.0045, u * 0.046, u * 0.0022, "#595e63")
    for x in (0.018 * w, 0.982 * w):
        for y in (0.018 * h, 0.98 * h):
            dot(c, x, y, u * 0.0058, "#f7f6ed")
            dot(c, x, y - u * 0.0008, u * 0.005, "#7b8081")
            pen(c, "#404548", u * 0.00125)
            c.line(x - u * 0.0023, y + u * 0.0023, x + u * 0.0023, y - u * 0.0023)


def label(c, title, x, y, size, color):
    c.saveState()
    c.translate(x, y)
    c.scale(1, -1)
    paint(c, color)
    c.setFont("Helvetica-Bold", size)
    c.drawCentredString(0, -size * 0.33, title)
    c.restoreState()


def control(c, item, f):
    x, y, w, h = rect(item["frame"])
    cx = x + w / 2
    cy = y + h / 2
    keys = item["inputs"]
    if isinstance(keys, dict):
        if keys.get("x") == "touchScreenX":
            return
        if "up" in keys:
            # A single connected cross keeps the center clear of overlapping bevels.
            for inset, color in [(0, f.rim), (0.025, f.edge), (0.044, f.face)]:
                lo, hi = inset, 1 - inset
                a, b = 0.337 + inset / 2, 0.663 - inset / 2
                xy = [
                    (a, lo),
                    (b, lo),
                    (b, a),
                    (hi, a),
                    (hi, b),
                    (b, b),
                    (b, hi),
                    (a, hi),
                    (a, b),
                    (lo, b),
                    (lo, a),
                    (a, a),
                ]
                polygon(c, [(x + w * px, y + h * py) for px, py in xy], color)
            for angle in (0, math.pi / 2, math.pi, math.pi * 1.5):

                def arrow(px, py, angle=angle):
                    return (
                        cx + w * (px * math.cos(angle) - py * math.sin(angle)),
                        cy + h * (px * math.sin(angle) + py * math.cos(angle)),
                    )

                polygon(
                    c,
                    [arrow(0, -0.395), arrow(-0.045, -0.322), arrow(0.045, -0.322)],
                    f.marking,
                )
            dot(c, cx, cy, w * 0.068, f.shell)
        else:
            for scale, color in [
                (0.53, f.rim),
                (0.50, f.edge),
                (0.48, f.face),
                (0.42, f.shell),
                (0.31, f.rim),
            ]:
                dot(c, cx, cy, w * scale, color)
        return
    title = keys[0].upper()
    if title in ("X", "Y", "A", "B"):
        for radius, color in [(0.52, f.rim), (0.48, f.edge), (0.455, f.face)]:
            dot(c, cx, cy, w * radius, color)
        label(c, title, cx, cy, w * 0.49, f.marking)
    else:
        for inset, color in [(0, f.rim), (0.032, f.edge), (0.054, f.face)]:
            d = h * inset
            panel(c, x + d, y + d, w - 2 * d, h - 2 * d, h * 0.25, color)
        label(c, title, cx, cy, min(h * 0.44, w / (len(title) * 0.84)), f.marking)


def tideweave(c, w, h):
    repeat = min(w, h) * 0.24
    yarns = (
        (0.45, "#008e98"),
        (0.055, "#006b78"),
        (0.035, "#a2d8c7"),
        (0.045, "#40302b"),
        (0.265, "#331e1a"),
        (0.055, "#654a37"),
        (0.028, "#ffe9bd"),
        (0.017, "#473631"),
        (0.020, "#ffe9bd"),
        (0.030, "#006b78"),
    )

    def mixed(a, b):
        if a == b:
            return a
        if "#ffe9bd" in (a, b):
            return "#efd9af"
        aa, bb = bytes.fromhex(a[1:]), bytes.fromhex(b[1:])
        return "#" + bytes((x + y) // 2 for x, y in zip(aa, bb)).hex()

    # Draw the weave repeat once. Reusing a PDF form keeps the installation
    # small and lets the GPU scale the same vector pattern on each device.
    c.beginForm("weave", 0, 0, repeat, repeat)
    x = 0
    for x_share, warp in yarns:
        y = 0
        for y_share, weft in yarns:
            paint(c, mixed(warp, weft))
            c.rect(
                x, y, x_share * repeat + 0.06, y_share * repeat + 0.06, stroke=0, fill=1
            )
            y += y_share * repeat
        x += x_share * repeat
    c.endForm()
    for row in range(math.ceil(h / repeat) + 1):
        for col in range(math.ceil(w / repeat) + 1):
            c.saveState()
            c.translate((col - 0.15) * repeat, (row - 0.12) * repeat)
            c.doForm("weave")
            c.restoreState()

    # Sparse directional highlights suggest the twill without covering the checks.
    thread = repeat * 0.023
    c.saveState()
    pen(c, "#d2f4de", thread * 0.14)
    c.setStrokeAlpha(0.18)
    for i in range(math.ceil((w + h) / thread) + 2):
        x = -h + i * thread
        c.line(x, h, x + h, 0)
    c.restoreState()


def silhouette(c, start, curves, fill, outline, width):
    path = c.beginPath()
    path.moveTo(*start)
    for segment in curves:
        path.curveTo(*segment)
    path.close()
    paint(c, fill)
    pen(c, outline, width)
    c.drawPath(path, stroke=1, fill=1)


def eye(c, x, y, r, dx, dy):
    for rr, color in [(1.06, "#172c27"), (0.92, "#ffefbd")]:
        dot(c, x, y, r * rr, color)
    dot(c, x + dx * r, y + dy * r, r * 0.49, "#1d322c")
    dot(c, x + (dx - 0.16) * r, y + (dy - 0.18) * r, r * 0.14, "#fffdf2")


def creature(c, x, y, size, tilt, style):
    c.saveState()
    c.translate(x, y)
    c.rotate(tilt)
    c.scale(size, size)
    ink = "#142e29"
    body = ("#ade542", "#4be696", "#d6ee39")[style]
    # Each character has a different silhouette, eye arrangement and expression.
    if style == 0:
        silhouette(
            c,
            (-0.66, -0.13),
            [
                (-0.81, -0.66, -0.48, -0.76, -0.32, -0.54),
                (-0.15, -0.87, 0.31, -0.81, 0.29, -0.51),
                (0.73, -0.78, 0.83, -0.48, 0.65, -0.18),
                (0.93, 0.08, 0.56, 0.61, 0.12, 0.65),
                (-0.28, 0.75, -0.75, 0.44, -0.66, -0.13),
            ],
            body,
            ink,
            0.047,
        )
        eye(c, -0.26, -0.31, 0.24, 0.17, -0.05)
        eye(c, 0.24, -0.32, 0.19, -0.17, 0.06)
    elif style == 1:
        silhouette(
            c,
            (-0.64, -0.11),
            [
                (-0.73, -0.36, -0.56, -0.47, -0.43, -0.47),
                (-0.64, -1.04, -0.28, -1.02, -0.21, -0.56),
                (0.08, -0.6, 0.24, -0.44, 0.39, -0.47),
                (0.67, -0.85, 0.84, -0.66, 0.64, -0.29),
                (0.99, 0.27, 0.5, 0.57, 0.18, 0.61),
                (-0.25, 0.72, -0.83, 0.52, -0.64, -0.11),
            ],
            body,
            ink,
            0.047,
        )
        eye(c, 0.03, -0.24, 0.33, -0.09, 0.17)
    else:
        silhouette(
            c,
            (-0.67, -0.18),
            [
                (-0.82, -0.66, -0.6, -0.69, -0.36, -0.51),
                (-0.29, -0.91, -0.04, -0.94, 0.03, -0.56),
                (0.43, -0.85, 0.66, -0.71, 0.61, -0.46),
                (0.96, -0.57, 0.82, -0.1, 0.69, 0.06),
                (0.66, 0.66, 0.15, 0.76, -0.21, 0.59),
                (-0.70, 0.65, -0.92, 0.29, -0.67, -0.18),
            ],
            body,
            ink,
            0.047,
        )
        for ex, ey, er, dx in [
            (-0.37, -0.27, 0.172, 0.27),
            (0, -0.36, 0.181, -0.03),
            (0.35, -0.25, 0.168, -0.28),
        ]:
            eye(c, ex, ey, er, dx, 0.15)
    silhouette(
        c,
        (-0.47, 0.07),
        [
            (-0.20, 0.15, 0.15, 0.12, 0.48, -0.05),
            (0.52, 0.24, 0.32, 0.50, 0.05, 0.48),
            (-0.26, 0.49, -0.44, 0.29, -0.47, 0.07),
        ],
        "#2b2330",
        ink,
        0.026,
    )
    for i in range(6):
        tx = -0.41 + i * 0.145
        ty = 0.105 - 0.135 * ((tx + 0.03) / 0.46) ** 2
        polygon(
            c,
            [
                (tx, ty),
                (tx + 0.121, ty - 0.014),
                (tx + 0.076, ty + (0.123 if i % 2 else 0.158)),
            ],
            "#fff2be",
        )
    silhouette(
        c,
        (0.11, 0.30),
        [
            (0.20, 0.25, 0.27, 0.28, 0.31, 0.36),
            (0.37, 0.47, 0.52, 0.49, 0.50, 0.57),
            (0.44, 0.74, 0.17, 0.60, 0.11, 0.48),
            (0.06, 0.41, 0.05, 0.34, 0.11, 0.30),
        ],
        "#b975ee",
        ink,
        0.024,
    )
    pen(c, "#733f9b", 0.016)
    c.bezier(0.15, 0.35, 0.19, 0.40, 0.22, 0.48, 0.31, 0.51)
    for xx, yy in [(-0.50, -0.012), (-0.55, 0.05), (0.55, -0.06)]:
        dot(c, xx, yy, 0.022, "#557638")
    c.restoreState()


def grin_garden(c, w, h):
    u = min(w, h)
    rng = random.Random(721)
    gradient(c, w, h, ["#307b54", "#3b9757", "#68a24e"], True)
    # Lime bursts, pods and pairs of single leaves form the smaller scale print.
    step = 0.118 * u
    for row in range(math.ceil(h / step) + 1):
        for col in range(math.ceil(w / step) + 1):
            x = (col + 0.47 * (row % 2) + rng.uniform(-0.1, 0.1)) * step
            y = (row + rng.uniform(-0.17, 0.17)) * step
            c.saveState()
            c.translate(x, y)
            c.rotate(rng.randrange(360))
            if (row + col) % 3 == 0:
                points = [
                    (
                        math.cos(i * math.pi / 6)
                        * (0.023 if i % 2 == 0 else 0.008)
                        * u,
                        math.sin(i * math.pi / 6)
                        * (0.023 if i % 2 == 0 else 0.008)
                        * u,
                    )
                    for i in range(12)
                ]
                polygon(c, points, "#bee04e")
            else:
                pen(c, "#285b40", u * 0.0024)
                c.bezier(0, 0, -0.03 * u, -0.016 * u, -0.009 * u, -0.029 * u, 0, 0)
                c.bezier(0, 0, 0.004 * u, -0.026 * u, 0.022 * u, -0.012 * u, 0, 0)
                dot(c, 0.014 * u, 0.011 * u, 0.003 * u, "#a7d155")
            c.restoreState()
    # The print continues between the screens and beneath the controls, with
    # larger faces centered on the exposed gaps between the two control banks.
    positions = [
        (0.081, 0.225, 0.111, -15, 0),
        (0.919, 0.214, 0.115, 16, 1),
        (0.204, 0.587, 0.111, 17, 2),
        (0.805, 0.566, 0.111, -12, 0),
        (0.083, 0.918, 0.129, -11, 1),
        (0.929, 0.901, 0.126, 8, 2),
        (0.287, 0.783, 0.091, -23, 1),
        (0.716, 0.756, 0.094, 15, 2),
        (0.495, 0.930, 0.105, 13, 0),
    ]
    if h > w:
        positions = [
            (0.076, 0.201, 0.135, -18, 0),
            (0.917, 0.26, 0.13, 14, 2),
            (0.121, 0.52, 0.14, 16, 1),
            (0.888, 0.513, 0.133, -17, 0),
            (0.505, 0.735, 0.162, -9, 2),
            (0.488, 0.889, 0.149, 11, 0),
            (0.129, 0.945, 0.111, -18, 2),
            (0.88, 0.946, 0.123, 20, 1),
        ]
    for x, y, s, a, style in positions:
        creature(c, x * w, y * h, s * u, a, style)


def planet(c, x, y, r, tilt, kind):
    c.saveState()
    c.translate(x, y)
    c.rotate(tilt)
    c.scale(r, r)
    outline = "#121d3d"
    colors = [
        ("#f6ab53", "#ed646e", "#ffdd8c"),
        ("#8ddad0", "#4fb2cd", "#ecf1c4"),
        ("#cc8bed", "#886edc", "#f8c5dd"),
    ]
    base, band, bright = colors[kind]
    dot(c, 0, 0, 1.05, outline)
    dot(c, 0, 0, 1, base)
    c.saveState()
    shape = c.beginPath()
    shape.circle(0, 0, 0.985)
    c.clipPath(shape, stroke=0)
    for y0 in [-0.71, -0.43, 0.08, 0.47]:
        pen(c, band, 0.10 if y0 < 0 else 0.19)
        c.bezier(-1.1, y0, -0.41, y0 + 0.34, 0.42, y0 - 0.19, 1.1, y0 + 0.01)
    c.restoreState()
    if kind != 1:
        pen(c, outline, 0.245)
        c.bezier(-0.9, -0.42, -2.54, -0.21, -0.06, 1.27, 1.55, 0.80)
        c.bezier(1.55, 0.80, 1.92, 0.67, 1.32, 0.42, 0.94, 0.36)
        pen(c, bright, 0.139)
        c.bezier(-0.9, -0.42, -2.54, -0.21, -0.06, 1.27, 1.55, 0.80)
        c.bezier(1.55, 0.80, 1.92, 0.67, 1.32, 0.42, 0.94, 0.36)
    else:
        for xx, yy, rr in [
            (-0.39, 0.27, 0.18),
            (0.35, -0.42, 0.13),
            (0.46, 0.52, 0.08),
        ]:
            dot(c, xx, yy, rr, band)
            dot(c, xx - 0.022, yy + 0.02, rr * 0.71, base)
    c.restoreState()


def rocket(c, x, y, r, tilt):
    c.saveState()
    c.translate(x, y)
    c.rotate(tilt)
    c.scale(r, r)
    outline = "#182346"
    silhouette(
        c,
        (-0.19, 0.48),
        [
            (-0.40, 0.90, -0.15, 1.23, -0.08, 1.47),
            (0.19, 1.14, 0.30, 0.87, 0.17, 0.45),
            (0.09, 0.37, -0.10, 0.37, -0.19, 0.48),
        ],
        "#ff9a52",
        outline,
        0.04,
    )
    polygon(c, [(-0.093, 0.48), (0.083, 0.49), (-0.045, 1.12)], "#fff19d")
    silhouette(
        c,
        (-0.22, -0.03),
        [
            (-0.57, 0.17, -0.68, 0.55, -0.61, 0.72),
            (-0.44, 0.57, -0.31, 0.47, -0.15, 0.48),
            (-0.02, 0.28, -0.08, 0.17, -0.22, -0.03),
        ],
        "#f47eac",
        outline,
        0.048,
    )
    silhouette(
        c,
        (0.22, -0.03),
        [
            (0.57, 0.17, 0.68, 0.55, 0.61, 0.72),
            (0.44, 0.57, 0.31, 0.47, 0.15, 0.48),
            (0.02, 0.28, 0.08, 0.17, 0.22, -0.03),
        ],
        "#f47eac",
        outline,
        0.048,
    )
    silhouette(
        c,
        (0, -0.93),
        [
            (-0.40, -0.57, -0.42, 0.12, -0.21, 0.48),
            (-0.11, 0.56, 0.10, 0.56, 0.21, 0.48),
            (0.43, 0.05, 0.39, -0.58, 0, -0.93),
        ],
        "#f2edc8",
        outline,
        0.054,
    )
    silhouette(
        c,
        (0, -0.92),
        [
            (-0.15, -0.77, -0.22, -0.60, -0.26, -0.44),
            (-0.11, -0.38, 0.13, -0.38, 0.26, -0.44),
            (0.21, -0.61, 0.12, -0.81, 0, -0.92),
        ],
        "#ec6585",
        outline,
        0.031,
    )
    dot(c, 0, -0.09, 0.215, outline)
    dot(c, 0, -0.09, 0.157, "#6adace")
    dot(c, -0.049, -0.137, 0.05, "#d9ffec")
    c.restoreState()


def orbit_arcade(c, w, h):
    u = min(w, h)
    rng = random.Random(110)
    gradient(c, w, h, ["#11213f", "#1d2145", "#25264b"], True)
    for _ in range(round(w * h / (u * u) * 210)):
        x = rng.random() * w
        y = rng.random() * h
        radius = rng.uniform(0.00085, 0.0026) * u
        color = rng.choice(["#acc6cf", "#d9b4c8", "#98a8d3", "#f4dfb0"])
        dot(c, x, y, radius, color)
    for _ in range(round(w * h / (u * u) * 51)):
        x = rng.random() * w
        y = rng.random() * h
        r = rng.uniform(0.004, 0.012) * u
        points = [
            (
                x + r * math.cos(i * math.pi / 4) * (0.24 if i % 2 else 1),
                y + r * math.sin(i * math.pi / 4) * (0.24 if i % 2 else 1),
            )
            for i in range(8)
        ]
        polygon(c, points, rng.choice(["#edd586", "#ed9cc1", "#7ddbd9"]))
    # Diagonal comet marks use rounded tails and a double nucleus.
    for xx, yy in [
        (0.06, 0.57),
        (0.95, 0.60),
        (0.22, 0.88),
        (0.76, 0.87),
        (0.11, 0.29),
        (0.92, 0.28),
        (0.50, 0.76),
    ]:
        x = xx * w
        y = yy * h
        r = 0.018 * u
        for dx, dy, color in [
            (0, 0, "#6cbce0"),
            (0.5, 0.38, "#b68dd7"),
            (-0.32, -0.31, "#eb989e"),
        ]:
            pen(c, color, r * 0.17)
            c.line(x + r * dx, y + r * dy, x + r * (dx + 2.15), y + r * (dy - 1.65))
        dot(c, x, y, r * 0.49, "#89d3d5")
        dot(c, x - r * 0.08, y - r * 0.07, r * 0.20, "#fff2b9")
    if w > h:
        worlds = [
            (0.191, 0.383, 0.045, -18, 0),
            (0.188, 0.594, 0.064, -21, 1),
            (0.810, 0.572, 0.067, 12, 2),
            (0.079, 0.922, 0.067, -12, 0),
            (0.717, 0.810, 0.047, 17, 1),
            (0.914, 0.248, 0.069, -27, 0),
        ]
        rockets = [
            (0.089, 0.167, 0.072, -32),
            (0.918, 0.90, 0.090, 35),
            (0.260, 0.795, 0.059, 40),
        ]
    else:
        worlds = [
            (0.088, 0.22, 0.074, -19, 0),
            (0.888, 0.253, 0.072, 15, 2),
            (0.114, 0.509, 0.091, -21, 2),
            (0.908, 0.535, 0.072, -23, 0),
            (0.491, 0.838, 0.088, 15, 0),
            (0.881, 0.957, 0.074, -14, 1),
        ]
        rockets = [
            (0.49, 0.713, 0.094, 34),
            (0.10, 0.932, 0.09, -20),
            (0.928, 0.197, 0.059, 22),
        ]
    for x, y, r, a, kind in worlds:
        planet(c, x * w, y * h, r * u, a, kind)
    for x, y, r, a in rockets:
        rocket(c, x * w, y * h, r * u, a)


def artwork(f, rep, title):
    w, h = rep["mappingSize"]["width"], rep["mappingSize"]["height"]
    u = min(w, h)
    out = io.BytesIO()
    c = Canvas(out, pagesize=(w, h), invariant=1, pageCompression=1)
    c.setTitle(f"{f.name} — {title}")
    c.setAuthor("Cara")
    c.translate(0, h)
    c.scale(1, -1)
    holes = [rect(screen["outputFrame"]) for screen in rep["screens"]]
    clip = c.beginPath()
    clip.rect(0, 0, w, h)
    for x, y, sw, sh in holes:
        clip.rect(x, y, sw, sh)
    c.clipPath(
        clip, stroke=0, fill=0, fillMode=0
    )  # Even-odd: both displays remain transparent.
    {
        "meadow": meadow,
        "rose-ink": rose_ink,
        "silverline": silverline,
        "tideweave": tideweave,
        "grin-garden": grin_garden,
        "orbit-arcade": orbit_arcade,
    }[f.identifier](c, w, h)
    # The OS draws the metrics, status indicator and clock over this top rail.
    _, sy, _, sh = rect(rep["caraStatsFrame"])
    rail = sy + sh + u * 0.010
    panel(c, 0, 0, w, rail, 0, f.rim)
    panel(c, 0, rail, w, u * 0.0016, 0, f.glint)
    for x, y, sw, sh in holes:
        pen(c, f.rim, u * 0.008)
        c.rect(x, y, sw, sh, stroke=1, fill=0)
    for item in rep["items"]:
        control(c, item, f)
    c.showPage()
    c.save()
    return out.getvalue()


def thumbstick(f):
    # Supersample the concentric rubber cap so it holds its shape on Retina screens.
    image = Image.new("RGBA", (512, 512))
    draw = ImageDraw.Draw(image)
    for inset, color in [
        (8, f.rim),
        (13, f.edge),
        (20, f.face),
        (49, f.shell),
        (55, f.face),
    ]:
        draw.ellipse((inset, inset, 511 - inset, 511 - inset), fill=color)
    draw.arc((61, 61, 450, 450), 202, 283, fill=f.edge, width=3)
    out = io.BytesIO()
    image.resize((256, 256), Image.Resampling.LANCZOS).save(out, format="PNG")
    return out.getvalue()


def build(f):
    info = json.loads((DEST / "carbon-duo.json").read_text())
    info.update(name=f.name, identifier=f"com.dmikey.cara.{f.identifier}.v1")
    files = {"thumbstick.png": thumbstick(f)}
    for device, representations in info["representations"].items():
        for orientation, rep in representations["standard"].items():
            name = f"{f.identifier}-{device}-{orientation}.pdf"
            rep["assets"]["resizable"] = name
            files[name] = artwork(f, rep, f"{device} {orientation}")
    files["info.json"] = (json.dumps(info, indent=2) + "\n").encode()
    archive = DEST / (f.name.replace(" ", "") + ".deltaskin")
    with ZipFile(archive, "w") as package:
        for name, data in sorted(files.items()):
            entry = ZipInfo(name, (2026, 1, 1, 0, 0, 0))
            entry.compress_type = ZIP_DEFLATED
            entry.external_attr = 0o100644 << 16
            package.writestr(entry, data)
    print(archive.name)


if __name__ == "__main__":
    for finish in FINISHES:
        build(finish)
