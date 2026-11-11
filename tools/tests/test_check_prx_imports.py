import io
import struct
import sys
import tempfile
import unittest
from contextlib import redirect_stderr, redirect_stdout
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import check_prx_imports

SECTION_RVA = 0x1000
SECTION_FILE = 0x200


class Payload:
    def __init__(self):
        self.data = bytearray()

    def put(self, data):
        rva = SECTION_RVA + len(self.data)
        self.data += data
        while len(self.data) % 8:
            self.data += b"\0"
        return rva

    def string(self, text):
        return self.put(text.encode("ascii") + b"\0")


def image(module, exports=(), imports=None, ordinal_imports=None):
    payload = Payload()
    export_rva = 0
    export_size = 0
    if exports or module is not None:
        name_rvas = [payload.string(name) for name in exports]
        names_table = payload.put(b"".join(struct.pack("<I", rva) for rva in name_rvas)) if name_rvas else 0
        module_rva = payload.string(module) if module is not None else 0
        export_rva = payload.put(struct.pack("<IIHHIIIIIII", 0, 0, 0, 0, module_rva, 1, len(exports), len(exports), 0, names_table, 0))
        export_size = 40
    import_rva = 0
    imports = dict(imports or {})
    ordinal_imports = dict(ordinal_imports or {})
    modules = sorted(set(imports) | set(ordinal_imports))
    if modules:
        descriptors = []
        for name in modules:
            thunks = [payload.put(struct.pack("<H", 0) + symbol.encode("ascii") + b"\0") for symbol in imports.get(name, [])]
            thunks += [(1 << 63) | ordinal for ordinal in ordinal_imports.get(name, [])]
            table = payload.put(b"".join(struct.pack("<Q", thunk) for thunk in thunks) + struct.pack("<Q", 0))
            descriptors.append(struct.pack("<IIIII", table, 0, 0, payload.string(name), table))
        import_rva = payload.put(b"".join(descriptors) + struct.pack("<IIIII", 0, 0, 0, 0, 0))
    raw = bytes(payload.data)
    raw_size = (len(raw) + 0x1ff) & ~0x1ff
    optional_size = 112 + 16 * 8
    header = bytearray(SECTION_FILE)
    header[0:2] = b"MZ"
    struct.pack_into("<I", header, 0x3c, 0x40)
    header[0x40:0x44] = b"PE\0\0"
    struct.pack_into("<HHIIIHH", header, 0x44, 0x8664, 1, 0, 0, 0, optional_size, 0x2022)
    optional = 0x58
    struct.pack_into("<H", header, optional, 0x20b)
    directories = optional + 112
    struct.pack_into("<II", header, directories, export_rva, export_size)
    struct.pack_into("<II", header, directories + 8, import_rva, 20 * (len(modules) + 1) if modules else 0)
    section = optional + optional_size
    header[section:section + 8] = b".data\0\0\0"
    struct.pack_into("<IIII", header, section + 8, len(raw), SECTION_RVA, raw_size, SECTION_FILE)
    return bytes(header) + raw + b"\0" * (raw_size - len(raw))


class CheckPrxImportsTest(unittest.TestCase):
    def run_check(self, files, *extra):
        with tempfile.TemporaryDirectory() as directory:
            for name, data in files.items():
                (Path(directory) / name).write_bytes(data)
            out = io.StringIO()
            err = io.StringIO()
            with redirect_stdout(out), redirect_stderr(err):
                code = check_prx_imports.main([directory, *extra])
            return code, out.getvalue(), err.getvalue()

    def test_resolved_imports_pass(self):
        code, out, err = self.run_check({
            "libSceAgcDriver.prx": image("libSceAgcDriver.prx", ["_ZN9AgcDriver4WaitEv", "_ZN9AgcDriver4WakeEv"]),
            "libSceVideoOut.prx": image("libSceVideoOut.prx", ["sceVideoOutOpen"], {"libSceAgcDriver.prx": ["_ZN9AgcDriver4WaitEv"], "KERNEL32.dll": ["GetLastError"]}),
        })
        self.assertEqual(code, 0, err)
        self.assertIn("2 libraries", out)

    def test_missing_export_is_reported(self):
        code, _, err = self.run_check({
            "libSceAgcDriver.prx": image("libSceAgcDriver.prx", ["_ZN9AgcDriver4WaitEv"]),
            "libSceVideoOut.prx": image("libSceVideoOut.prx", [], {"libSceAgcDriver.prx": ["_ZN9AgcDriver4WaitEv", "_ZN9AgcDriver4WakeEv"]}),
        })
        self.assertEqual(code, 1)
        self.assertIn("libSceVideoOut.prx: imports _ZN9AgcDriver4WakeEv from libsceagcdriver.prx, which does not export it", err)
        self.assertNotIn("4WaitEv from", err)

    def test_module_matched_by_export_name_and_case(self):
        code, _, err = self.run_check({
            "renamed.prx": image("LIBSCEAGCDRIVER.PRX", ["Wait"]),
            "user.prx": image("user.prx", [], {"libSceAgcDriver.prx": ["Wait", "Wake"]}),
        })
        self.assertEqual(code, 1)
        self.assertIn("imports Wake", err)

    def test_imports_from_other_modules_and_ordinals_are_skipped(self):
        code, _, err = self.run_check({
            "a.prx": image("a.prx", ["f"]),
            "b.prx": image("b.prx", [], {"KERNEL32.dll": ["Missing"]}, {"a.prx": [7]}),
        })
        self.assertEqual(code, 0, err)

    def test_dll_suffix_and_custom_suffix(self):
        files = {"a.dll": image("a.dll", ["f"]), "b.dll": image("b.dll", [], {"a.dll": ["g"]})}
        code, _, _ = self.run_check(files)
        self.assertEqual(code, 1)
        code, _, err = self.run_check(files, "--suffix", ".prx")
        self.assertEqual(code, 2)
        self.assertIn("no libraries to check", err)

    def test_malformed_file_is_an_error(self):
        code, _, err = self.run_check({"broken.prx": b"MZ" + b"\0" * 10})
        self.assertEqual(code, 2)
        self.assertIn("broken.prx", err)
        code, _, err = self.run_check({"text.prx": b"not a library"})
        self.assertEqual(code, 2)
        self.assertIn("no MZ signature", err)


if __name__ == "__main__":
    unittest.main()
