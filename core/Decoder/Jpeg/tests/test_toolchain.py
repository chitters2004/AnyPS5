import os
import pathlib
import shutil
import struct
import subprocess
import tempfile
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[4]
CMAKE = shutil.which("cmake")
SHT_RELA = 4
R_X86_64_TLSGD = 19
R_X86_64_TLSLD = 20
R_X86_64_GOTPC32_TLSDESC = 34


def archive_relocations(path):
    data = path.read_bytes()
    if data[:8] != b"!<arch>\n":
        raise ValueError(f"{path.name} is not an ar archive")
    kinds = set()
    at = 8
    while at + 60 <= len(data):
        size = int(data[at + 48:at + 58].decode().strip())
        member = data[at + 60:at + 60 + size]
        at += 60 + size + (size & 1)
        if member[:4] != b"\x7fELF" or member[4] != 2 or member[5] != 1:
            continue
        shoff, = struct.unpack_from("<Q", member, 0x28)
        shentsize, shnum = struct.unpack_from("<HH", member, 0x3A)
        for index in range(shnum):
            _, kind, _, _, offset, length, _, _, _, entsize = struct.unpack_from("<IIQQQQIIQQ", member, shoff + index * shentsize)
            if kind != SHT_RELA:
                continue
            for entry in range(offset, offset + length, entsize or 24):
                info, = struct.unpack_from("<Q", member, entry + 8)
                kinds.add(info & 0xFFFFFFFF)
    return kinds


class JpegToolchain(unittest.TestCase):
    def build(self, configured, descriptors=False):
        self.assertIsNotNone(CMAKE, "CMake is required for the external-project fixture")
        with tempfile.TemporaryDirectory(prefix="jpeg toolchain ") as directory:
            root = pathlib.Path(directory)
            source = root / "source"
            jpeg = source / "3rdparty/libjpeg-turbo"
            (jpeg / "src").mkdir(parents=True)
            (source / "3rdparty/stb").mkdir()
            probe = "int jpeg_toolchain_probe(void) { return 123; }\n"
            if descriptors:
                probe += "_Thread_local int jpeg_toolchain_tls_value;\nint jpeg_toolchain_tls_probe(void) { return ++jpeg_toolchain_tls_value; }\n"
            if descriptors and configured:
                probe = "#if !defined(ANYPS5_JPEG_ENVIRONMENT_PROBE) || ANYPS5_JPEG_ENVIRONMENT_PROBE != 7\n#error nested JPEG build lost CFLAGS\n#endif\n" + probe
            if configured:
                probe = "#if !defined(ANYPS5_JPEG_TOOLCHAIN_PROBE) || ANYPS5_JPEG_TOOLCHAIN_PROBE != 123\n#error nested JPEG build lost the selected toolchain\n#endif\n" + probe
            (jpeg / "probe.c").write_text(probe)
            (jpeg / "CMakeLists.txt").write_text("""cmake_minimum_required(VERSION 3.20)
project(JpegToolchainChild LANGUAGES C)
add_library(turbojpeg STATIC probe.c)
if(MSVC)
    set_target_properties(turbojpeg PROPERTIES OUTPUT_NAME turbojpeg-static)
endif()
install(TARGETS turbojpeg ARCHIVE DESTINATION lib)
""")
            helper = (ROOT / "core/Decoder/Jpeg/CMakeLists.txt").as_posix()
            (source / "CMakeLists.txt").write_text(f"""cmake_minimum_required(VERSION 3.20)
project(JpegToolchainParent LANGUAGES C CXX)
set(BUILD_TESTING OFF)
add_library(probe_runtime INTERFACE)
if({"ON" if descriptors else "OFF"})
    include(CheckCCompilerFlag)
    check_c_compiler_flag(-mtls-dialect=gnu2 ANYPS5_C_HAS_TLS_DESCRIPTORS)
    if(NOT ANYPS5_C_HAS_TLS_DESCRIPTORS)
        file(WRITE "${{CMAKE_BINARY_DIR}}/no-tls-descriptors" "")
    endif()
endif()
include([==[{helper}]==])
add_jpeg_decoder(probe_decoder probe_runtime)
""")
            command = [CMAKE, "-S", str(source), "-B", str(root / "build"), "-DCMAKE_BUILD_TYPE=Release"]
            if configured:
                toolchain = root / "selected toolchain.cmake"
                toolchain.write_text('set(CMAKE_C_FLAGS_INIT "${CMAKE_C_FLAGS_INIT} -DANYPS5_JPEG_TOOLCHAIN_PROBE=123")\n')
                command.append("-DCMAKE_TOOLCHAIN_FILE=" + str(toolchain))
            environment = dict(os.environ)
            if descriptors and configured:
                environment["CFLAGS"] = (environment.get("CFLAGS", "") + " -DANYPS5_JPEG_ENVIRONMENT_PROBE=7").strip()
            self.run_command(command, environment)
            if descriptors and (root / "build/no-tls-descriptors").exists():
                self.skipTest("the C compiler does not take -mtls-dialect=gnu2")
            self.run_command([CMAKE, "--build", str(root / "build"), "--target", "jpeg_turbo_external", "--parallel", "1", "--config", "Release"], environment)
            archives = list((root / "build/libjpeg-turbo/lib").glob("*turbojpeg*"))
            self.assertTrue(archives)
            self.assertTrue(all(path.stat().st_size > 0 for path in archives))
            if descriptors:
                kinds = set().union(*(archive_relocations(path) for path in archives))
                self.assertIn(R_X86_64_GOTPC32_TLSDESC, kinds, "the nested JPEG build did not compile with -mtls-dialect=gnu2")
                self.assertFalse(kinds & {R_X86_64_TLSGD, R_X86_64_TLSLD}, "the nested JPEG build still reaches thread-local storage through __tls_get_addr")

    def run_command(self, command, environment=None):
        result = subprocess.run(command, env=environment, text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        self.assertEqual(result.returncode, 0, " ".join(command) + "\n" + result.stdout[-7000:])

    def test_selected_toolchain_reaches_nested_compilation(self):
        self.build(True)

    def test_tls_descriptors_keep_selected_toolchain(self):
        self.build(True, True)

    def test_tls_descriptors_without_toolchain(self):
        self.build(False, True)

    def test_native_build_without_toolchain_still_builds(self):
        self.build(False)


if __name__ == "__main__":
    unittest.main()
