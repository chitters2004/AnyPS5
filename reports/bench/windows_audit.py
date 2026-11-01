#!/usr/bin/env python3
import argparse
import collections
import json
import re
import subprocess
import sys
from pathlib import Path

KINDS = {
    "emutls": re.compile(r"^__emutls_get_address$"),
    "rwlock": re.compile(r"pthread_rwlock_(rdlock|wrlock|unlock|tryrdlock|trywrlock)"),
    "mutex": re.compile(r"pthread_mutex_(lock|unlock|trylock)"),
    "fls/tls": re.compile(r"(FlsGetValue|TlsGetValue|pthread_getspecific)"),
}
ORDER = ("emutls", "rwlock", "mutex", "fls/tls", "atomic")
FUNCTION = re.compile(r"^[0-9a-f]+ <(.+)>:$")
RELOCATION = re.compile(r"^\s+[0-9a-f]+: (?:R_X86_64|IMAGE_REL_AMD64)\S*\s+(\S+)")
LOCKED = re.compile(r"^\s+[0-9a-f]+:\s+lock ")
EMUTLS_VARIABLE = re.compile(r"^__emutls_v\.(\S+?)(?:[-+]0x[0-9a-f]+)?$")


def demangle(names):
    if not names:
        return {}
    result = subprocess.run(["c++filt"], input="\n".join(names), capture_output=True, text=True).stdout.splitlines()
    return dict(zip(names, result))


def static_counts(objdump, objects):
    functions = {}
    for path in objects:
        text = subprocess.run([objdump, "-dr", "--no-show-raw-insn", str(path)], capture_output=True, text=True, errors="replace").stdout
        current = None
        for line in text.splitlines():
            match = FUNCTION.match(line)
            if match:
                current = functions.setdefault(match.group(1), collections.Counter())
                continue
            if current is None:
                continue
            if LOCKED.match(line):
                current["atomic"] += 1
            match = RELOCATION.match(line)
            if not match:
                continue
            symbol = match.group(1)
            for kind, pattern in KINDS.items():
                if pattern.search(symbol):
                    current[kind] += 1
            variable = EMUTLS_VARIABLE.match(symbol)
            if variable:
                current["tls:" + variable.group(1)] += 1
    return {name: dict(counts) for name, counts in functions.items() if any(counts[kind] for kind in ORDER)}


def linux_symbols(library):
    symbols = {}
    for line in subprocess.run(["nm", library], capture_output=True, text=True).stdout.splitlines():
        parts = line.split()
        if len(parts) == 3 and parts[1] in "tTwW":
            symbols.setdefault(parts[2], int(parts[0], 16))
    return symbols


def probe_counts(perf, library, functions, command, draw_symbol):
    symbols = linux_symbols(library)
    targets = {name: symbols[name] for name in functions if name in symbols}
    if draw_symbol not in symbols:
        sys.exit(f"{draw_symbol} is not in {library}")
    targets[draw_symbol] = symbols[draw_symbol]
    names = sorted(targets)
    events = {}
    subprocess.run([perf, "probe", "-q", "-d", "probe_audit:*"], capture_output=True)
    for start in range(0, len(names), 64):
        arguments = [perf, "probe", "-q", "-x", library]
        for index, name in enumerate(names[start:start + 64], start):
            event = f"f{index}"
            events[event] = name
            arguments += ["-a", f"probe_audit:{event}=0x{targets[name]:x}"]
        subprocess.run(arguments, capture_output=True, check=True)
    totals = collections.Counter()
    try:
        for run in command:
            result = subprocess.run([perf, "stat", "-x", ";", "-e", "probe_audit:*", "--"] + run, capture_output=True, text=True)
            for line in result.stderr.splitlines():
                fields = line.split(";")
                if len(fields) > 2 and fields[2].startswith("probe_audit:") and fields[0].strip().isdigit():
                    totals[events[fields[2].split(":", 1)[1]]] += int(fields[0])
    finally:
        subprocess.run([perf, "probe", "-q", "-d", "probe_audit:*"], capture_output=True)
    return totals, len(targets) - 1, len(functions)


def main():
    parser = argparse.ArgumentParser(description="Estimate the operations that are slow on Windows (emutls thread_local access, winpthreads rwlock and mutex calls, FLS/TLS slots, atomic instructions) per draw: call sites per function from MinGW objects, times calls per draw counted with uprobes on a Linux build of the same source.")
    parser.add_argument("--objects", required=True, help="directory of MinGW objects of libSceAgcDriver (-O3, as the Release build)")
    parser.add_argument("--library", required=True, help="the Linux libSceAgcDriver.prx built from the same commit")
    parser.add_argument("--objdump", default="x86_64-w64-mingw32-objdump")
    parser.add_argument("--perf", default="perf")
    parser.add_argument("--draw-symbol", default="_ZN9AgcDriver8Graphics4DrawERKNS0_7ContextERKNS0_5StateERKNS_3Pm414DrawParametersESt4spanIKNS0_14CompiledShaderELm18446744073709551615EESB_IKNS0_19GuestMemorySnapshotELm18446744073709551615EEPSt10shared_ptrIKNS_10DrawRecipeEEPKNS0_16DrawTraceContextEm")
    parser.add_argument("--top", type=int, default=30)
    parser.add_argument("--json", help="also write the per-function table here")
    parser.add_argument("command", nargs=argparse.REMAINDER, help="-- workload commands separated by ::")
    args = parser.parse_args()
    command = args.command[1:] if args.command and args.command[0] == "--" else args.command
    runs = [[]]
    for word in command:
        if word == "::":
            runs.append([])
        else:
            runs[-1].append(word)
    runs = [run for run in runs if run]
    if not runs:
        parser.error("give the workload after --")
    functions = static_counts(args.objdump, sorted(Path(args.objects).glob("*.o")))
    calls, probed, candidates = probe_counts(args.perf, args.library, functions, runs, args.draw_symbol)
    draws = calls[args.draw_symbol]
    if draws == 0:
        sys.exit("the workload made no draws")
    totals = collections.Counter()
    variables = collections.Counter()
    rows = []
    for name, counts in functions.items():
        per_draw = calls.get(name, 0) / draws
        if per_draw == 0:
            continue
        weighted = {kind: counts.get(kind, 0) * per_draw for kind in ORDER}
        totals.update(weighted)
        for key, value in counts.items():
            if key.startswith("tls:"):
                variables[key[4:]] += value * per_draw
        rows.append((name, per_draw, weighted))
    rows.sort(key=lambda row: row[2]["emutls"] + 5 * row[2]["rwlock"] + row[2]["mutex"] + row[2]["fls/tls"] + 0.25 * row[2]["atomic"], reverse=True)
    names = demangle([row[0] for row in rows[:args.top]] + [name for name, _ in variables.most_common(args.top)])
    print(f"draws: {draws}; functions with such operations: {candidates} in the MinGW objects, {probed} of them found and probed in the Linux build")
    print("estimated per draw: " + ", ".join(f"{kind} {totals[kind]:.1f}" for kind in ORDER))
    print()
    print("| function | calls per draw | emutls | rwlock | mutex | fls/tls | atomic |")
    print("|---|---|---|---|---|---|---|")
    for name, per_draw, weighted in rows[:args.top]:
        shown = re.sub(r"\(.*", "", names.get(name, name))
        print(f"| `{shown}` | {per_draw:.2f} | " + " | ".join(f"{weighted[kind]:.1f}" for kind in ORDER) + " |")
    print()
    print("| thread_local variable | accesses per draw (sites times calls) |")
    print("|---|---|")
    for name, value in variables.most_common(args.top):
        print(f"| `{names.get(name, name)}` | {value:.1f} |")
    if args.json:
        Path(args.json).write_text(json.dumps({"draws": draws, "functions": {name: {"calls_per_draw": per_draw, **weighted} for name, per_draw, weighted in rows}}, indent=1))


if __name__ == "__main__":
    main()
