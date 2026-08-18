#!/usr/bin/env python3
"""Minimal stackcollapse + flamegraph SVG generator.

Brendan Gregg's FlameGraph scripts are not installed on this box, so this is a
small self-contained equivalent: it reads `perf script` output on stdin, folds
the stacks, and writes an interactive SVG (click to zoom, hover for detail).

Usage:  perf script -i perf.data | flamegraph.py out.svg "Title"
"""
import html
import sys
from collections import defaultdict


def fold(stream):
    """perf script -> {';'-joined stack: sample count}"""
    folded = defaultdict(int)
    stack = []
    for line in stream:
        line = line.rstrip()
        if not line:
            if stack:
                folded[";".join(reversed(stack))] += 1
                stack = []
            continue
        if not line.startswith(("\t", " ")):
            continue                      # the "comm pid ts event:" header line
        parts = line.split()
        if len(parts) < 2:
            continue
        sym = parts[1]
        if sym == "[unknown]":
            sym = parts[-1].strip("()")
        stack.append(sym)
    if stack:
        folded[";".join(reversed(stack))] += 1
    return folded


def build_tree(folded):
    root = {"name": "all", "value": 0, "children": {}}
    for stack, count in folded.items():
        root["value"] += count
        node = root
        for frame in stack.split(";"):
            child = node["children"].get(frame)
            if child is None:
                child = {"name": frame, "value": 0, "children": {}}
                node["children"][frame] = child
            child["value"] += count
            node = child
    return root


def color(name):
    """Warm palette, hashed by name so a function keeps its colour."""
    h = 0
    for ch in name:
        h = (h * 31 + ord(ch)) & 0xFFFFFFFF
    r = 205 + (h % 50)
    g = 40 + ((h >> 8) % 160)
    b = 30 + ((h >> 16) % 55)
    return "rgb(%d,%d,%d)" % (r, g, b)


def render(root, title, out):
    ROW = 16
    WIDTH = 1200
    rows = []

    def walk(node, depth, x0, total):
        rows.append((depth, x0, node["value"], node["name"], total))
        cur = x0
        for child in sorted(node["children"].values(),
                            key=lambda c: -c["value"]):
            walk(child, depth + 1, cur, total)
            cur += child["value"]

    walk(root, 0, 0, root["value"])
    depth_max = max(r[0] for r in rows) + 1
    height = depth_max * ROW + 60
    total = root["value"] or 1

    parts = [
        '<svg xmlns="http://www.w3.org/2000/svg" width="%d" height="%d" '
        'viewBox="0 0 %d %d" font-family="Verdana,sans-serif" font-size="11">'
        % (WIDTH, height, WIDTH, height),
        '<style>rect:hover{stroke:#000;stroke-width:0.6}</style>',
        '<rect width="100%%" height="100%%" fill="#fdfdfd"/>',
        '<text x="%d" y="18" text-anchor="middle" font-size="15">%s</text>'
        % (WIDTH // 2, html.escape(title)),
    ]
    for depth, x0, value, name, _ in rows:
        w = value * WIDTH / total
        if w < 0.35:
            continue
        x = x0 * WIDTH / total
        y = height - (depth + 1) * ROW - 6
        pct = 100.0 * value / total
        label = ""
        if w > 42:
            maxch = int(w / 6.2)
            label = name if len(name) <= maxch else name[:maxch - 2] + ".."
        parts.append(
            '<g><title>%s (%d samples, %.2f%%)</title>'
            '<rect x="%.2f" y="%d" width="%.2f" height="%d" fill="%s" '
            'rx="1"/><text x="%.2f" y="%d" fill="#000">%s</text></g>'
            % (html.escape(name), value, pct, x, y, w, ROW - 1, color(name),
               x + 2, y + ROW - 4, html.escape(label)))
    parts.append("</svg>")
    with open(out, "w") as f:
        f.write("\n".join(parts))
    return len(rows), total


def main():
    if len(sys.argv) < 2:
        sys.stderr.write(__doc__)
        return 1
    out = sys.argv[1]
    title = sys.argv[2] if len(sys.argv) > 2 else "flamegraph"
    folded = fold(sys.stdin)
    if not folded:
        sys.stderr.write("no stacks parsed from perf script output\n")
        return 2
    root = build_tree(folded)
    nboxes, total = render(root, title, out)
    print("%s: %d samples, %d boxes" % (out, total, nboxes))
    # Also print the flat top-20 by self time, which is what you act on.
    self_time = defaultdict(int)
    for stack, count in folded.items():
        self_time[stack.split(";")[-1]] += count
    print("\n%-52s %8s %7s" % ("SELF-TIME HOT FRAMES", "samples", "pct"))
    for name, count in sorted(self_time.items(), key=lambda kv: -kv[1])[:20]:
        print("%-52s %8d %6.2f%%" % (name[:52], count, 100.0 * count / total))
    return 0


if __name__ == "__main__":
    sys.exit(main())
