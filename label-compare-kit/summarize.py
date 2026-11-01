import argparse
import collections
import json
import re
import sys
from pathlib import Path

SHADOW = re.compile(r"\[label-compare\] waits checked (\d+), eligible (\d+) \(([\d.]+) ms waited\), refused (\d+) \(([\d.]+) ms waited:(.*?)\); blocks store (\d+) keep (\d+) gpu (\d+) gpu-lease (\d+) fallback (\d+); decisions compared (\d+), mismatches (\d+)")
GPU = re.compile(r"\[label-compare\] GPU compare: waits skipped (\d+), blocks store (\d+) keep (\d+) gpu (\d+) gpu-lease (\d+); GPU-compared blocks stored (\d+) kept (\d+); images in sync at completion (\d+), left stale (\d+); waits kept:(.*)$")
REASON = re.compile(r"(\S+) (\d+)/([\d.]+) ms")
MISMATCH = re.compile(r"\[label-compare\] mismatch ")
IMPORTS = re.compile(r"\[write-watch\] host imports.*")
TEST_RESULT = re.compile(r"^\s*\d+/\d+ Test\s+#(\d+): (\S+) [ .]*(Passed|\*\*\*Failed|\*\*\*Skipped|Not Run|Subprocess aborted|\*\*\*Timeout|\*\*\*Exception)")
TEST_START = re.compile(r"^\s*Start\s+(\d+): (\S+)")
VUID = re.compile(r"^(\d+): .*?(VUID-[A-Za-z0-9_-]+)")
LAYER_MISSING = "was not found but was requested by env var VK_INSTANCE_LAYERS"

NEXT_STEP = {
    "fallback-lease-in-flight": "per-use lease tables (report: Extensions, in-flight leases)",
    "fallback-edge": "exact model of the 4-piece tracker for edge blocks (report: Extensions, edge blocks)",
    "fallback-lease-untabled": "find which leases have no fault checks; their tables are not registered",
    "fallback-lease-table-shared": "per-use lease tables (a table shared with a submitted batch)",
    "fallback-lease-no-table": "check lease table registration (ShaderResources::MarkGpuWrites)",
    "large": "device-side baseline and in-place scratch reads (report: The size limit)",
    "units": "block units above 64 KiB: gate the unit-shadow publish (report: Next steps)",
    "keys": "DCC images with a test (report: Next steps)",
    "no-baseline": "block-unit images without a baseline: fill uncovered bytes from the import on the GPU",
    "late-stamping-writer": "model non-reusable writers' completion stamps instead of waiting",
    "copied-writers": "copied writers write back on the CPU at completion; needs their own design",
    "completion-stores": "completion-deferred labels or stores pending; needs their own design",
    "drawn": "drawn linear targets store through their drawn rectangle; not covered",
    "import": "the image has no host import",
    "shadowed": "unit shadows over the image",
    "shadowed-range": "unit shadows over the flushed range",
    "unsaved": "compared memory without a saved snapshot",
    "save-pending": "compared memory with a stored save still in flight",
    "pipeline": "the conditional store pipeline failed to build; check the log",
}


def parse_gta(path):
    shadow = collections.Counter()
    gpu = collections.Counter()
    refused = collections.defaultdict(lambda: [0, 0.0])
    kept = collections.defaultdict(lambda: [0, 0.0])
    mismatches = []
    imports = []
    lines = 0
    for line in Path(path).read_text(errors="replace").splitlines():
        if IMPORTS.search(line):
            imports.append(IMPORTS.search(line).group(0))
        if MISMATCH.search(line):
            mismatches.append(line.strip())
        m = SHADOW.search(line)
        if m:
            lines += 1
            keys = ["checked", "eligible", "eligible_ms", "refused", "refused_ms"]
            for key, value in zip(keys, m.groups()[:5]):
                shadow[key] += float(value)
            for why, count, ms in REASON.findall(m.group(6)):
                refused[why][0] += int(count)
                refused[why][1] += float(ms)
            for key, value in zip(["store", "keep", "gpu", "gpu-lease", "fallback", "compared", "mismatches"], m.groups()[6:]):
                shadow[key] += int(value)
            continue
        m = GPU.search(line)
        if m:
            lines += 1
            for key, value in zip(["skipped", "store", "keep", "gpu", "gpu-lease", "stored", "kept", "in_sync", "stale"], m.groups()[:9]):
                gpu[key] += int(value)
            for why, count, ms in REASON.findall(m.group(10)):
                kept[why][0] += int(count)
                kept[why][1] += float(ms)
    return {"lines": lines, "shadow": shadow, "gpu": gpu, "refused": refused, "kept": kept, "mismatches": mismatches, "imports": imports}


def read_summaries(paths):
    rows = []
    for path in paths:
        for line in Path(path).read_text(errors="replace").splitlines():
            line = line.strip()
            if not line:
                continue
            try:
                row = json.loads(line)
            except json.JSONDecodeError:
                continue
            if row.get("trace") == "" and "interval_start_s" in row:
                continue
            rows.append(row)
    return rows


def label_of(row):
    env = row.get("env", {})
    if env.get("APS5_LABEL_GPU_COMPARE") == "1":
        mode = "gpu"
    elif env.get("APS5_LABEL_COMPARE_SHADOW"):
        mode = "shadow"
    else:
        mode = "off"
    return f"{row.get('commit', '?')} {mode}"


def ranked(table):
    return sorted(table.items(), key=lambda item: (-item[1][1], -item[1][0], item[0]))


def report_gta(args):
    out = []
    any_mismatch = False
    for path in args.logs:
        result = parse_gta(path)
        out.append(f"== {path}")
        for line in result["imports"][:3]:
            out.append(f"   {line}")
        if not result["imports"]:
            out.append("   no [write-watch] host imports line (imports are compared when the write watch is off)")
        if result["lines"] == 0:
            out.append("   no [label-compare] lines: set APS5_LABEL_COMPARE_SHADOW=1 or APS5_LABEL_GPU_COMPARE=1 and run past 10 s of label stores")
            continue
        s, g = result["shadow"], result["gpu"]
        if s["checked"]:
            out.append(f"   shadow: {int(s['checked'])} waits checked, {int(s['eligible'])} eligible ({s['eligible_ms']:.1f} ms waited), {int(s['refused'])} refused ({s['refused_ms']:.1f} ms waited)")
            out.append(f"   shadow blocks: store {s['store']} keep {s['keep']} gpu {s['gpu']} gpu-lease {s['gpu-lease']} fallback {s['fallback']}; decisions compared {s['compared']}, mismatches {s['mismatches']}")
        if g["skipped"] or result["kept"]:
            out.append(f"   GPU compare: {g['skipped']} waits skipped; GPU ranges stored {g['stored']} kept {g['kept']}; images in sync at completion {g['in_sync']}, left stale {g['stale']}")
        if s["mismatches"] or result["mismatches"]:
            any_mismatch = True
            out.append("   STOP: the GPU rule disagreed with the waited decision; send these lines:")
            out.extend(f"     {line}" for line in result["mismatches"][:32])
        for title, table in (("shadow refusals", result["refused"]), ("GPU compare waits kept", result["kept"])):
            if not table:
                continue
            out.append(f"   {title} (count, ms waited), most time first:")
            for why, (count, ms) in ranked(table):
                out.append(f"     {why:32} {count:8d} {ms:10.1f} ms   next: {NEXT_STEP.get(why, '?')}")
    summaries = read_summaries(args.summary) if args.summary else []
    if summaries:
        out.append("== frame summaries (aps5-summary.jsonl window lines)")
        out.append(f"   {'build and mode':28} {'p50':>8} {'p90':>8} {'p99':>8} {'gpu ms':>8} presents")
        base = None
        for row in summaries:
            frame = row.get("frame_ms", {})
            p50 = frame.get("p50")
            if base is None and label_of(row).endswith(" off"):
                base = p50
            delta = f" ({p50 - base:+.1f} ms vs first off)" if base is not None and p50 is not None else ""
            out.append(f"   {label_of(row):28} {frame.get('p50', 0):8.1f} {frame.get('p90', 0):8.1f} {frame.get('p99', 0):8.1f} {row.get('gpu_busy_ms_per_present', 0):8.1f} {row.get('presents', 0)}{delta}")
    print("\n".join(out))
    return 2 if any_mismatch else 0


def parse_tests(path):
    names = {}
    results = {}
    vuids = collections.Counter()
    missing = 0
    text = Path(path).read_text(errors="replace").splitlines()
    for line in text:
        m = TEST_START.match(line) or TEST_RESULT.match(line)
        if m:
            names[m.group(1)] = m.group(2)
        m = TEST_RESULT.match(line)
        if m:
            results[m.group(2)] = m.group(3).replace("*", "")
        if LAYER_MISSING in line:
            missing += 1
    for line in text:
        m = VUID.match(line)
        if m:
            vuids[(names.get(m.group(1), m.group(1)), m.group(2))] += 1
    return results, vuids, missing


def report_tests(args):
    base_results, base_vuids, base_missing = parse_tests(args.main)
    out = []
    status = 0
    for path in args.head:
        results, vuids, missing = parse_tests(path)
        out.append(f"== {args.main} vs {path}")
        out.append(f"   tests: main {len(base_results)}, head {len(results)}; layer missing: main {base_missing}, head {missing}")
        if base_missing or missing:
            status = 1
            out.append("   the validation layer was not loaded in at least one run: install the Vulkan SDK layer and rerun")
        changed = [(name, base_results[name], results[name]) for name in sorted(base_results) if name in results and base_results[name] != results[name]]
        added = sorted(set(results) - set(base_results))
        removed = sorted(set(base_results) - set(results))
        for name, before, after in changed:
            status = 1
            out.append(f"   changed: {name}: {before} -> {after}")
        for name in added:
            out.append(f"   new: {name}: {results[name]}")
            if results[name] != "Passed" and results[name] != "Skipped":
                status = 1
        for name in removed:
            out.append(f"   missing on head: {name}")
        new_vuids = [(key, count, base_vuids.get(key, 0)) for key, count in sorted(vuids.items()) if count > base_vuids.get(key, 0) and key[0] in base_results]
        for (name, vuid), count, before in new_vuids:
            status = 1
            out.append(f"   VUID increase: {name} {vuid}: {before} -> {count}")
        for (name, vuid), count in sorted(vuids.items()):
            if name not in base_results:
                out.append(f"   VUID in new test: {name} {vuid}: {count}")
        if not changed and not new_vuids:
            out.append("   identical outcomes and no new VUID on tests common to both")
    print("\n".join(out))
    return status


SAMPLE = """[write-watch] host imports of write-watched memory keep the write watch: no (0 of 4 scratch pages reported written at the import); direct host imports of private memory use comparisons, separate shared aliases retain guest write tracking
[label-compare] waits checked 3, eligible 1 (2.5 ms waited), refused 2 (7.0 ms waited: fallback-lease-in-flight 1/6.0 ms no-baseline 1/1.0 ms); blocks store 1 keep 0 gpu 2 gpu-lease 0 fallback 1; decisions compared 3, mismatches 0
[label-compare] GPU compare: waits skipped 4, blocks store 0 keep 1 gpu 6 gpu-lease 1; GPU-compared blocks stored 5 kept 2; images in sync at completion 1, left stale 3; waits kept: large 2/9.5 ms
[label-compare] waits checked 1, eligible 0 (0.0 ms waited), refused 1 (3.0 ms waited: fallback-lease-in-flight 1/3.0 ms); blocks store 0 keep 0 gpu 0 gpu-lease 0 fallback 1; decisions compared 0, mismatches 0
"""


def self_test():
    import tempfile
    with tempfile.TemporaryDirectory() as folder:
        log = Path(folder) / "gpu.log"
        log.write_text(SAMPLE)
        result = parse_gta(log)
        assert result["lines"] == 3, result
        assert result["shadow"]["checked"] == 4 and result["shadow"]["refused_ms"] == 10.0, result["shadow"]
        assert result["refused"]["fallback-lease-in-flight"] == [2, 9.0], result["refused"]
        assert result["refused"]["no-baseline"] == [1, 1.0], result["refused"]
        assert result["gpu"]["skipped"] == 4 and result["gpu"]["stale"] == 3, result["gpu"]
        assert result["kept"]["large"] == [2, 9.5], result["kept"]
        assert len(result["imports"]) == 1 and not result["mismatches"]
        bad = Path(folder) / "bad.log"
        bad.write_text(SAMPLE + "[label-compare] mismatch image 0x1 block 0 mode gpu changed-after 1 written 0 for readback before label\n")
        assert parse_gta(bad)["mismatches"], "a mismatch line was not found"
        main = Path(folder) / "main.log"
        head = Path(folder) / "head.log"
        main.write_text("      Start 5: agc_a\n5: VUID-VkDeviceCreateInfo-pNext-pNext x\n 1/2 Test  #5: agc_a ....   Passed    0.1 sec\n 2/2 Test  #6: agc_b ....***Failed  0.1 sec\n")
        head.write_text("      Start 7: agc_a\n7: VUID-VkDeviceCreateInfo-pNext-pNext x\n7: VUID-vkCmdCopyBuffer-srcBuffer-00118 x\n 1/3 Test  #7: agc_a ....   Passed    0.1 sec\n 2/3 Test  #8: agc_b ....***Failed  0.1 sec\n 3/3 Test  #9: agc_c ....   Passed    0.1 sec\n")
        results, vuids, missing = parse_tests(head)
        assert results == {"agc_a": "Passed", "agc_b": "Failed", "agc_c": "Passed"}, results
        assert vuids[("agc_a", "VUID-vkCmdCopyBuffer-srcBuffer-00118")] == 1 and missing == 0, vuids
        status = report_tests(argparse.Namespace(main=main, head=[head]))
        assert status == 1, "a new VUID on a common test must fail the comparison"
    print("summarize.py self-test passed")
    return 0


def main():
    parser = argparse.ArgumentParser(description="Summarize label store GPU compare runs and test logs.")
    sub = parser.add_subparsers(dest="command")
    gta = sub.add_parser("gta", help="digest [label-compare] lines from title logs, and frame summaries")
    gta.add_argument("logs", nargs="+", help="stderr logs of title runs")
    gta.add_argument("--summary", nargs="*", help="aps5-summary.jsonl files")
    tests = sub.add_parser("tests", help="compare ctest -V logs of main and the branch by test name")
    tests.add_argument("main", help="ctest -V log of main")
    tests.add_argument("head", nargs="+", help="ctest -V logs of the branch (compare off, on)")
    sub.add_parser("self-test", help="check the parsers on built-in samples")
    args = parser.parse_args()
    if args.command == "gta":
        return report_gta(args)
    if args.command == "tests":
        return report_tests(args)
    if args.command == "self-test":
        return self_test()
    parser.print_help()
    return 1


if __name__ == "__main__":
    sys.exit(main())
