#!/bin/bash
set -u
if [ $# -lt 2 ]; then
    echo "usage: run_all_agc.sh <label> <output directory> [extra ctest arguments]" >&2
    exit 2
fi
label=$1
out=$2
shift 2
root=$(git -C "$(dirname "$0")" rev-parse --show-toplevel)
mkdir -p "$out"
export ANYPS5_REQUIRE_VULKAN=1 ANYPS5_NO_SHADER_CACHE=1 VK_INSTANCE_LAYERS=VK_LAYER_KHRONOS_validation
export VK_ICD_FILENAMES=${VK_ICD_FILENAMES:-/usr/share/vulkan/icd.d/lvp_icd.json}
cd "$root"
ctest --test-dir build -R '^agc_' -E 'fuzz' -V -j"$(nproc)" --timeout 900 "$@" > "$out/$label.log" 2>&1
grep -E "^\s*[0-9]+/[0-9]+ Test +#[0-9]+: " "$out/$label.log" | sed -E 's/^\s*[0-9]+\/[0-9]+ Test +#[0-9]+: ([^ ]+) \.+ *([A-Za-z*]+).*/\1 \2/' | sort > "$out/$label.results"
ctest --test-dir build -N | sed -nE 's/^\s*Test +#([0-9]+): (.*)$/\1 \2/p' > "$out/$label.names"
python3 - "$out/$label.log" "$out/$label.names" "$out/$label.pertest" <<'PY'
import collections, re, sys
names = dict(line.split(' ', 1) for line in open(sys.argv[2]).read().split('\n') if line)
counts = collections.Counter()
for line in open(sys.argv[1], errors='replace'):
    match = re.match(r'^(\d+): ', line)
    if not match:
        continue
    for vuid in re.findall(r'VUID-[A-Za-z0-9_-]+', line):
        counts[(names.get(match.group(1), match.group(1)).strip(), vuid)] += 1
with open(sys.argv[3], 'w') as out:
    for (name, vuid), count in sorted(counts.items()):
        out.write(f"{name} {vuid} {count}\n")
PY
missing=$(grep -E "^[0-9]+: .*was not found but was requested by env var VK_INSTANCE_LAYERS" "$out/$label.log" | grep -vc "Regex=")
echo "$label: $(grep -E '^[0-9]+% tests passed' "$out/$label.log"); missing-layer lines: $missing"
