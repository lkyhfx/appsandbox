#!/usr/bin/env python3
"""Summarize ASB_C0 kernel logs by the last complete workload matrix."""

from __future__ import annotations

import re
import sys
from collections import Counter
from pathlib import Path


LINE_RE = re.compile(r"^\[\s*(?P<time>\d+\.\d+)\]\s+(?P<body>.*)$")
FIELD_RE = re.compile(r"(?P<key>[a-zA-Z0-9_]+)=(?P<value>\S+)")
WORKLOADS = ("IDLE", "WINDOW_DRAG", "GLX", "VULKAN", "VIDEO")


def parse(path: Path):
    samples = []
    markers = []
    for line in path.read_text(encoding="utf-8", errors="replace").splitlines():
        match = LINE_RE.match(line)
        if not match:
            continue
        timestamp = float(match.group("time"))
        body = match.group("body")
        if "ASB_C0_WORKLOAD " in body:
            markers.append((timestamp, body.split("ASB_C0_WORKLOAD ", 1)[1].strip()))
        elif "ASB_C0 seq=" in body:
            fields = {m.group("key"): m.group("value") for m in FIELD_RE.finditer(body)}
            fields["time"] = timestamp
            fields["seq"] = int(fields["seq"])
            samples.append(fields)
    return samples, markers


def last_matrix(markers):
    starts = [i for i, (_, name) in enumerate(markers) if name == "IDLE_START"]
    if not starts:
        raise SystemExit("no IDLE_START marker")
    selected = markers[starts[-1] :]
    if not any(name == "MATRIX_END" for _, name in selected):
        raise SystemExit("last workload matrix is incomplete")
    return selected


def boundary(markers, name):
    return next(timestamp for timestamp, marker in markers if marker == name)


def prior_seq(samples, timestamp):
    prior = [sample["seq"] for sample in samples if sample["time"] <= timestamp]
    return max(prior) if prior else None


def main() -> int:
    if len(sys.argv) != 2:
        print(f"usage: {Path(sys.argv[0]).name} <gate-c0-kernel.log>", file=sys.stderr)
        return 2

    samples, markers = parse(Path(sys.argv[1]))
    matrix = last_matrix(markers)
    print("| Workload | Approx. atomic updates | Samples | Provenance | GEM objects | FB IDs | Identity changed |")
    print("|---|---:|---:|---|---:|---:|---:|")

    matrix_samples = []
    for workload in WORKLOADS:
        start = boundary(matrix, workload + "_START")
        end = boundary(matrix, workload + "_END")
        rows = [sample for sample in samples if start <= sample["time"] <= end]
        matrix_samples.extend(rows)
        start_seq = prior_seq(samples, start)
        end_seq = prior_seq(samples, end)
        approximate_updates = "n/a" if start_seq is None or end_seq is None else str(end_seq - start_seq)
        provenance = ", ".join(f"{key}:{value}" for key, value in sorted(
            Counter(row.get("provenance", "UNKNOWN") for row in rows).items()))
        objects = len({row.get("gem_obj") for row in rows if row.get("gem_obj")})
        fb_ids = len({row.get("fb_id") for row in rows if row.get("fb_id")})
        changed = sum(row.get("identity_changed") == "yes" for row in rows)
        print(f"| {workload} | {approximate_updates} | {len(rows)} | {provenance or '-'} | {objects} | {fb_ids} | {changed} |")

    imported = sum(row.get("provenance") == "IMPORTED_DMABUF" for row in matrix_samples)
    local = sum(row.get("provenance") == "LOCAL" for row in matrix_samples)
    funcs = sorted({row.get("gem_funcs", "-") for row in matrix_samples})
    exporters = sorted({row.get("exp_name", "-") for row in matrix_samples})
    print()
    print(f"matrix_samples={len(matrix_samples)} local_samples={local} imported_samples={imported}")
    print(f"gem_funcs={','.join(funcs)} exporters={','.join(exporters)}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
