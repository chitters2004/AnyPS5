#!/usr/bin/env python3
import argparse
import collections
import re
import subprocess
import sys
from pathlib import Path

KINDS = {
    "emutls": re.compile(r"__emutls_get_address"),
    "rwlock": re.compile(r"pthread_rwlock_(rdlock|wrlock|unlock|tryrdlock|trywrlock)"),
    "mutex": re.compile(r"pthread_mutex_(lock|unlock|trylock)"),
    "fls/tls": re.compile(r"(FlsGetValue|TlsGetValue|pthread_getspecific)"),
}
FUNCTION = re.compile(r"^[0-9a-f]+ <(.+)>:$")
RELOCATION = re.compile(r"^\s+[0-9a-f]+: (?:R_X86_64|IMAGE_REL_AMD64)\S* +(\S+)")
LOCKED = re.compile(r"^\s+[0-9a-f]+:\s+lock ")
EMUTLS_VARIABLE = re.compile(r"__emutls_v\.(\S+?)(?:[-+]0x[0-9a-f]+)?$")


def static_counts(objdump, objects):
    functions = {}
    for path in objects:
        text = subprocess.run([objdump, "-dr", "-C", "--no-show-raw-insn", str(path)], capture_output=True, text=True, errors="replace").stdout
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
            variable = EMUTLS_VARIABLE.search(symbol)
            if variable:
                current["tls:" + variable.group(1)] += 1
    return functions


def callgrind_calls(path):
    names = {}
    calls = collections.Counter()
    callee = None
    for line in open(path, errors="replace"):
        if line.startswith(("fn=", "cfn=")):
            key, _, rest = line.rstrip("\n").partition("=")
            match = re.match(r"\((\d+)\)(?: (.*))?$", rest)
            if match:
                ident, name = match.groups()
                if name is not None:
                    names[ident] = name
                resolved = names.get(ident, rest)
            else:
                resolved = rest
            if key == "cfn":
                callee = resolved
            continue
        if line.startswith("calls=") and callee is not None:
            calls[callee] += int(line.split("=", 1)[1].split()[0])
    return calls


def base_name(name):
    name = re.sub(r"\[clone [^\]]*\]", "", name).strip()
    depth = 0
    for index, char in enumerate(name):
        if char == "(" and depth == 0:
            return name[:index]
        if char in "<":
            depth += 1
        elif char == ">":
            depth -= 1
    return name


def main():
    parser = argparse.ArgumentParser(description="Estimate Windows-expensive operations per draw: static counts per function from MinGW objects times dynamic calls per draw from a callgrind run of a Linux build.")
    parser.add_argument("--objdump", default="x86_64-w64-mingw32-objdump")
    parser.add_argument("--objects", required=True, help="directory of MinGW objects")
    parser.add_argument("--callgrind", required=True, help="callgrind.out file of a draw workload")
    parser.add_argument("--draw-function", default="AgcDriver::Graphics::Draw", help="function whose call count is the number of draws")
    parser.add_argument("--top", type=int, default=30)
    args = parser.parse_args()
    functions = static_counts(args.objdump, sorted(Path(args.objects).glob("*.o")))
    by_base = collections.defaultdict(collections.Counter)
    for name, counts in functions.items():
        by_base[base_name(name)].update(counts)
    calls = callgrind_calls(args.callgrind)
    calls_by_base = collections.Counter()
    for name, count in calls.items():
        calls_by_base[base_name(name)] += count
    draws = calls_by_base.get(args.draw_function, 0)
    if draws == 0:
        sys.exit(f"no calls to {args.draw_function} in {args.callgrind}")
    totals = collections.Counter()
    rows = []
    variables = collections.Counter()
    for name, counts in by_base.items():
        per_draw = calls_by_base.get(name, 0) / draws
        if per_draw == 0:
            continue
        weighted = {kind: counts[kind] * per_draw for kind in ("emutls", "rwlock", "mutex", "fls/tls", "atomic")}
        if not any(weighted.values()):
            continue
        totals.update(weighted)
        for key, value in counts.items():
            if key.startswith("tls:"):
                variables[key[4:]] += value * per_draw
        rows.append((name, per_draw, weighted))
    print(f"draws ({args.draw_function} calls): {draws}")
    print("estimated per draw: " + ", ".join(f"{kind} {totals[kind]:.1f}" for kind in ("emutls", "rwlock", "mutex", "fls/tls", "atomic")))
    print()
    print("| function | calls per draw | emutls | rwlock | mutex | fls/tls | atomic |")
    print("|---|---|---|---|---|---|---|")
    rows.sort(key=lambda row: (row[2]["emutls"] + row[2]["rwlock"] * 5 + row[2]["mutex"] + row[2]["atomic"] * 0.5), reverse=True)
    for name, per_draw, weighted in rows[:args.top]:
        print(f"| `{name}` | {per_draw:.2f} | {weighted['emutls']:.1f} | {weighted['rwlock']:.1f} | {weighted['mutex']:.1f} | {weighted['fls/tls']:.1f} | {weighted['atomic']:.1f} |")
    print()
    print("| thread_local variable | estimated accesses per draw |")
    print("|---|---|")
    for name, value in variables.most_common(args.top):
        print(f"| `{name}` | {value:.1f} |")


if __name__ == "__main__":
    main()
