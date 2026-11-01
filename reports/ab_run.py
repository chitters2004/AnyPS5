#!/usr/bin/env python3
import argparse
import json
import os
import re
import shutil
import statistics
import subprocess
import sys
import tempfile
import time
from pathlib import Path

SWITCHES = {
    "ring": {"APS5_NO_DYNAMIC_DRAW_DATA": "1"},
    "set-reuse": {"APS5_NO_DRAW_SET_REUSE": "1"},
    "chunk": {"APS5_DRAW_UPLOAD_CHUNK_MIB": "1"},
    "pending-gate": {"APS5_NO_PENDING_WRITE_GATE": "1"},
}


def configurations(names):
    table = {"head": {}}
    for name, env in SWITCHES.items():
        table["no-" + name] = dict(env)
    table["all-off"] = {key: value for env in SWITCHES.values() for key, value in env.items()}
    if not names:
        return table
    unknown = [name for name in names if name not in table]
    if unknown:
        raise SystemExit(f"unknown configurations {unknown}; known: {sorted(table)}")
    return {name: table[name] for name in names}


MODES = {
    "timing": {"APS5_TRACE_SUMMARY_EVERY": "10"},
    "profile": {"APS5_PROFILE_DRAW": "1", "APS5_TRACE_SUMMARY_EVERY": "10"},
}


def launch(command, env, directory, seconds):
    directory.mkdir(parents=True, exist_ok=True)
    log = open(directory / "stderr.log", "wb")
    out = open(directory / "stdout.log", "wb")
    started = time.monotonic()
    process = subprocess.Popen(command, env=env, stdout=out, stderr=log)
    killed = False
    try:
        process.wait(timeout=seconds)
    except subprocess.TimeoutExpired:
        killed = True
        process.terminate()
        try:
            process.wait(timeout=15)
        except subprocess.TimeoutExpired:
            process.kill()
            process.wait()
    log.close()
    out.close()
    return {"exit": process.returncode, "killed_after_timeout": killed, "seconds": time.monotonic() - started}


def run(args):
    out = Path(args.out)
    configs = configurations(args.configs.split(",") if args.configs else [])
    modes = ["timing", "profile"] if args.mode == "both" else [args.mode]
    names = list(configs)
    base = dict(os.environ)
    for key in [key for key in base if key in {name for env in SWITCHES.values() for name in env}]:
        del base[key]
    if args.pipeline:
        base["APS5_DRAW_PIPELINE"] = "1"
    for mode in modes:
        for round_index in range(args.rounds):
            order = names[round_index % len(names):] + names[:round_index % len(names)]
            for name in order:
                directory = out / mode / name / f"round-{round_index + 1}"
                if directory.exists():
                    shutil.rmtree(directory)
                directory.mkdir(parents=True)
                env = dict(base)
                env.update(MODES[mode])
                env.update(configs[name])
                env["APS5_TRACE_DIR"] = str(directory.resolve())
                print(f"[ab_run] {mode} {name} round {round_index + 1}: {' '.join(args.command)}", flush=True)
                result = launch(args.command, env, directory, args.seconds)
                meta = {"mode": mode, "config": name, "round": round_index + 1, "switches": configs[name], "pipeline": args.pipeline, "warmup_s": args.warmup, "command": args.command}
                meta.update(result)
                (directory / "meta.json").write_text(json.dumps(meta, indent=1))
                if args.pause:
                    time.sleep(args.pause)
    print(report(out), flush=True)


NUMBER = r"([0-9]+(?:\.[0-9]+)?)"


def last_line(text, prefix):
    found = None
    for line in text.splitlines():
        if line.startswith(prefix):
            found = line
    return found


def parse_profile(text):
    result = {}
    line = last_line(text, "[lookup-split]")
    if line:
        match = re.search(r"\(10 s\): ([0-9]+) draw resource lookups, ([0-9]+) hits; us per lookup:(.*)$", line)
        if match:
            result["lookups_per_10s"] = int(match.group(1))
            steps = dict((name.strip(), float(value)) for name, value in re.findall(r"([a-z ]+?) " + NUMBER, match.group(3)))
            for name, value in steps.items():
                result["lookup us: " + name] = value
    line = last_line(text, "[revalidate-split]")
    if line:
        fields = {
            "epoch proofs reused": r"([0-9]+) epoch proofs reused",
            "epoch proofs proved": r"([0-9]+) proved \(",
            "full revalidations": r"([0-9]+) full revalidations",
            "revalidate us: pre-pass": r"pre-pass " + NUMBER + " us",
            "ranges per revalidation": r"over " + NUMBER + " ranges",
            "pending-write checks per revalidation": r"\(" + NUMBER + " pending-write checks\)",
            "revalidate us: images": r"images " + NUMBER + " us",
            "revalidate us: imports": r"imports " + NUMBER + " us",
            "revalidate us: rest": r"rest " + NUMBER + " us",
            "moved: collect epoch": r"collect epoch ([0-9]+)",
            "moved: proof serial": r"proof serial ([0-9]+)",
            "moved: pending serial": r"pending serial ([0-9]+)",
        }
        for name, pattern in fields.items():
            match = re.search(pattern, line)
            if match:
                result[name] = float(match.group(1))
    line = last_line(text, "[rescache] draws:")
    if line:
        match = re.search(r"([0-9]+) ring with dynamic offsets \(([0-9]+) ring sets written\), ([0-9]+) rewritten sets", line)
        if match:
            ring, sets, rewritten = (int(value) for value in match.groups())
            result["ring draws"] = ring
            result["ring sets written"] = sets
            result["rewritten draw sets"] = rewritten
            if ring + rewritten:
                result["ring share %"] = 100.0 * ring / (ring + rewritten)
    return result


def parse_summary(path, warmup):
    rows = []
    if not path.exists():
        return rows
    for text in path.read_text(errors="replace").splitlines():
        try:
            line = json.loads(text)
        except ValueError:
            continue
        if "interval_start_s" not in line or line.get("presents", 0) <= 0:
            continue
        if line["interval_start_s"] < warmup:
            continue
        rows.append(line)
    return rows


def median(values):
    return statistics.median(values) if values else None


def collect(out):
    data = {}
    for meta_path in sorted(Path(out).glob("*/*/round-*/meta.json")):
        meta = json.loads(meta_path.read_text())
        directory = meta_path.parent
        entry = data.setdefault(meta["mode"], {}).setdefault(meta["config"], {"runs": []})
        rows = parse_summary(directory / "aps5-summary.jsonl", meta.get("warmup_s", 0))
        stderr = (directory / "stderr.log").read_text(errors="replace") if (directory / "stderr.log").exists() else ""
        run = {"round": meta["round"], "exit": meta.get("exit"), "killed": meta.get("killed_after_timeout"), "intervals": len(rows)}
        if rows:
            run["p50"] = median([row["frame_ms"]["p50"] for row in rows])
            run["p99"] = median([row["frame_ms"]["p99"] for row in rows])
            run["fps"] = sum(row["presents"] for row in rows) / max(sum(row["interval_end_s"] - row["interval_start_s"] for row in rows), 1e-9)
            run["gpu"] = median([row["gpu_busy_ms_per_present"] for row in rows])
            workers = {}
            for row in rows:
                for thread, pct in row.get("worker_busy_pct", {}).items():
                    workers.setdefault(thread, []).append(pct)
            if workers:
                busiest = max(workers.items(), key=lambda entry: median(entry[1]))
                run["busiest"] = f"{busiest[0]} {median(busiest[1]):.0f} %"
        run["profile"] = parse_profile(stderr)
        entry["runs"].append(run)
    return data


def fmt(value, digits=2):
    return "-" if value is None else f"{value:.{digits}f}"


def report(out):
    data = collect(out)
    lines = []
    timing = data.get("timing", {})
    if timing:
        lines += ["## Frame times (timing pass, no profiling; medians over the rounds, intervals after the warm-up)", "",
                  "| configuration | rounds | intervals | frame p50 ms | p99 ms | fps | GPU busy ms | busiest worker | p50 vs head | p50 spread over rounds |",
                  "|---|---|---|---|---|---|---|---|---|---|"]
        head = median([run["p50"] for run in timing.get("head", {"runs": []})["runs"] if "p50" in run])
        for name, entry in timing.items():
            runs = [run for run in entry["runs"] if "p50" in run]
            p50 = median([run["p50"] for run in runs])
            delta = "-" if head is None or p50 is None or name == "head" else f"{p50 - head:+.2f} ms ({100.0 * (p50 - head) / head:+.1f} %)"
            spread = "-" if not runs else f"{min(run['p50'] for run in runs):.2f} to {max(run['p50'] for run in runs):.2f}"
            busiest = runs[0].get("busiest", "-") if runs else "-"
            lines.append(f"| {name} | {len(entry['runs'])} | {sum(run['intervals'] for run in entry['runs'])} | {fmt(p50)} | {fmt(median([run['p99'] for run in runs]))} | {fmt(median([run['fps'] for run in runs]), 1)} | {fmt(median([run['gpu'] for run in runs]))} | {busiest} | {delta} | {spread} |")
        lines += ["", "A `no-X` row slower than `head` means change X saves that much; `all-off` approximates main for the switchable changes.", ""]
    profile = data.get("profile", {})
    if profile:
        names = list(profile)
        keys = []
        for entry in profile.values():
            for run in entry["runs"]:
                for key in run["profile"]:
                    if key not in keys:
                        keys.append(key)
        lines += ["## Profile lines (APS5_PROFILE_DRAW pass; medians over the rounds, last line of each run)", "",
                  "| value | " + " | ".join(names) + " |", "|---|" + "---|" * len(names)]
        for key in keys:
            cells = []
            for name in names:
                values = [run["profile"][key] for run in profile[name]["runs"] if key in run["profile"]]
                cells.append(fmt(median(values)))
            lines.append(f"| {key} | " + " | ".join(cells) + " |")
        lines.append("")
    problems = [f"{mode}/{name} round {run['round']}: exit {run['exit']}, {run['intervals']} intervals" for mode, configs in data.items() for name, entry in configs.items() for run in entry["runs"] if run["intervals"] == 0 and mode == "timing"]
    if problems:
        lines += ["## Runs without summary intervals", ""] + ["- " + problem for problem in problems] + [""]
    return "\n".join(lines) if lines else "no runs found\n"


def self_test():
    profile_text = "\n".join([
        "[lookup-split] (10 s): 1000 draw resource lookups, 990 hits; us per lookup: key 0.50 find 0.40 revalidate 9.00 moved buffers 1.20 alias checks 0.30",
        "[lookup-split] (10 s): 2000 draw resource lookups, 1980 hits; us per lookup: key 0.60 find 0.45 revalidate 8.00 moved buffers 1.10 alias checks 0.25",
        "[revalidate-split] (cumulative): 50 epoch proofs reused, 150 proved (no epoch 0, first proof 10; moved since the object's proof: collect epoch 120, proof serial 30, pending serial 5); 150 full revalidations, per call: pre-pass 2.50 us over 30.0 ranges (0.0 pending-write checks), images 5.00 us (12.0 textures, 1.0 storage images), imports 0.80 us, rest 0.20 us",
        "[rescache] draws: 10 hits, 1 misses, 0 invalidated, 0 uncacheable; validation memo 5 hits / 1 misses (which key words the misses differ in: the miss churn line); draw bindings (cumulative): 900 ring with dynamic offsets (40 ring sets written), 100 rewritten sets",
    ])
    parsed = parse_profile(profile_text)
    assert parsed["lookup us: revalidate"] == 8.0 and parsed["lookup us: moved buffers"] == 1.1 and parsed["lookups_per_10s"] == 2000, parsed
    assert parsed["revalidate us: pre-pass"] == 2.5 and parsed["pending-write checks per revalidation"] == 0.0 and parsed["moved: collect epoch"] == 120, parsed
    assert parsed["ring share %"] == 90.0 and parsed["ring sets written"] == 40, parsed
    with tempfile.TemporaryDirectory() as temporary:
        root = Path(temporary)
        script = root / "fake_title.py"
        script.write_text("\n".join([
            "import json, os, sys, time",
            "slow = 1.0 if os.environ.get('APS5_NO_DYNAMIC_DRAW_DATA') else 0.0",
            "path = os.path.join(os.environ['APS5_TRACE_DIR'], 'aps5-summary.jsonl')",
            "with open(path, 'w') as out:",
            "    for start in (0, 10, 20, 30):",
            "        out.write(json.dumps({'interval_start_s': start, 'interval_end_s': start + 10, 'presents': 300, 'frame_ms': {'p50': 30.0 + slow, 'p90': 32.0, 'p99': 40.0, 'max': 50.0}, 'gpu_busy_ms_per_present': 20.0, 'worker_busy_pct': {'committer': 90.0}}) + '\\n')",
            "if os.environ.get('APS5_PROFILE_DRAW'):",
            "    sys.stderr.write(" + json.dumps(profile_text + "\n") + ")",
            "    sys.stderr.flush()",
            "if os.environ.get('APS5_NO_DRAW_SET_REUSE'):",
            "    time.sleep(30)",
        ]))
        out = root / "runs"
        arguments = argparse.Namespace(out=str(out), configs="head,no-ring,no-set-reuse", mode="both", rounds=2, seconds=3, pipeline=False, warmup=5, pause=0, command=[sys.executable, str(script)])
        run(arguments)
        data = collect(out)
        timing = data["timing"]
        assert [run["p50"] for run in timing["no-ring"]["runs"]] == [31.0, 31.0], timing["no-ring"]
        assert all(run["intervals"] == 3 for run in timing["head"]["runs"]), timing["head"]
        assert all(run["killed"] for run in timing["no-set-reuse"]["runs"]), timing["no-set-reuse"]
        assert data["profile"]["head"]["runs"][0]["profile"]["ring share %"] == 90.0
        assert "PROFILE" not in json.dumps(data["timing"]) and not data["timing"]["head"]["runs"][0]["profile"]
        text = report(out)
        assert "+1.00 ms (+3.3 %)" in text, text
    print("ab_run self-test passed")


def main():
    parser = argparse.ArgumentParser(description="Run a title or a frame replay once per draw-path switch configuration and compare frame times and profile lines.")
    parser.add_argument("--self-test", action="store_true")
    commands = parser.add_subparsers(dest="action")
    runner = commands.add_parser("run", help="run the command for every configuration, then print the report")
    runner.add_argument("--out", required=True, help="directory for the runs (one subdirectory per mode, configuration and round)")
    runner.add_argument("--configs", default="", help="comma-separated subset of: head, no-ring, no-set-reuse, no-chunk, no-pending-gate, all-off")
    runner.add_argument("--mode", choices=["timing", "profile", "both"], default="both")
    runner.add_argument("--rounds", type=int, default=3)
    runner.add_argument("--seconds", type=float, default=150, help="run length before the process is stopped")
    runner.add_argument("--warmup", type=float, default=30, help="summary intervals starting earlier are ignored")
    runner.add_argument("--pause", type=float, default=5, help="seconds between runs")
    runner.add_argument("--pipeline", action="store_true", help="also set APS5_DRAW_PIPELINE=1 (epoch proofs)")
    runner.add_argument("command", nargs=argparse.REMAINDER, help="-- followed by the command that starts the title or agc_frame_replay")
    reporter = commands.add_parser("report", help="print the report of an earlier run directory")
    reporter.add_argument("out")
    args = parser.parse_args()
    if args.self_test:
        self_test()
        return
    if args.action == "run":
        if args.command and args.command[0] == "--":
            args.command = args.command[1:]
        if not args.command:
            parser.error("run needs a command after --")
        run(args)
    elif args.action == "report":
        print(report(args.out))
    else:
        parser.print_help()


if __name__ == "__main__":
    main()
