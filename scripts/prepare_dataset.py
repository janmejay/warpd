#!/usr/bin/env python3
"""
Build a labeled next-click training dataset from warpd's events.jsonl.

Joins each click/drag_press/copy_and_exit event back to the most recent
hint_present pool (matched by session + screen.uuid + seq) so that for every
click we have both the chosen target and the candidate pool the user chose
from. Coordinates are normalized to the screen they landed on.

Usage:
    prepare_dataset.py [events.jsonl] [-o out.parquet|out.jsonl]

If no input path is given, defaults to ~/.local/share/warpd/events.jsonl.
Output format is inferred from the suffix; jsonl is always supported,
parquet requires pyarrow.
"""

from __future__ import annotations

import argparse
import json
import os
import sys
from collections import defaultdict
from dataclasses import dataclass, field, asdict
from pathlib import Path
from typing import Any, Iterator


SCHEMA_VERSION = 1
TARGET_TYPES = {"click", "drag_press", "copy_and_exit"}


@dataclass
class HintPool:
    session: str
    screen_uuid: str
    seq: int
    signature: str
    viewport_w: int
    viewport_h: int
    hints: list[dict[str, Any]]


@dataclass
class Sample:
    session: str
    seq: int
    t_us: int
    wall_ms: int
    event_type: str
    mode: str
    mode_trace: str
    bundle_id: str
    app_name: str
    screen_uuid: str
    screen_index: int
    screen_total: int
    screen_w: int
    screen_h: int
    x: int
    y: int
    x_norm: float
    y_norm: float
    start_x: int
    start_y: int
    button: int
    mods: str
    context: str
    hint_label: str
    pool_signature: str
    pool_size: int
    pool_hit_index: int       # index of chosen label in pool, -1 if no pool/no label match
    aerospace_corner_ambiguous: bool
    candidates: list[dict[str, Any]]


def iter_events(path: Path) -> Iterator[dict[str, Any]]:
    with path.open("r", encoding="utf-8") as f:
        for lineno, line in enumerate(f, 1):
            line = line.strip()
            if not line:
                continue
            try:
                ev = json.loads(line)
            except json.JSONDecodeError as e:
                print(f"WARN: line {lineno}: {e}", file=sys.stderr)
                continue
            if ev.get("schema") != SCHEMA_VERSION:
                print(
                    f"WARN: line {lineno}: schema {ev.get('schema')} != {SCHEMA_VERSION}",
                    file=sys.stderr,
                )
                continue
            yield ev


def build_samples(events: Iterator[dict[str, Any]]) -> Iterator[Sample]:
    # Most recent hint_present per (session, screen_uuid).
    last_pool: dict[tuple[str, str], HintPool] = {}

    for ev in events:
        typ = ev.get("type")
        if typ == "hint_present":
            session = ev["session"]
            screen = ev.get("screen", {})
            scr_uuid = screen.get("uuid", "unknown")
            last_pool[(session, scr_uuid)] = HintPool(
                session=session,
                screen_uuid=scr_uuid,
                seq=ev["seq"],
                signature=ev.get("signature", ""),
                viewport_w=ev.get("viewport_w", screen.get("w", 0)),
                viewport_h=ev.get("viewport_h", screen.get("h", 0)),
                hints=ev.get("hints", []),
            )
            continue

        if typ not in TARGET_TYPES:
            continue

        screen = ev.get("screen", {})
        window = ev.get("window", {})
        scr_uuid = screen.get("uuid", "unknown")
        scr_w = max(int(screen.get("w", 0)), 1)
        scr_h = max(int(screen.get("h", 0)), 1)
        x = int(ev.get("x", 0))
        y = int(ev.get("y", 0))

        pool = last_pool.get((ev["session"], scr_uuid))
        # Only join pools that preceded this click in the same session.
        candidates: list[dict[str, Any]] = []
        pool_sig = ""
        pool_hit_index = -1
        if pool and pool.seq < ev["seq"]:
            candidates = pool.hints
            pool_sig = pool.signature
            label = ev.get("hint_label", "")
            if label:
                for i, h in enumerate(candidates):
                    if h.get("label") == label:
                        pool_hit_index = i
                        break

        corner = x > scr_w - 10 and y > scr_h - 10

        yield Sample(
            session=ev["session"],
            seq=int(ev["seq"]),
            t_us=int(ev["t_us"]),
            wall_ms=int(ev["wall_ms"]),
            event_type=typ,
            mode=ev.get("mode", ""),
            mode_trace=ev.get("mode_trace", ""),
            bundle_id=window.get("bundle_id", "unknown"),
            app_name=window.get("app_name", "unknown"),
            screen_uuid=scr_uuid,
            screen_index=int(screen.get("index", 0)),
            screen_total=int(screen.get("total", 1)),
            screen_w=scr_w,
            screen_h=scr_h,
            x=x,
            y=y,
            x_norm=x / scr_w,
            y_norm=y / scr_h,
            start_x=int(ev.get("start_x", x)),
            start_y=int(ev.get("start_y", y)),
            button=int(ev.get("button", 0)),
            mods=ev.get("mods", ""),
            context=ev.get("context", ""),
            hint_label=ev.get("hint_label", ""),
            pool_signature=pool_sig,
            pool_size=len(candidates),
            pool_hit_index=pool_hit_index,
            aerospace_corner_ambiguous=corner,
            candidates=candidates,
        )


def validate_invariants(samples: list[Sample]) -> list[str]:
    errors: list[str] = []
    by_session: dict[str, list[Sample]] = defaultdict(list)
    for s in samples:
        by_session[s.session].append(s)

    for sess, items in by_session.items():
        items.sort(key=lambda s: s.seq)
        prev = -1
        for s in items:
            if s.seq <= prev:
                errors.append(
                    f"session={sess}: seq not monotonic ({prev} -> {s.seq})"
                )
            prev = s.seq

    for s in samples:
        if not s.screen_uuid:
            errors.append(f"session={s.session} seq={s.seq}: empty screen_uuid")
        if not (0.0 <= s.x_norm <= 1.0) or not (0.0 <= s.y_norm <= 1.0):
            errors.append(
                f"session={s.session} seq={s.seq}: normalized coord out of [0,1]: "
                f"({s.x_norm:.3f},{s.y_norm:.3f})"
            )
        if s.event_type == "click" and s.hint_label and s.pool_size > 0 and s.pool_hit_index < 0:
            errors.append(
                f"session={s.session} seq={s.seq}: hint_label '{s.hint_label}' "
                f"not present in joined pool (sig={s.pool_signature})"
            )

    return errors


def write_jsonl(samples: list[Sample], out: Path) -> None:
    with out.open("w", encoding="utf-8") as f:
        for s in samples:
            f.write(json.dumps(asdict(s), separators=(",", ":")) + "\n")


def write_parquet(samples: list[Sample], out: Path) -> None:
    try:
        import pyarrow as pa
        import pyarrow.parquet as pq
    except ImportError:
        print("ERROR: pyarrow not installed; falling back to jsonl", file=sys.stderr)
        write_jsonl(samples, out.with_suffix(".jsonl"))
        return

    rows = [asdict(s) for s in samples]
    table = pa.Table.from_pylist(rows)
    pq.write_table(table, out)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "input",
        nargs="?",
        default=str(Path.home() / ".local/share/warpd/events.jsonl"),
        help="Path to events.jsonl",
    )
    parser.add_argument(
        "-o", "--output",
        default="warpd_dataset.jsonl",
        help="Output path (.jsonl or .parquet)",
    )
    parser.add_argument(
        "--strict", action="store_true",
        help="Exit nonzero if any invariant check fails",
    )
    args = parser.parse_args()

    in_path = Path(args.input)
    if not in_path.exists():
        print(f"ERROR: {in_path} does not exist", file=sys.stderr)
        return 2

    samples = list(build_samples(iter_events(in_path)))
    if not samples:
        print("WARN: no joinable samples produced (no click events?)", file=sys.stderr)

    errors = validate_invariants(samples)
    for e in errors:
        print(f"WARN: {e}", file=sys.stderr)

    out_path = Path(args.output)
    if out_path.suffix == ".parquet":
        write_parquet(samples, out_path)
    else:
        write_jsonl(samples, out_path)

    n_with_label = sum(1 for s in samples if s.hint_label)
    n_with_pool = sum(1 for s in samples if s.pool_size > 0)
    n_hits = sum(1 for s in samples if s.pool_hit_index >= 0)
    sessions = {s.session for s in samples}
    print(
        f"wrote {len(samples)} samples ({n_with_label} with hint_label, "
        f"{n_with_pool} with pool, {n_hits} pool hits) "
        f"across {len(sessions)} sessions to {out_path}",
        file=sys.stderr,
    )

    if args.strict and errors:
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
