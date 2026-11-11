import pathlib
import re
import sys

ROOTS = ("Graphics", "Execution")
MEMBER = re.compile(r"([A-Za-z_][A-Za-z_0-9]*)\s*(?:\.|->)\s*pushConstants\b(\s*\.\s*(?:size|empty)\s*\(\s*\))?")
COPY = re.compile(r"RecompileResult\s*>\s*\(\s*\*|RecompileResult\s*\(\s*\*")
OTHER_MEMBERS = {
    ("Execution/src/VulkanDevice.cpp", "capture"),
    ("Execution/src/ComputePipelineCapture.cpp", "result"),
    ("Graphics/src/PipelineJournal.cpp", "entry"),
}
HELPER_LINES = {
    ("Graphics/include/Shaders.hpp", "return push != nullptr ? *push : program.pushConstants;"),
    ("Graphics/include/Shaders.hpp", "return copy.pushConstants;"),
    ("Graphics/include/Shaders.hpp", "if (push != nullptr) copy.pushConstants = *push;"),
}
SIZE_ONLY_COPIES = {"Graphics/src/PipelineJournal.cpp"}


def scan(root):
    failures = []
    for top in ROOTS:
        for path in sorted((root / top).rglob("*")):
            if path.suffix not in (".cpp", ".hpp", ".h"):
                continue
            relative = path.relative_to(root).as_posix()
            for number, line in enumerate(path.read_text(encoding="utf-8", errors="replace").splitlines(), 1):
                if (relative, line.strip()) in HELPER_LINES:
                    continue
                for match in MEMBER.finditer(line):
                    if match.group(2) is not None or (relative, match.group(1)) in OTHER_MEMBERS:
                        continue
                    failures.append(f"{relative}:{number}: reads {match.group(1)}'s push constant bytes directly; read them with Graphics::PushBytes so a stage that shares its push donor gives its own bytes")
                if COPY.search(line) and relative not in SIZE_ONLY_COPIES:
                    failures.append(f"{relative}:{number}: copies a compiled shader directly; copy it with Graphics::CopyCompiled so a stage that shares its push donor keeps its own bytes")
    return failures


def main():
    root = pathlib.Path(sys.argv[1]) if len(sys.argv) > 1 else pathlib.Path(__file__).resolve().parents[1]
    failures = scan(root)
    for failure in failures:
        print(failure)
    if failures:
        return 1
    print(f"push bytes readers: every push constant value read and compiled shader copy in {', '.join(ROOTS)} goes through PushBytes or CopyCompiled")
    return 0


if __name__ == "__main__":
    sys.exit(main())
