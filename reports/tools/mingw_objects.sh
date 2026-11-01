#!/bin/bash
set -u
if [ $# -ne 1 ]; then
    echo "usage: mingw_objects.sh <output directory>; needs a configured Linux build in build/ and g++-mingw-w64-x86-64-posix" >&2
    exit 2
fi
out=$(realpath -m "$1")
root=$(git -C "$(dirname "$0")" rev-parse --show-toplevel)
mkdir -p "$out/shim/prx/libc/include" "$out/obj"
sed 's/entry->Processor.EfficiencyClass/0u/' "$root/core/libs/prx/libc/include/CpuTopology.hpp" > "$out/shim/prx/libc/include/CpuTopology.hpp"
sed 's/::_fflush_nolock(native)/::fflush(native)/' "$root/core/libs/prx/libc/include/FileStream.hpp" > "$out/shim/prx/libc/include/FileStream.hpp"
ninja -C "$root/build" -t commands libSceAgcDriver | grep -E "g\+\+ .* -c $root/core" > "$out/commands.txt"
python3 - "$out" "$root" <<'PY'
import re, sys
out, root = sys.argv[1], sys.argv[2]
with open(out + "/compile.sh", "w") as script:
    for line in open(out + "/commands.txt"):
        line = line.strip()
        source = re.search(r" -c (\S+)$", line).group(1)
        name = source.split("/core/", 1)[1].replace("/", "__")
        command = line.replace("ccache /usr/bin/g++", "x86_64-w64-mingw32-g++-posix -I" + out + "/shim", 1)
        command = re.sub(r" -fPIC", "", command)
        command = re.sub(r" -MD -MT \S+ -MF \S+", "", command)
        command = re.sub(r" -o \S+", " -o " + out + "/obj/" + name + ".o", command)
        script.write(command + " > " + out + "/obj/" + name + ".log 2>&1 || echo FAIL " + name + "\n")
PY
cd "$root/build"
tr '\n' '\0' < "$out/compile.sh" | xargs -0 -P"$(nproc)" -I{} bash -c '{}' > "$out/failures.txt"
echo "$(grep -c FAIL "$out/failures.txt") of $(wc -l < "$out/commands.txt") sources failed (Linux-only libc/libkernel sources and Windows SEH assembler stubs are expected); objects in $out/obj"
