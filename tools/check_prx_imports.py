import argparse
import struct
import sys
from pathlib import Path

PE_DIRECTORY_OFFSET = {0x20b: 112, 0x10b: 96}
THUNK_FORMAT = {0x20b: ("<Q", 8, 1 << 63), 0x10b: ("<I", 4, 1 << 31)}


class PeError(Exception):
    pass


def take(data, offset, size, path, what):
    if offset < 0 or size < 0 or offset + size > len(data):
        raise PeError(f"{path}: {what} at 0x{offset:x} with size {size} lies outside the file ({len(data)} bytes)")
    return data[offset:offset + size]


def unpack(layout, data, offset, path, what):
    return struct.unpack_from(layout, take(data, offset, struct.calcsize(layout), path, what))


class Image:
    def __init__(self, path):
        self.path = path
        self.data = path.read_bytes()
        if take(self.data, 0, 2, path, "DOS signature") != b"MZ":
            raise PeError(f"{path}: not a PE file (no MZ signature)")
        (pe,) = unpack("<I", self.data, 0x3c, path, "PE header offset")
        if take(self.data, pe, 4, path, "PE signature") != b"PE\0\0":
            raise PeError(f"{path}: not a PE file (no PE signature)")
        _, sections, _, _, _, optional_size, _ = unpack("<HHIIIHH", self.data, pe + 4, path, "COFF header")
        optional = pe + 24
        (self.magic,) = unpack("<H", self.data, optional, path, "optional header magic")
        if self.magic not in PE_DIRECTORY_OFFSET:
            raise PeError(f"{path}: unknown optional header magic 0x{self.magic:x}")
        directories = optional + PE_DIRECTORY_OFFSET[self.magic]
        self.export_rva, self.export_size = unpack("<II", self.data, directories, path, "export directory entry")
        self.import_rva, _ = unpack("<II", self.data, directories + 8, path, "import directory entry")
        self.sections = []
        table = optional + optional_size
        for index in range(sections):
            entry = table + index * 40
            virtual_size, virtual_address, raw_size, raw_offset = unpack("<IIII", self.data, entry + 8, path, "section header")
            self.sections.append((virtual_address, max(virtual_size, raw_size), raw_offset, raw_size))

    def offset(self, rva, what):
        for virtual_address, size, raw_offset, raw_size in self.sections:
            if virtual_address <= rva < virtual_address + size:
                if rva - virtual_address >= raw_size:
                    raise PeError(f"{self.path}: {what} at RVA 0x{rva:x} has no file data")
                return raw_offset + rva - virtual_address
        raise PeError(f"{self.path}: {what} at RVA 0x{rva:x} is in no section")

    def string(self, rva, what):
        start = self.offset(rva, what)
        end = self.data.find(b"\0", start)
        if end < 0:
            raise PeError(f"{self.path}: {what} at RVA 0x{rva:x} is not terminated")
        return self.data[start:end].decode("ascii", "replace")

    def exports(self):
        if self.export_rva == 0:
            return None, set()
        base = self.offset(self.export_rva, "export directory")
        name_rva, = unpack("<I", self.data, base + 12, self.path, "export name")
        names, = unpack("<I", self.data, base + 24, self.path, "export name count")
        names_rva, = unpack("<I", self.data, base + 32, self.path, "export name table")
        table = self.offset(names_rva, "export name table") if names != 0 else 0
        exported = set()
        for index in range(names):
            entry, = unpack("<I", self.data, table + index * 4, self.path, "export name entry")
            exported.add(self.string(entry, "export name"))
        return (self.string(name_rva, "export module name") if name_rva != 0 else None), exported

    def imports(self):
        found = {}
        if self.import_rva == 0:
            return found
        layout, width, ordinal_flag = THUNK_FORMAT[self.magic]
        descriptor = self.offset(self.import_rva, "import directory")
        while True:
            lookup, _, _, name_rva, address = unpack("<IIIII", self.data, descriptor, self.path, "import descriptor")
            if lookup == 0 and name_rva == 0 and address == 0:
                break
            module = self.string(name_rva, "import module name")
            symbols = found.setdefault(module.lower(), [])
            thunk = self.offset(lookup if lookup != 0 else address, "import lookup table")
            while True:
                value, = unpack(layout, self.data, thunk, self.path, "import lookup entry")
                if value == 0:
                    break
                if value & ordinal_flag:
                    symbols.append(f"#{value & 0xffff}")
                else:
                    symbols.append(self.string((value & 0x7fffffff) + 2, "import name"))
                thunk += width
            descriptor += 20
        return found


def check(directory, suffixes):
    images = {}
    for path in sorted(directory.iterdir()):
        if path.is_file() and path.suffix.lower() in suffixes:
            images[path.name.lower()] = Image(path)
    exported = {}
    for name, image in images.items():
        module, symbols = image.exports()
        exported[name] = symbols
        if module is not None:
            exported.setdefault(module.lower(), symbols)
    missing = []
    for name, image in images.items():
        for module, symbols in image.imports().items():
            if module not in exported:
                continue
            for symbol in symbols:
                if not symbol.startswith("#") and symbol not in exported[module]:
                    missing.append((images[name].path.name, module, symbol))
    return len(images), missing


def main(argv=None):
    parser = argparse.ArgumentParser(description="Check that every symbol a library imports from another library in the same directory is exported by it.")
    parser.add_argument("directory", type=Path)
    parser.add_argument("--suffix", action="append", default=None, help="file suffix to check (default .prx and .dll)")
    args = parser.parse_args(argv)
    suffixes = {suffix.lower() for suffix in (args.suffix or [".prx", ".dll"])}
    try:
        count, missing = check(args.directory, suffixes)
    except (OSError, PeError) as error:
        print(error, file=sys.stderr)
        return 2
    for importer, module, symbol in missing:
        print(f"{importer}: imports {symbol} from {module}, which does not export it", file=sys.stderr)
    if count == 0:
        print(f"{args.directory}: no libraries to check", file=sys.stderr)
        return 2
    if missing:
        return 1
    print(f"{count} libraries: every import between them resolves")
    return 0


if __name__ == "__main__":
    sys.exit(main())
