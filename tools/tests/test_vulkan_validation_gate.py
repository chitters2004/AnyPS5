import contextlib
import io
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import time
import unittest
import unittest.mock

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import vulkan_validation_gate as gate

PYTHON = Path(sys.executable).as_posix()


class ReportTests(unittest.TestCase):
    def test_effective_timeouts_cannot_override_the_requested_limit(self):
        for value in (0, 2, -1, float("inf"), float("nan"), "1", True):
            with self.subTest(value=value), self.assertRaisesRegex(gate.GateError, "probe: TIMEOUT"):
                gate.check_timeouts([{"name": "probe", "properties": [{"name": "TIMEOUT", "value": value}]}], 1)
        for value in (.05, 1):
            gate.check_timeouts([{"name": "probe", "properties": [{"name": "TIMEOUT", "value": value}]}], 1)
        gate.check_timeouts([{"name": "default", "properties": []}], 1)
        with self.assertRaisesRegex(gate.GateError, "TIMEOUT_AFTER_MATCH"):
            gate.check_timeouts([{"name": "probe", "properties": [{"name": "TIMEOUT_AFTER_MATCH", "value": ["1", "ready"]}]}], 1)

    def report(self, text):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "junit.xml"
            path.write_text(text, encoding="utf-8")
            return gate.parse_report(path, {"probe"})

    def test_test_body_messages_are_distinct_from_configuration_echoes(self):
        text = '<testsuite><testcase name="probe"><system-out>'
        text += 'environment: VK_INSTANCE_LAYERS=VUID-Fake-1\n'
        text += 'Validation Error: [ VUID-Real-1 ] message\n'
        text += 'Validation Warning: [ SYNC-HAZARD-WRITE_AFTER_WRITE ] message\n'
        text += 'VUID-Old-2 (ERROR / SPEC): msgNum: 1\n'
        text += '</system-out><system-err>WARNING: [Loader Message] layer was not found but was requested by env var VK_INSTANCE_LAYERS</system-err></testcase></testsuite>'
        tests, problems = self.report(text)
        self.assertEqual(tests["probe"]["diagnostics"], {"VUID-Real-1": 1, "SYNC-HAZARD-WRITE_AFTER_WRITE": 1, "VUID-Old-2": 1})
        self.assertEqual(len(tests["probe"]["missing_layers"]), 1)
        self.assertEqual(problems, ["probe"])

    def test_a_test_without_the_layer_insertion_line_is_not_a_success(self):
        inserted = '[Vulkan Loader] INFO | LAYER:   Insert instance layer "VK_LAYER_KHRONOS_validation" (layer.json)'
        for line, problems in ((inserted, []), ('Insert instance layer "VK_LAYER_OTHER" (layer.json)', ["probe"]), ("", ["probe"])):
            with self.subTest(line=line):
                tests, found = self.report('<testsuite><testcase name="probe"><system-err>' + line + '</system-err></testcase></testsuite>')
                self.assertEqual(tests["probe"]["layer_inserted"], not problems)
                self.assertEqual(found, problems)

    def test_skip_disabled_failure_and_missing_tests_are_not_successes(self):
        for body in ('<testcase name="probe"><skipped/></testcase>',
                     '<testcase name="probe" status="disabled"/>',
                     '<testcase name="probe"><failure/></testcase>', ''):
            with self.subTest(body=body):
                self.assertTrue(self.report('<testsuite>' + body + '</testsuite>')[1])

    def test_invalid_or_duplicate_test_names_are_setup_errors(self):
        for body in ('<testcase/>', '<testcase name="probe"/><testcase name="probe"/>'):
            with self.subTest(body=body), self.assertRaises(gate.GateError):
                self.report('<testsuite>' + body + '</testsuite>')

    def test_new_ids_are_compared_per_test_and_counts_are_retained(self):
        def iteration(side, test, messages):
            return {"side": side, "iteration": 1, "returncode": 0, "problems": [],
                    "tests": {test: {"diagnostics": messages}}}
        report = gate.compare([iteration("baseline", "probe", {"VUID-A-1": 1}),
                               iteration("candidate", "probe", {"VUID-A-1": 3})])
        self.assertEqual(report["exit_code"], 0)
        self.assertEqual(report["counts"]["candidate"]["probe"]["VUID-A-1"], 3)
        report = gate.compare([iteration("baseline", "other", {"VUID-A-1": 1}),
                               iteration("candidate", "probe", {"VUID-A-1": 1})])
        self.assertEqual(report["new_diagnostics"], {"probe": ["VUID-A-1"]})
        self.assertEqual(report["exit_code"], 1)

    def test_relative_executable_follows_the_test_working_directory(self):
        for mode in ("default", "absolute", "relative"):
            with self.subTest(mode=mode), tempfile.TemporaryDirectory() as directory:
                build = Path(directory).resolve()
                working = build if mode == "default" else build / "working directory"
                working.mkdir(exist_ok=True)
                executable = working / "probe"
                executable.write_bytes(b"original executable")
                properties = [] if mode == "default" else [{"name": "WORKING_DIRECTORY",
                    "value": str(working if mode == "absolute" else working.relative_to(build))}]
                tests = [{"name": "probe", "command": ["./probe"], "properties": properties}]
                before = gate.artifact_hashes(build, tests, [])
                self.assertIn(str(executable), before)
                executable.write_bytes(b"changed executable")
                self.assertNotEqual(before, gate.artifact_hashes(build, tests, []))

    def test_relative_command_files_follow_the_test_working_directory(self):
        for mode in ("default", "absolute", "relative"):
            for definition in (False, True):
                with self.subTest(mode=mode, definition=definition), tempfile.TemporaryDirectory() as directory:
                    build = Path(directory).resolve()
                    working = build if mode == "default" else build / "working directory"
                    working.mkdir(exist_ok=True)
                    script = working / "probe.py"
                    script.write_text("original", encoding="utf-8")
                    argument = "-DINPUT=probe.py" if definition else "probe.py"
                    properties = [] if mode == "default" else [{
                        "name": "WORKING_DIRECTORY",
                        "value": str(working if mode == "absolute" else working.relative_to(build)),
                    }]
                    command = build / "probe-command"
                    command.write_bytes(b"probe")
                    tests = [{"name": "probe", "command": [str(command), argument], "properties": properties}]
                    before = gate.artifact_hashes(build, tests, [])
                    self.assertIn(str(script.resolve()), before)
                    script.write_text("changed", encoding="utf-8")
                    self.assertNotEqual(before, gate.artifact_hashes(build, tests, []))


LOADER_LOG = """[Vulkan Loader] INFO | LAYER:      Insert instance layer "VK_LAYER_KHRONOS_validation" ({library})
[Vulkan Loader] LAYER:                VK_LAYER_KHRONOS_validation
[Vulkan Loader] LAYER:                        Manifest: {manifest}
[Vulkan Loader] LAYER:                        Library:  {library}
"""


class LayerVersionTests(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.addCleanup(self.directory.cleanup)
        self.root = Path(self.directory.name)

    def manifest(self, api_version, name="VK_LAYER_KHRONOS_validation", array=False):
        layer = {"name": name, "type": "GLOBAL", "library_path": "libVkLayer_khronos_validation.so",
                 "api_version": api_version, "implementation_version": "1"}
        path = self.root / ("array.json" if array else "VkLayer_khronos_validation.json")
        path.write_text(json.dumps({"file_format_version": "1.2.0", **({"layers": [layer]} if array else {"layer": layer})}), encoding="utf-8")
        return path

    def test_manifest_versions_below_the_minimum_are_refused(self):
        for api_version in ("1.3.275", "1.4.362", "0.9.999"):
            with self.subTest(api_version=api_version), self.assertRaisesRegex(gate.GateError, "older than the minimum 1.4.363"):
                gate.layer_record(self.manifest(api_version), "1.4.363")

    def test_manifest_versions_at_or_above_the_minimum_are_recorded(self):
        for api_version in ("1.4.363", "1.4.364", "1.5.0", "2.0.0"):
            with self.subTest(api_version=api_version):
                record = gate.layer_record(self.manifest(api_version), "1.4.363")
                self.assertEqual(record["api_version"], api_version)
                self.assertEqual(record["minimum_api_version"], "1.4.363")
                self.assertEqual(len(record["manifest_sha256"]), 64)
        self.assertEqual(gate.layer_record(self.manifest("1.4.363", array=True), "1.4.363")["api_version"], "1.4.363")

    def test_missing_or_unusable_manifests_are_refused(self):
        with self.assertRaisesRegex(gate.GateError, "unreadable validation layer manifest"):
            gate.layer_record(self.root / "missing.json", "1.4.363")
        broken = self.root / "broken.json"
        broken.write_text("{", encoding="utf-8")
        with self.assertRaisesRegex(gate.GateError, "unreadable validation layer manifest"):
            gate.layer_record(broken, "1.4.363")
        with self.assertRaisesRegex(gate.GateError, "does not describe"):
            gate.layer_record(self.manifest("1.4.363", name="VK_LAYER_OTHER"), "1.4.363")
        for api_version in ("1.4", "1.4.363.1", None, "v1.4.363"):
            with self.subTest(api_version=api_version), self.assertRaisesRegex(gate.GateError, "MAJOR.MINOR.PATCH"):
                gate.layer_record(self.manifest(api_version), "1.4.363")

    def test_the_manifest_comes_from_the_loader_log(self):
        manifest = self.manifest("1.4.363")
        log = LOADER_LOG.format(library="/layers/libVkLayer_khronos_validation.so", manifest=manifest)
        self.assertEqual(gate.layer_manifest(log), manifest)
        with self.assertRaisesRegex(gate.GateError, "names no manifest"):
            gate.layer_manifest(log.replace("Manifest:", "Manifesto"))
        with self.assertRaisesRegex(gate.GateError, "names no manifest"):
            gate.layer_manifest("")

    def check(self, log, code=0, minimum="1.4.363"):
        def run(command, path, environment, timeout):
            self.assertEqual(command[-1], "--canary")
            path.write_text(log, encoding="utf-8")
            return code

        with unittest.mock.patch.object(gate, "run", run):
            return gate.check_layer(self.root, {}, 10, minimum)

    def test_the_canary_must_be_reported_by_the_inserted_layer(self):
        manifest = self.manifest("1.4.363")
        loader = LOADER_LOG.format(library="/layers/libVkLayer_khronos_validation.so", manifest=manifest)
        reported = "Validation Error: [ " + gate.CANARY_VUID + " ] | MessageID = 0x0 | vkCreateBuffer(): size is zero.\n"
        record = self.check(loader + reported)
        self.assertEqual(record["library"], "/layers/libVkLayer_khronos_validation.so")
        self.assertEqual(record["canary"], gate.CANARY_VUID)
        self.assertEqual(self.check(loader + gate.CANARY_VUID + "(ERROR / SPEC): msgNum: 1 - size is zero\n")["api_version"], "1.4.363")
        for log, code, reason in ((loader + "canary: zero-size vkCreateBuffer returned 0\n", 0, "did not report"),
                                  (loader + "environment: " + gate.CANARY_VUID + "\n", 0, "did not report"),
                                  ("Layer VK_LAYER_KHRONOS_validation " + gate.MISSING_LAYER + "\n" + reported, 0, "ran without"),
                                  (reported, 0, "ran without"),
                                  (loader + reported, 5, "exited with 5")):
            with self.subTest(reason=reason), self.assertRaisesRegex(gate.GateError, reason):
                self.check(log, code)
        with self.assertRaisesRegex(gate.GateError, "older than the minimum 1.4.364"):
            self.check(loader + reported, minimum="1.4.364")

    def test_tests_must_insert_the_layer_the_canary_checked(self):
        def report(library):
            with tempfile.TemporaryDirectory() as directory:
                path = Path(directory) / "junit.xml"
                path.write_text('<testsuite><testcase name="probe"><system-err>' + gate.INSERTED_LAYER + " (" + library
                                + ")</system-err></testcase></testsuite>", encoding="utf-8")
                return gate.parse_report(path, {"probe"}, "/checked/libVkLayer_khronos_validation.so")

        tests, problems = report("/checked/libVkLayer_khronos_validation.so")
        self.assertEqual(problems, [])
        self.assertEqual(tests["probe"]["layer_libraries"], ["/checked/libVkLayer_khronos_validation.so"])
        self.assertEqual(report("/other/libVkLayer_khronos_validation.so")[1], ["probe"])

    def test_a_malformed_minimum_is_refused_before_output_creation(self):
        output = self.root / "evidence"
        with contextlib.redirect_stderr(io.StringIO()), self.assertRaises(SystemExit) as raised:
            gate.main(["--baseline", ".", "0" * 40, "--candidate", ".", "0" * 40, "--output", str(output), "--target", "t",
                       "--tests", ".*", "--gpu", "g", "--minimum-layer-version", "1.4"])
        self.assertEqual(raised.exception.code, 2)
        self.assertFalse(output.exists())


class CanaryTests(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.addCleanup(self.directory.cleanup)
        self.root = Path(self.directory.name)
        self.environment = {**os.environ, **gate.FORCED}
        probe = {key: value for key, value in os.environ.items() if key != "VK_INSTANCE_LAYERS"}
        command = [sys.executable, str(Path(gate.__file__).resolve()), "--canary"]
        if subprocess.run(command, env=probe, capture_output=True, timeout=120).returncode:
            self.skipTest("no Vulkan loader or device")
        try:
            gate.check_layer(self.root, self.environment, 120, "0.0.0")
        except gate.GateError as error:
            self.skipTest("no working validation layer: " + str(error))

    def test_the_installed_layer_reports_the_canary(self):
        record = gate.check_layer(self.root, self.environment, 120, "0.0.0")
        self.assertEqual(record["canary"], gate.CANARY_VUID)
        self.assertTrue(Path(record["manifest"]).is_file())

    def test_an_absent_layer_fails_the_canary(self):
        empty = self.root / "no-layers"
        empty.mkdir()
        environment = {**self.environment, "VK_LAYER_PATH": str(empty), "VK_ADD_LAYER_PATH": ""}
        with self.assertRaisesRegex(gate.GateError, "ran without"):
            gate.check_layer(self.root, environment, 120, "0.0.0")

    def test_a_silenced_layer_fails_the_canary(self):
        environment = {**self.environment, "VK_LAYER_MESSAGE_ID_FILTER": gate.CANARY_VUID}
        with self.assertRaisesRegex(gate.GateError, "did not report"):
            gate.check_layer(self.root, environment, 120, "0.0.0")


@unittest.skipUnless(shutil.which("cmake") and shutil.which("ninja") and shutil.which("ctest"), "requires CMake, Ninja and CTest")
class FixtureTests(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.addCleanup(self.directory.cleanup)
        self.root = Path(self.directory.name).resolve()

    def source(self, name, message="pass", test="probe", property_text="", layer=True):
        source = self.root / name
        source.mkdir()
        program = 'import os, sys\nfrom pathlib import Path\n'
        program += 'assert os.environ["ANYPS5_REQUIRE_VULKAN"] == "1"\n'
        program += 'assert os.environ["ANYPS5_NO_SHADER_CACHE"] == "1"\n'
        program += 'assert os.environ["VK_INSTANCE_LAYERS"] == "VK_LAYER_KHRONOS_validation"\n'
        program += 'assert os.environ["VK_LOADER_DEBUG"] == "layer"\n'
        program += 'assert os.environ["ANYPS5_GPU"] == "fixture"\n'
        if layer:
            program += 'print(\'[Vulkan Loader] INFO | LAYER:   Insert instance layer "VK_LAYER_KHRONOS_validation" (fixture)\', file=sys.stderr)\n'
        if message == "skip":
            program += 'print("skipped, synthetic unavailable device")\nsys.exit(77)\n'
        elif message == "fail":
            program += 'sys.exit(7)\n'
        elif message == "mutate":
            program += 'with Path(__file__).open("a") as output: output.write("\\n")\n'
        elif message == "large":
            program += 'print("Validation Error: [ VUID-Early-00001 ] synthetic")\n'
            program += 'print("x" * 131072)\n'
            program += 'print("Validation Error: [ VUID-Late-00002 ] synthetic")\n'
        else:
            program += 'print(' + repr(message) + ')\n'
        (source / "probe.py").write_text(program, encoding="utf-8")
        (source / "CMakeLists.txt").write_text(
            'cmake_minimum_required(VERSION 3.20)\nproject(gate_fixture NONE)\nenable_testing()\n'
            'add_custom_command(OUTPUT "${CMAKE_BINARY_DIR}/probe.py" COMMAND "${CMAKE_COMMAND}" -E copy "${CMAKE_SOURCE_DIR}/probe.py" "${CMAKE_BINARY_DIR}/probe.py" DEPENDS "${CMAKE_SOURCE_DIR}/probe.py")\n'
            'add_custom_target(fixture DEPENDS "${CMAKE_BINARY_DIR}/probe.py")\n'
            'add_test(NAME ' + test + ' COMMAND "' + PYTHON + '" "${CMAKE_BINARY_DIR}/probe.py")\n'
            'set_tests_properties(' + test + ' PROPERTIES SKIP_RETURN_CODE 77 ' + property_text + ')\n', encoding="utf-8")
        subprocess.run(["git", "init", "-q", str(source)], check=True)
        subprocess.run(["git", "-C", str(source), "add", "."], check=True)
        subprocess.run(["git", "-C", str(source), "-c", "user.name=Fixture", "-c", "user.email=fixture@example.invalid",
                        "-c", "commit.gpgSign=false", "commit", "-qm", "fixture"], check=True)
        return source, gate.git(source, "rev-parse", "HEAD")

    def execute(self, baseline, candidate, *extra):
        output = self.root / "evidence"
        arguments = ["--baseline", str(baseline[0]), baseline[1], "--candidate", str(candidate[0]), candidate[1],
                     "--output", str(output), "--target", "fixture", "--tests", ".*", "--gpu", "fixture", *extra]
        layer = {"manifest": "fixture.json", "api_version": gate.MINIMUM_LAYER_VERSION, "library": "fixture", "canary": gate.CANARY_VUID}
        with contextlib.redirect_stdout(io.StringIO()), contextlib.redirect_stderr(io.StringIO()), \
                unittest.mock.patch.object(gate, "check_layer", getattr(self, "check_layer", lambda *_: layer)):
            code = gate.main(arguments)
        report = json.loads((output / "report.json").read_text()) if (output / "report.json").is_file() else None
        return code, report


class TimeoutTests(FixtureTests):
    def test_timeout_reset_property_is_refused_before_running_tests(self):
        baseline = self.source("base-reset")
        candidate = self.source("head-reset", property_text='TIMEOUT_AFTER_MATCH "1;ready"')
        code, report = self.execute(baseline, candidate, "--repeat", "1", "--test-timeout", "1")
        self.assertEqual(code, 2, report)
        self.assertIn("TIMEOUT_AFTER_MATCH", report["error"])
        self.assertEqual(report["iterations"], [])

    def test_a_shorter_ctest_property_remains_usable(self):
        baseline = self.source("base-short", property_text="TIMEOUT 0.5")
        candidate = self.source("head-short", property_text="TIMEOUT 0.5")
        report_text = ('<testsuite><testcase name="probe"><system-err>' + gate.INSERTED_LAYER
                       + '</system-err></testcase></testsuite>')
        ctest_runs = []
        real_run = gate.run

        def run(command, log, environment, timeout):
            if "--output-junit" not in command:
                return real_run(command, log, environment, timeout)
            ctest_runs.append(command)
            Path(command[command.index("--output-junit") + 1]).write_text(report_text, encoding="utf-8")
            return 0

        with unittest.mock.patch.object(gate, "run", run):
            code, report = self.execute(baseline, candidate, "--repeat", "1", "--test-timeout", "1")
        self.assertEqual(code, 0, report)
        self.assertEqual(report["status"], "complete")
        self.assertEqual(len(ctest_runs), 2)
        for command in ctest_runs:
            self.assertEqual(command[command.index("--timeout") + 1], "1.0")

    def test_request_above_thirty_seconds_is_refused_before_output_creation(self):
        with self.assertRaises(SystemExit) as raised:
            self.execute(self.source("base-limit"), self.source("head-limit"), "--test-timeout", "31")
        self.assertEqual(raised.exception.code, 2)
        self.assertFalse((self.root / "evidence").exists())

    def test_ctest_timeout_property_is_checked_before_running_tests(self):
        for value in ("0", "2"):
            with self.subTest(value=value):
                baseline = self.source("base-" + value)
                candidate = self.source("head-" + value, property_text="TIMEOUT " + value)
                code, report = self.execute(baseline, candidate, "--repeat", "1", "--test-timeout", ".05")
                self.assertEqual(code, 2, report)
                self.assertIn("probe: TIMEOUT", report["error"])
                self.assertEqual(report["iterations"], [])
                self.assertEqual(report["status"], "incomplete")
                (self.root / "evidence").rename(self.root / ("evidence-" + value))


class IntegrationTests(FixtureTests):
    def test_pass_builds_both_targets_and_alternates_with_attribution(self):
        code, report = self.execute(self.source("base"), self.source("head"))
        self.assertEqual(code, 0, report)
        self.assertEqual([entry["side"] for entry in report["iterations"]], ["baseline", "candidate", "candidate", "baseline"])
        self.assertEqual(report["status"], "complete")
        for side in ("baseline", "candidate"):
            files = report["builds"][side]["artifact_sha256"]
            self.assertTrue(any(path.endswith("probe.py") for path in files))
            self.assertTrue((self.root / "evidence" / side / "iteration-1.xml").is_file())


class ValidationTests(FixtureTests):
    def test_ctest_success_with_validation_errors_is_a_gate_failure(self):
        code, report = self.execute(self.source("base"), self.source("head", "Validation Error: [ VUID-Fixture-00001 ] synthetic"), "--repeat", "1")
        self.assertEqual(code, 1, report)
        self.assertTrue(all(entry["returncode"] == 0 for entry in report["iterations"]))
        self.assertEqual(report["new_diagnostics"], {"probe": ["VUID-Fixture-00001"]})


class LayerTests(FixtureTests):
    def test_ctest_success_with_missing_layer_is_a_gate_failure(self):
        message = "WARNING: [Loader Message] layer was not found but was requested by env var VK_INSTANCE_LAYERS"
        code, report = self.execute(self.source("base", message), self.source("head"), "--repeat", "1")
        self.assertEqual(code, 1, report)
        self.assertTrue(all(entry["returncode"] == 0 for entry in report["iterations"]))
        self.assertTrue(report["problems"])

    def test_missing_layer_without_severity_is_a_gate_failure(self):
        message = "Layer VK_LAYER_KHRONOS_validation was not found but was requested by env var VK_INSTANCE_LAYERS"
        code, report = self.execute(self.source("base"), self.source("head", message), "--repeat", "1")
        self.assertEqual(code, 1, report)

    def test_a_failed_layer_check_stops_before_building(self):
        def check_layer(*_):
            raise gate.GateError("the validation layer did not report the canary")

        self.check_layer = check_layer
        code, report = self.execute(self.source("base"), self.source("head"), "--repeat", "1")
        self.assertEqual(code, 2, report)
        self.assertIn("canary", report["error"])
        self.assertEqual(report["builds"], {})
        self.assertEqual(report["iterations"], [])

    def test_the_checked_layer_is_recorded(self):
        code, report = self.execute(self.source("base"), self.source("head"), "--repeat", "1")
        self.assertEqual(code, 0, report)
        self.assertEqual(report["layer"]["api_version"], gate.MINIMUM_LAYER_VERSION)
        self.assertEqual(report["layer"]["canary"], gate.CANARY_VUID)

    def test_ctest_success_without_layer_insertion_is_a_gate_failure(self):
        code, report = self.execute(self.source("base"), self.source("head", layer=False), "--repeat", "1")
        self.assertEqual(code, 1, report)
        self.assertTrue(all(entry["returncode"] == 0 for entry in report["iterations"]))
        self.assertEqual({entry["side"]: entry["tests"]["probe"]["layer_inserted"] for entry in report["iterations"]},
                         {"baseline": True, "candidate": False})


class OutputTests(FixtureTests):
    def test_large_passed_output_retains_early_and_late_validation_messages(self):
        code, report = self.execute(self.source("base"), self.source("head", "large"), "--repeat", "1")
        self.assertEqual(code, 1, report)
        self.assertEqual(report["new_diagnostics"], {"probe": ["VUID-Early-00001", "VUID-Late-00002"]})

    def test_configuration_like_text_does_not_create_a_validation_error(self):
        text = "VUID-Fixture-00001 was not found but was requested by env var VK_INSTANCE_LAYERS"
        code, report = self.execute(self.source("base"), self.source("head", property_text='ENVIRONMENT "GATE_CONFIGURATION_ECHO=' + text + '"'), "--repeat", "1")
        self.assertEqual(code, 0, report)
        self.assertIn(text, (self.root / "evidence/candidate/iteration-1.log").read_text())

    def test_a_skipped_selected_test_is_not_accepted(self):
        code, report = self.execute(self.source("base"), self.source("head", "skip"), "--repeat", "1")
        self.assertEqual(code, 1, report)
        self.assertTrue(report["problems"])


class AttributionTests(FixtureTests):
    def test_relative_script_mutation_invalidates_successful_ctest_results(self):
        baseline, candidate = self.source("base"), self.source("head", "mutate")
        cmake = candidate[0] / "CMakeLists.txt"
        text = cmake.read_text(encoding="utf-8")
        absolute = 'add_test(NAME probe COMMAND "' + PYTHON + '" "${CMAKE_BINARY_DIR}/probe.py")'
        self.assertIn(absolute, text)
        cmake.write_text(text.replace(absolute, 'add_test(NAME probe COMMAND "' + PYTHON + '" probe.py)'), encoding="utf-8")
        subprocess.run(["git", "-C", str(candidate[0]), "-c", "user.name=Fixture", "-c", "user.email=fixture@example.invalid",
                        "-c", "commit.gpgSign=false", "commit", "-qam", "relative fixture"], check=True)
        candidate = candidate[0], gate.git(candidate[0], "rev-parse", "HEAD")
        code, report = self.execute(baseline, candidate, "--repeat", "1")
        self.assertTrue(all(iteration["returncode"] == 0 for iteration in report["iterations"]), report)
        self.assertEqual(report["builds"]["candidate"]["tests"][0]["command"][1], "probe.py")
        self.assertNotEqual((self.root / "evidence/candidate/build/probe.py").read_bytes(), (candidate[0] / "probe.py").read_bytes())
        self.assertEqual(code, 2, report)
        self.assertIn("artifacts changed", report["error"])

    def test_different_selected_names_stop_before_iterations(self):
        code, report = self.execute(self.source("base"), self.source("head", test="other"))
        self.assertEqual(code, 2, report)
        self.assertEqual(report["iterations"], [])
        self.assertIn("test-name sets differ", report["error"])

    def test_changed_artifact_invalidates_completed_test_results(self):
        code, report = self.execute(self.source("base"), self.source("head", "mutate"), "--repeat", "1")
        self.assertEqual(code, 2, report)
        self.assertIn("artifacts changed", report["error"])


class SetupTests(FixtureTests):
    def test_test_properties_cannot_disable_required_validation(self):
        code, report = self.execute(self.source("base"), self.source("head", property_text='ENVIRONMENT "ANYPS5_REQUIRE_VULKAN=0"'))
        self.assertEqual(code, 2, report)
        self.assertIn("overrides required environment", report["error"])

    def test_test_properties_cannot_override_driver_selection(self):
        code, report = self.execute(self.source("base"), self.source("head", property_text='ENVIRONMENT "VK_DRIVER_FILES=/different/driver.json"'))
        self.assertEqual(code, 2, report)
        self.assertIn("overrides required environment", report["error"])

    def test_dirty_source_is_rejected_before_output_creation(self):
        baseline, candidate = self.source("base"), self.source("head")
        (candidate[0] / "untracked.txt").write_text("dirty")
        code, report = self.execute(baseline, candidate)
        self.assertEqual(code, 2)
        self.assertIsNone(report)

    def test_wrong_expected_commit_is_rejected_before_output_creation(self):
        baseline, candidate = self.source("base"), self.source("head")
        code, report = self.execute(baseline, (candidate[0], "0" * 40))
        self.assertEqual(code, 2)
        self.assertIsNone(report)

    def test_existing_output_is_preserved(self):
        output = self.root / "evidence"
        output.mkdir()
        marker = output / "keep"
        marker.write_text("existing data")
        code, report = self.execute(self.source("base"), self.source("head"))
        self.assertEqual(code, 2)
        self.assertIsNone(report)
        self.assertEqual(marker.read_text(), "existing data")

    def test_empty_selection_is_a_setup_error(self):
        code, report = self.execute(self.source("base"), self.source("head"), "--tests", "^missing$")
        self.assertEqual(code, 2, report)
        self.assertIn("nonempty unique test names", report["error"])


class CommandTests(FixtureTests):
    def test_unexpected_command_failure_is_retained(self):
        code, report = self.execute(self.source("base"), self.source("head", "fail"), "--repeat", "1")
        self.assertEqual(code, 1, report)
        self.assertTrue(any(entry["returncode"] != 0 for entry in report["iterations"]))

    def test_command_cleanup_removes_descendant_after_parent_exit_and_timeout(self):
        for timeout in (True, False):
            with self.subTest(timeout=timeout):
                marker = self.root / ("timeout-marker" if timeout else "exit-marker")
                ready = marker.with_suffix(".ready")
                child = f"import time; from pathlib import Path; Path({str(ready)!r}).touch(); time.sleep(1); Path({str(marker)!r}).touch()"
                parent = f"import subprocess,sys,time; from pathlib import Path; subprocess.Popen([sys.executable,'-c',{child!r}])\nwhile not Path({str(ready)!r}).exists(): time.sleep(.01)\n"
                if timeout:
                    parent += "time.sleep(30)\n"
                    with self.assertRaises(subprocess.TimeoutExpired):
                        gate.run([sys.executable, "-c", parent], self.root / "timeout.log", os.environ, .5)
                else:
                    self.assertEqual(gate.run([sys.executable, "-c", parent], self.root / "exit.log", os.environ, 5), 0)
                self.assertTrue(ready.exists())
                time.sleep(1.1)
                self.assertFalse(marker.exists())

    def test_symlink_loops_are_setup_errors(self):
        try:
            (self.root / "first").symlink_to("second")
            (self.root / "second").symlink_to("first")
        except OSError as error:
            self.skipTest(f"symlinks unavailable: {error}")
        loop, source = self.root / "first", self.source("base")
        for baseline, output in ((source, loop / "evidence"), ((loop, source[1]), self.root / "evidence")):
            with self.subTest(baseline=baseline[0], output=output):
                arguments = ["--baseline", str(baseline[0]), baseline[1], "--candidate", str(source[0]), source[1],
                             "--output", str(output), "--target", "fixture", "--tests", ".*", "--gpu", "fixture"]
                stderr = io.StringIO()
                with contextlib.redirect_stdout(io.StringIO()), contextlib.redirect_stderr(stderr):
                    self.assertEqual(gate.main(arguments), 2)
                self.assertIn("validation gate:", stderr.getvalue())
                self.assertFalse((self.root / "evidence").exists())


if __name__ == "__main__":
    unittest.main()
