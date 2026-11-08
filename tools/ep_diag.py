import os, re, subprocess, sys, glob
exe = sys.argv[1]
extra = sys.argv[2:]
search = [os.path.dirname(exe)] + extra + [r"C:\Windows\System32"] + os.environ["PATH"].split(os.pathsep)
cache = {}
def dump(p):
    if p not in cache:
        cache[p] = subprocess.run(["objdump", "-p", p], capture_output=True, text=True).stdout
    return cache[p]
def imports(p):
    res = {}; cur = None
    for line in dump(p).splitlines():
        m = re.match(r"\s*DLL Name: (\S+)", line)
        if m: cur = m.group(1); res[cur] = []; continue
        if cur:
            m = re.match(r"\s+[0-9a-f]+\s+(?:<none>|\d+)\s+[0-9a-f]+\s+(\S+)", line)
            if m: res[cur].append(m.group(1))
            elif line.strip() and not line.strip().startswith("vma:"): cur = None
    return res
def exports(p):
    out = set(); on = False
    for line in dump(p).splitlines():
        if "Ordinal/Name Pointer" in line: on = True; continue
        if on:
            m = re.match(r"\s+\[\s*\d+\]\s+\+base\[\s*\d+\]\s+[0-9a-f]+\s+(\S+)", line)
            if m: out.add(m.group(1))
            elif line.strip() == "": 
                if out: on = False
    return out
def find(name):
    hits = []
    for d in search:
        c = os.path.join(d, name)
        if os.path.isfile(c) and c.lower() not in [h.lower() for h in hits]: hits.append(c)
    return hits
seen = set()
def walk(p, depth=0):
    if p.lower() in seen: return
    seen.add(p.lower())
    for dll, syms in imports(p).items():
        if dll.lower().startswith("api-ms-") or dll.lower() in ("kernel32.dll", "ntdll.dll", "user32.dll", "advapi32.dll", "msvcrt.dll", "ws2_32.dll", "shell32.dll", "ole32.dll", "bcrypt.dll"): continue
        hits = find(dll)
        print("  " * depth + f"{os.path.basename(p)} -> {dll}: {hits}")
        if not hits: print("  " * depth + "  NOT FOUND"); continue
        ex = exports(hits[0])
        missing = [s for s in syms if s not in ex] if ex else []
        if ex and missing:
            print("  " * depth + f"  MISSING {len(missing)} of {len(syms)} in {hits[0]}: {missing[:15]}")
        walk(hits[0], depth + 1)
walk(exe)
