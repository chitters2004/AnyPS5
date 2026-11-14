#!/usr/bin/env python3
"""Turn one APS5_TRACE_CRITICAL file into a per-frame verdict on what limits the frame."""

import argparse
import json
import statistics
import sys
from pathlib import Path

VERDICTS = ("committer", "producer", "shared-lock", "write-watch", "mixed")
WAITS = ("commit_idle_ns", "room_wait_ns", "finisher_room_wait_ns", "finisher_idle_ns", "drain_wait_ns", "worker_idle_ns", "gpu_wait_ns", "tracker_wait_ns")


def read(path):
    rows = []
    header = None
    with open(path, encoding="utf-8") as file:
        for number, line in enumerate(file, 1):
            line = line.strip()
            if not line:
                continue
            try:
                value = json.loads(line)
            except json.JSONDecodeError as error:
                raise ValueError(f"{path}:{number}: {error}") from error
            if header is None:
                if value.get("format") != "aps5-critical/1":
                    raise ValueError(f"{path}: not an aps5-critical/1 file")
                header = value
                continue
            rows.append(value)
    if header is None:
        raise ValueError(f"{path}: empty file")
    return header, rows


def total(threads, key, role=None):
    return sum(thread.get(key, 0) for thread in threads if role is None or thread.get("role") == role)


def behind(threads, lock, role):
    result = {}
    for thread in threads:
        if thread.get("role") != role:
            continue
        for owner, value in thread.get(f"{lock}_wait_behind_ns", {}).items():
            result[owner] = result.get(owner, 0) + value
    return result


def producer_of(threads):
    producers = [thread for thread in threads if thread.get("enqueues", 0) > 0]
    if not producers:
        return None
    return max(producers, key=lambda thread: thread.get("enqueues", 0))


def frame(row, limits):
    threads = row.get("threads", [])
    wall = max(row["end_ns"] - row["begin_ns"], 1)
    committer = [thread for thread in threads if thread.get("role") == "committer"]
    busy = total(committer, "commit_busy_ns")
    idle = total(committer, "commit_idle_ns")
    enqueues = total(threads, "enqueues")
    occupancy = total(threads, "occupancy_sum") / enqueues if enqueues else 0.0
    depth = row.get("ring_depth", 0)
    room = total(threads, "room_wait_ns")
    producer = producer_of(threads)
    critical = [thread for thread in threads if thread.get("role") in ("committer", "worker", "finisher")]
    lock_wait = {lock: max((thread.get(f"{lock}_wait_ns", 0) for thread in critical), default=0) for lock in ("gpu", "tracker")}
    collect = max((thread.get("collect_ns", 0) for thread in critical), default=0)
    unaccounted = {}
    for thread in threads:
        if "cpu_ns" not in thread or thread.get("role") not in ("committer", "worker", "finisher"):
            continue
        waited = sum(thread.get(key, 0) for key in WAITS)
        unaccounted[thread["name"]] = max(wall - thread["cpu_ns"] - waited, 0) / wall
    shares = {
        "committer_busy": busy / wall,
        "committer_idle": idle / wall,
        "room_wait": room / wall,
        "producer_room_wait": producer.get("room_wait_ns", 0) / wall if producer else 0.0,
        "gpu_lock_wait": lock_wait["gpu"] / wall,
        "tracker_lock_wait": lock_wait["tracker"] / wall,
        "collect": collect / wall,
    }
    full = depth and occupancy >= limits.full * depth
    empty = not depth or occupancy <= limits.empty * depth
    if shares["committer_busy"] >= limits.busy and (shares["producer_room_wait"] >= limits.wait or full):
        verdict = "committer"
    elif shares["committer_idle"] >= limits.wait and empty:
        verdict = "producer"
    elif max(shares["gpu_lock_wait"], shares["tracker_lock_wait"]) >= limits.wait:
        verdict = "shared-lock"
    elif shares["collect"] >= limits.wait:
        verdict = "write-watch"
    else:
        verdict = "mixed"
    return {
        "present": row["present"],
        "frame_ms": wall / 1e6,
        "verdict": verdict,
        "shares": shares,
        "commits": total(committer, "commits"),
        "ring_occupancy": occupancy,
        "ring_depth": depth,
        "producer": producer["name"] if producer else None,
        "gpu_wait_behind_committer_side": behind(threads, "gpu", "committer"),
        "gpu_wait_behind_worker_side": behind(threads, "gpu", "worker"),
        "not_running_unrecorded": unaccounted,
    }


def summarize(frames):
    counts = {verdict: 0 for verdict in VERDICTS}
    for entry in frames:
        counts[entry["verdict"]] += 1
    medians = {}
    if frames:
        for key in frames[0]["shares"]:
            medians[key] = statistics.median(entry["shares"][key] for entry in frames)
    leading = max(VERDICTS, key=lambda verdict: counts[verdict]) if frames else None
    return {"frames": len(frames), "verdicts": counts, "leading": leading, "median_shares": medians,
            "frame_ms_p50": statistics.median(entry["frame_ms"] for entry in frames) if frames else 0.0}


def percent(value):
    return f"{100.0 * value:5.1f}%"


def report(header, frames, summary, out, limit):
    out.write(f"{summary['frames']} frames, frame p50 {summary['frame_ms_p50']:.1f} ms; verdicts: " + ", ".join(f"{name} {count}" for name, count in summary["verdicts"].items() if count) + "\n")
    out.write(f"leading: {summary['leading']}\n")
    out.write("median shares of the frame: " + ", ".join(f"{name} {percent(value)}" for name, value in summary["median_shares"].items()) + "\n")
    out.write("present  frame_ms  verdict      commit busy  idle   room wait  ring occ/depth  gpu lock  tracker  collect\n")
    for entry in frames[:limit]:
        shares = entry["shares"]
        out.write(f"{entry['present']:7d}  {entry['frame_ms']:8.2f}  {entry['verdict']:<11}  {percent(shares['committer_busy'])}  {percent(shares['committer_idle'])}  {percent(shares['room_wait'])}  {entry['ring_occupancy']:6.1f}/{entry['ring_depth']:<6}  {percent(shares['gpu_lock_wait'])}  {percent(shares['tracker_lock_wait'])}  {percent(shares['collect'])}\n")
    if len(frames) > limit:
        out.write(f"... {len(frames) - limit} more frames (--frames to list more, --json for all)\n")


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("file", type=Path, help="aps5-critical-<commit>-<pid>.jsonl written by an APS5_TRACE_CRITICAL=1 trace window")
    parser.add_argument("--busy", type=float, default=0.85, help="committer busy share of the frame that counts as saturated (default 0.85)")
    parser.add_argument("--wait", type=float, default=0.10, help="share of the frame a wait must reach to count (default 0.10)")
    parser.add_argument("--full", type=float, default=0.75, help="mean ring occupancy, as a share of its depth, that counts as full (default 0.75)")
    parser.add_argument("--empty", type=float, default=0.25, help="mean ring occupancy, as a share of its depth, that counts as empty (default 0.25)")
    parser.add_argument("--frames", type=int, default=20, help="frames listed in the table (default 20)")
    parser.add_argument("--json", action="store_true", help="print the per-frame rows and the summary as JSON")
    args = parser.parse_args(argv)
    try:
        header, rows = read(args.file)
    except (OSError, ValueError) as error:
        print(f"critical_path: {error}", file=sys.stderr)
        return 1
    frames = [frame(row, args) for row in rows]
    summary = summarize(frames)
    if args.json:
        json.dump({"header": header, "summary": summary, "frames": frames}, sys.stdout, indent=1)
        sys.stdout.write("\n")
    else:
        report(header, frames, summary, sys.stdout, args.frames)
    return 0


if __name__ == "__main__":
    sys.exit(main())
