import bisect, os, re, subprocess, sys

nm, objdump, executable = sys.argv[1], sys.argv[2], sys.argv[3]
modules = {os.path.basename(path).lower(): path for path in sys.argv[3:]}
run = subprocess.run([executable], stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
output = run.stdout.decode(errors="replace")
sys.stdout.write(output)
sys.stdout.flush()
code = run.returncode
if code == 0:
    sys.exit(0)
print(f"[symbolize] exit code {code & 0xffffffff:#x}")
offsets = {}
for name, offset in re.findall(r"([A-Za-z0-9_.+-]+\.(?:exe|prx|dll))\+0x([0-9a-fA-F]+)", output):
    if name.lower() in modules:
        offsets.setdefault(name.lower(), set()).add(int(offset, 16))
for name, wanted in offsets.items():
    path = modules[name]
    header = subprocess.run([objdump, "-p", path], stdout=subprocess.PIPE, stderr=subprocess.STDOUT).stdout.decode(errors="replace")
    base = re.search(r"ImageBase\s+([0-9a-fA-F]+)", header)
    if base is None:
        print(f"[symbolize] {name}: no ImageBase")
        continue
    base = int(base.group(1), 16)
    table = subprocess.run([nm, "-C", "-n", "--defined-only", path], stdout=subprocess.PIPE, stderr=subprocess.STDOUT).stdout.decode(errors="replace")
    symbols = []
    for line in table.splitlines():
        parts = line.split(" ", 2)
        if len(parts) == 3 and parts[1] in "tTwW" and re.fullmatch(r"[0-9a-fA-F]+", parts[0]):
            symbols.append((int(parts[0], 16), parts[2]))
    addresses = [address for address, _ in symbols]
    print(f"[symbolize] {name} ImageBase {base:#x}, {len(symbols)} text symbols")
    for offset in sorted(wanted):
        address = base + offset
        index = bisect.bisect_right(addresses, address) - 1
        if index < 0:
            print(f"[symbolize] {name}+{offset:#x}: before the first symbol")
            continue
        start, symbol = symbols[index]
        print(f"[symbolize] {name}+{offset:#x} = {symbol} + {address - start:#x}")
sys.exit(1)
