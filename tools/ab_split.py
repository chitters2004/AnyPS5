import argparse
import json
import math
import re
import sys
from pathlib import Path

DESCRIPTION = """Per-phase means of an in-run A/B (APS5_AB) log. The driver prints "[ab] phase A|B <k> begins at present
<n> (t <s> s)" when a phase starts and "[ab] phase A|B <k> settled at present <n> (t <s> s)" APS5_AB_SETTLE seconds
later. Each periodic report covers the time since the previous report of its kind, so a report counts for phase k
only when the previous report of its kind came after phase k's settled line and the report came before phase k+1
began. Presents per 10 s and frame time come from the [ab] lines (settled line to the next phase's begin), GPU busy
and idle gaps per frame from [present], GPU time per present from [gputime] (APS5_PROFILE_GPU), the committer and
drains from [draw] pipelined, unlocked hook waits from [syncwait] (APS5_PROFILE_DRAW). The driver also prints one
"[ab-counters] phase A|B <k> ..." line when phase k ends, from counters that run without APS5_PROFILE_DRAW: they cover
exactly the settled part of the phase and give the committer's busy time and microseconds per committed draw, its
waits, each traced thread's busy and waiting time (queue workers, the draw finisher; "threads=none" when the trace
totals were not recording, so run with APS5_TRACE_SUMMARY_EVERY), the GPU's busy time and the flip interval p50 and
p90. The noise column compares
each B phase with the mean of its neighbouring A phases, which cancels a linear drift: mean difference, standard
error, standard deviation and count; --spread adds each side's phase count and phase-to-phase standard deviation. A requested program's phase metric is omitted if a report lists 12 programs
without it: the list may be truncated, so the program's time is unknown."""

COUNTERS = re.compile(r"^\[ab-counters\] phase ([AB]) (\d+) (.*)$")
PHASE = re.compile(r"^\[ab\] phase ([AB]) (\d+) (begins|settled) at present (\d+) \(t ([\d.]+) s\)")
KINDS = (("present", "[present] "), ("gputime", "[gputime] "), ("draw", "[draw] pipelined"), ("syncwait", "[syncwait] "))


def finite(value):
    if isinstance(value, float) and not math.isfinite(value):
        raise ValueError("non-finite A/B statistic")
    if isinstance(value, dict):
        for item in value.values():
            finite(item)
    elif isinstance(value, (list, tuple)):
        for item in value:
            finite(item)
    return value


def finite_sum(values):
    return finite(sum(values))


def number(pattern, text):
    match = re.search(pattern, text)
    return finite(float(match.group(1))) if match else None


def parse_report(kind, text):
    if kind == "present":
        presents, dropped = number(r"^\[present\] (\d+) presents", text), number(r"\((\d+) dropped", text)
        busy, gaps = number(r"GPU busy ([\d.]+) ms", text), number(r"idle gaps ([\d.]+) ms", text)
        if None in (presents, dropped, busy, gaps):
            return None
        return {"frames": presents + dropped, "busy": busy, "gaps": gaps}
    if kind == "gputime":
        total, presents = number(r"^\[gputime\] ([\d.]+) ms of GPU time", text), number(r"; (\d+) presents;", text)
        if total is None or presents is None or "; by class:" not in text:
            return None
        programs_text, classes_text = text.split("by program:", 1)[-1].split("; by class:", 1)
        programs, classes = {}, {}
        for key, ms in re.findall(r" (?<!draw )(0x[0-9a-f]+) x\d+ ([\d.]+)ms", programs_text):
            programs[key] = programs.get(key, 0.0) + finite(float(ms))
        for name, ms in re.findall(r" ([a-z][a-z-]*) x\d+ ([\d.]+)ms [\d.]+MiB \(per present", classes_text):
            classes[name] = classes.get(name, 0.0) + finite(float(ms))
        truncated = len(re.findall(r" (?:draw )?0x[0-9a-f]+ x\d+ [\d.]+ms", programs_text)) >= 12
        return {"total": total, "presents": presents, "programs": programs, "programs_truncated": truncated, "classes": classes}
    if kind == "draw":
        commits, per_commit = number(r": (\d+) commits at", text), number(r"commits at ([\d.]+) us each", text)
        enqueued, room = number(r"; (\d+) enqueued behind", text), number(r"waiting ([\d.]+) us per draw for room", text)
        if None in (commits, per_commit, enqueued, room) or "; drains:" not in text:
            return None
        drains = text.split("; drains:", 1)[1].split(";", 1)[0]
        return {"commits": commits, "per_commit": per_commit, "enqueued": enqueued, "room": room,
                "drain_ms": finite_sum(finite(float(ms)) for ms in re.findall(r"\(([\d.]+) ms\)", drains))}
    if kind == "syncwait":
        if "):" not in text:
            return None
        sites = text.split("):", 1)[1].split("; batches ahead", 1)[0]
        return {"wait_ms": finite_sum(finite(float(ms)) for ms in re.findall(r" \d+/([\d.]+)ms", sites))}
    return None


def parse_counters(text):
    counters = {key: finite(float(value)) for key, value in re.findall(r" ?([\w.]+)=(-?[\d.]+)", text)}
    counters["recorded"] = "threads=none" not in text
    return counters


def read_log(lines):
    config, phases, current = None, [], None
    previous = {kind: None for kind, _ in KINDS}
    for line in lines:
        text = line.rstrip("\r\n")
        if text.startswith("[ab] in-run A/B:"):
            if config is not None or phases:
                raise ValueError("multiple A/B runs in one log; split the captures before comparing phases")
            config = text
            continue
        counters = COUNTERS.match(text)
        if counters:
            for phase in reversed(phases):
                if phase["kind"] == counters.group(1) and phase["index"] == int(counters.group(2)):
                    phase["counters"] = parse_counters(counters.group(3))
                    break
            continue
        match = PHASE.match(text)
        if match:
            kind, index, event, present, seconds = match.group(1), int(match.group(2)), match.group(3), int(match.group(4)), finite(float(match.group(5)))
            point = (present, seconds)

            if event == "begins":
                if phases and index <= phases[-1]["index"]:
                    raise ValueError("phase index restarts in one log (multiple A/B runs); split the captures before comparing phases")

                if current is not None:
                    previous_point = current["settled"] or current["begin"]
                    if point[0] < previous_point[0] or point[1] < previous_point[1]:
                        raise ValueError("phase began before the previous phase boundary")
                    current["end"] = point

                current = {"kind": kind, "index": index, "begin": point, "settled": None, "end": None, "counters": None,
                           "reports": {name: [] for name, _ in KINDS}}
                phases.append(current)

            elif current is not None and current["index"] == index and current["kind"] == kind:
                if current["settled"] is not None:
                    raise ValueError("phase settled more than once")
                previous_point = current["begin"]
                if point[0] < previous_point[0] or point[1] < previous_point[1]:
                    raise ValueError("phase settled before it began")
                current["settled"] = point

            continue
        for name, prefix in KINDS:
            if text.startswith(prefix):
                state = (current["index"], current["settled"] is not None) if current is not None else None
                if state is not None and state[1] and previous[name] == state:
                    report = parse_report(name, text)
                    if report is not None:
                        current["reports"][name].append(report)
                previous[name] = state
                break
    return config, phases


def weighted(rows, value, weight):
    total = finite_sum(row[weight] for row in rows)
    return finite_sum(row[value] * row[weight] for row in rows) / total if total else None


THREAD_METRICS = (("busy_ms", "busy %", True), ("idle_ms", "idle %", True), ("memory_wait_ms", "memory wait ms/present", False),
                  ("lock_wait_ms", "GPU lock wait ms/present", False), ("gpu_wait_ms", "GPU wait ms/present", False), ("jobs", "jobs/present", False))


def counter_metrics(counters):
    out = {}
    wall, presents = counters.get("wall_ms", 0.0), counters.get("presents", 0.0)
    if presents:
        per_present = {"committer ms/present": "commit_ms", "committer room wait ms/present": "room_wait_ms", "committer drain wait ms/present": "drain_wait_ms", "finisher drain wait ms/present": "finisher_drain_wait_ms",
                       "GPU busy ms/present": "gpu_busy_ms", "GPU idle gaps ms/present": "gpu_gap_ms", "draws/present": "draws"}
        for name, key in per_present.items():
            if key in counters:
                out[name] = counters[key] / presents
    for name in ("flip_p50_ms", "flip_p90_ms"):
        if name in counters:
            out["flip " + name[5:8] + " ms"] = counters[name]
    if wall and "commit_ms" in counters:
        out["committer busy %"] = 100 * counters["commit_ms"] / wall
        out["committer idle %"] = 100 - out["committer busy %"]
    if counters.get("draws"):
        out["committer us/draw"] = 1000 * counters["draw_commit_ms"] / counters["draws"]
    for key, value in counters.items():
        match = re.fullmatch(r"t\.(.+)\.(\w+)", key)
        if not match:
            continue
        thread = match.group(1).replace("_", " ")
        for suffix, name, percent in THREAD_METRICS:
            if match.group(2) != suffix:
                continue
            if percent and wall:
                out[f"{thread} {name}"] = 100 * value / wall
            elif not percent and presents:
                out[f"{thread} {name}"] = value / presents
    return out


def metrics(phase, programs, classes):
    out = counter_metrics(phase["counters"]) if phase.get("counters") else {}
    if phase["settled"] is not None and phase["end"] is not None and phase["end"][1] > phase["settled"][1]:
        presents, seconds = phase["end"][0] - phase["settled"][0], phase["end"][1] - phase["settled"][1]
        out["presents/10s"] = presents / seconds * 10
        if presents > 0:
            out["frame ms"] = seconds / presents * 1000
    rows = phase["reports"]["present"]
    out["GPU busy ms/frame"], out["GPU idle gaps ms/frame"] = weighted(rows, "busy", "frames"), weighted(rows, "gaps", "frames")
    rows = phase["reports"]["gputime"]
    presents = finite_sum(row["presents"] for row in rows)
    if presents:
        out["gputime ms/present"] = finite_sum(row["total"] for row in rows) / presents
        for key in programs:
            if all(key in row["programs"] or not row["programs_truncated"] for row in rows):
                out[f"{key} ms/present"] = finite_sum(row["programs"].get(key, 0.0) for row in rows) / presents
        for name in classes:
            out[f"{name} ms/present"] = finite_sum(row["classes"].get(name, 0.0) for row in rows) / presents
    rows = phase["reports"]["draw"]
    out["committer us/commit"], out["room wait us/draw"] = weighted(rows, "per_commit", "commits"), weighted(rows, "room", "enqueued")
    out["drain ms/report"] = finite_sum(row["drain_ms"] for row in rows) / len(rows) if rows else None
    rows = phase["reports"]["syncwait"]
    out["syncwait GPU ms/report"] = finite_sum(row["wait_ms"] for row in rows) / len(rows) if rows else None
    return {name: value for name, value in out.items() if value is not None}


def noise(values, phases):
    differences = []
    for index, phase in enumerate(phases):
        if phase["kind"] != "B" or values[index] is None:
            continue
        neighbours = [values[other] for other in (index - 1, index + 1)
                      if 0 <= other < len(phases) and phases[other]["kind"] == "A"
                      and abs(phases[other]["index"] - phase["index"]) == 1 and values[other] is not None]
        if neighbours:
            differences.append(values[index] - finite_sum(neighbours) / len(neighbours))
    if not differences:
        return None
    mean = finite_sum(differences) / len(differences)
    if len(differences) == 1:
        return {"mean": mean, "se": None, "sd": None, "n": 1}
    sd = math.sqrt(finite_sum((value - mean) ** 2 for value in differences) / (len(differences) - 1))
    return {"mean": mean, "se": sd / math.sqrt(len(differences)), "sd": sd, "n": len(differences)}


def spread(values):
    if len(values) < 2:
        return None
    mean = finite_sum(values) / len(values)
    return math.sqrt(finite_sum((value - mean) ** 2 for value in values) / (len(values) - 1))


def split_report(lines, programs, classes):
    config, phases = read_log(lines)
    if not phases:
        raise ValueError("no [ab] phase lines (was APS5_AB set?)")
    table = [metrics(phase, programs, classes) for phase in phases]
    names = list(dict.fromkeys(name for row in table for name in row))
    rows = []
    for name in names:
        values = [row.get(name) for row in table]
        a = [value for phase, value in zip(phases, values) if phase["kind"] == "A" and value is not None]
        b = [value for phase, value in zip(phases, values) if phase["kind"] == "B" and value is not None]
        if a and b:
            mean_a, mean_b = finite_sum(a) / len(a), finite_sum(b) / len(b)
            rows.append({"metric": name, "a": mean_a, "b": mean_b, "change": mean_b - mean_a,
                         "change_pct": (mean_b / mean_a - 1) * 100 if mean_a else None, "noise": noise(values, phases),
                         "a_phases": len(a), "a_sd": spread(a), "b_phases": len(b), "b_sd": spread(b)})
    return {"config": config,
            "phases": [{"kind": phase["kind"], "index": phase["index"], "begin_present": phase["begin"][0],
                        "settled_present": phase["settled"][0] if phase["settled"] else None,
                        "end_present": phase["end"][0] if phase["end"] else None,
                        "clean_reports": {name: len(reports) for name, reports in phase["reports"].items()},
                        "metrics": row} for phase, row in zip(phases, table)],
            "metrics": rows}


def split(lines, programs=(), classes=()):
    try:
        return finite(split_report(lines, programs, classes))
    except OverflowError as error:
        raise ValueError("A/B statistic exceeds the finite numeric range") from error


def side(mean, phases, sd):
    return f"{mean:>10.2f} {phases:>3} " + ("     -" if sd is None else f"{sd:>6.2f}")


def text_report(result, show_spread=False):
    if show_spread:
        return spread_report(result)
    lines = [result["config"] or "[ab] configuration line missing", "",
             "phase  begin / settled / end   clean reports (present/gputime/draw/syncwait)  presents/10s"]
    for phase in result["phases"]:
        settled = "-" if phase["settled_present"] is None else phase["settled_present"]
        end = "-" if phase["end_present"] is None else phase["end_present"]
        reports = "/".join(str(phase["clean_reports"][name]) for name, _ in KINDS)
        rate = phase["metrics"].get("presents/10s")
        lines.append(f"{phase['kind']} {phase['index']:<3} {phase['begin_present']:>7} / {settled:>7} / {end:>7}   {reports:<45}  "
                     + ("unfinished" if rate is None else f"{rate:.2f}"))
    lines.extend(["", f"{'metric':<30} {'A':>10} {'B':>10} {'B-A':>10} {'B/A-1':>8}   B vs neighbouring A: mean +- SE (SD, n)"])
    for row in result["metrics"]:
        spread = row["noise"]
        if spread is None:
            text = "-"
        elif spread["n"] == 1:
            text = f"{spread['mean']:+.2f} (n 1)"
        else:
            text = f"{spread['mean']:+.2f} +- {spread['se']:.2f} (SD {spread['sd']:.2f}, n {spread['n']})"
        percent = "n/a" if row["change_pct"] is None else f"{row['change_pct']:+.1f}%"
        lines.append(f"{row['metric']:<30} {row['a']:>10.2f} {row['b']:>10.2f} {row['change']:>+10.2f} {percent:>8}   {text}")
    return "\n".join(lines) + "\n"


def spread_report(result):
    lines = [result["config"] or "[ab] configuration line missing", "",
             "phase  begin / settled / end   clean reports (present/gputime/draw/syncwait)  presents/10s"]
    for phase in result["phases"]:
        settled = "-" if phase["settled_present"] is None else phase["settled_present"]
        end = "-" if phase["end_present"] is None else phase["end_present"]
        reports = "/".join(str(phase["clean_reports"][name]) for name, _ in KINDS)
        rate = phase["metrics"].get("presents/10s")
        lines.append(f"{phase['kind']} {phase['index']:<3} {phase['begin_present']:>7} / {settled:>7} / {end:>7}   {reports:<45}  "
                     + ("unfinished" if rate is None else f"{rate:.2f}"))
    lines.extend(["", f"{'metric':<30} arm {'A':>10} {'n':>3} {'SD':>6} {'arm':>10} {'n':>3} {'SD':>6} {'arm-A':>10} {'arm/A-1':>8}   arm vs bracketing A: mean +- SE (SD, n)"])
    for row in result["metrics"]:
        spread_noise = row["noise"]
        if spread_noise is None:
            text = "-"
        elif spread_noise["n"] == 1:
            text = f"{spread_noise['mean']:+.2f} (n 1)"
        else:
            text = f"{spread_noise['mean']:+.2f} +- {spread_noise['se']:.2f} (SD {spread_noise['sd']:.2f}, n {spread_noise['n']})"
        percent = "n/a" if row["change_pct"] is None else f"{row['change_pct']:+.1f}%"
        lines.append(f"{row['metric']:<30} {row.get('arm', 'B'):>3} {side(row['a'], row['a_phases'], row['a_sd'])} "
                     f"{side(row['b'], row['b_phases'], row['b_sd'])} {row['change']:>+10.2f} {percent:>8}   {text}")
    return "\n".join(lines) + "\n"


def program_key(value):
    if not re.fullmatch(r"0x[0-9a-fA-F]+", value):
        raise argparse.ArgumentTypeError("must be a hexadecimal program key such as 0x1000")
    return "0x" + value[2:].lower().lstrip("0").rjust(1, "0")


def main(arguments=None):
    parser = argparse.ArgumentParser(description=DESCRIPTION)
    parser.add_argument("log", help="the run's standard error log with the [ab] lines")
    parser.add_argument("--program", action="append", type=program_key, default=[], help="also report this [gputime] program key per present (repeatable)")
    parser.add_argument("--class", dest="classes", action="append", default=[], help="also report this [gputime] command class per present (repeatable)")
    parser.add_argument("--json", action="store_true", help="print the phases and metrics as JSON")
    parser.add_argument("--spread", action="store_true", help="also print each side's phase count and phase-to-phase standard deviation (always shown with several arms)")
    options = parser.parse_args(arguments)
    with Path(options.log).open(encoding="utf-8-sig", errors="replace") as source:
        result = split(source, options.program, options.classes)
    sys.stdout.write(json.dumps(result, indent=1) + "\n" if options.json else text_report(result, options.spread))
    return 0


if __name__ == "__main__":
    if hasattr(sys.stdout, "reconfigure"):
        sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    try:
        sys.exit(main())
    except (OSError, ValueError) as error:
        print(f"FAIL: {error}", file=sys.stderr)
        sys.exit(2)
