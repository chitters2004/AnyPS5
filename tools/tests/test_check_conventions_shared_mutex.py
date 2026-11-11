import os
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path, PurePosixPath

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from check_conventions import CPP, NATIVE_SHARED_MUTEX, SHARED_MUTEX, cpp_text


class SharedMutexPatternTests(unittest.TestCase):
    def test_reported_declarations(self):
        for line, match in (
            ("std::shared_mutex mutex;", "std::shared_mutex"),
            ("static std::shared_timed_mutex mutex;", "std::shared_timed_mutex"),
            ("std::shared_lock<std::shared_mutex> lock(mutex);", "std::shared_mutex"),
            ("using Lock = std::shared_mutex;", "std::shared_mutex"),
            ("pthread_rwlock_t lock = PTHREAD_RWLOCK_INITIALIZER;", "pthread_rwlock_t"),
            ("pthread_rwlock_rdlock(&lock);", "pthread_rwlock_rdlock("),
            ("pthread_rwlock_init (&lock, nullptr);", "pthread_rwlock_init ("),
            ("std :: shared_mutex mutex;", "std :: shared_mutex"),
            ("std::  shared_timed_mutex mutex;", "std::  shared_timed_mutex"),
        ):
            with self.subTest(line=line):
                self.assertEqual(SHARED_MUTEX.search(line).group(0), match)

    def test_accepted_declarations(self):
        for line in (
            "NativeSharedMutex mutex;",
            "std::shared_lock lock(mutex);",
            "#include <shared_mutex>",
            "int APS5_VABI pthread_rwlock_rdlock_nid_postfix(PthreadRwlock* rwlock);",
            "return pthread_rwlock_timedwrlock_nid_postfix(rwlock, abstime);",
            "PthreadRwlock rwlock = nullptr;",
            "std::shared_mutexes;",
        ):
            with self.subTest(line=line):
                self.assertIsNone(SHARED_MUTEX.search(line))


class RepositoryTests(unittest.TestCase):
    def test_core_has_no_host_reader_writer_lock(self):
        root = Path(__file__).resolve().parents[2]
        listed = subprocess.run(["git", "ls-files", "-z", "core"], cwd=root, capture_output=True, check=True).stdout.decode().split("\0")
        found = []
        for path in listed:
            parts = PurePosixPath(path)
            if not path or parts.suffix not in CPP or "tests" in parts.parts or path == NATIVE_SHARED_MUTEX:
                continue
            text = cpp_text((root / path).read_text(encoding="utf-8", errors="replace"))
            for number, line in enumerate(text.split("\n"), 1):
                match = SHARED_MUTEX.search(line)
                if match:
                    found.append(f"{path}:{number}: {match.group(0)}")
        self.assertEqual(found, [])


class SharedMutexCheckTests(unittest.TestCase):
    def check(self, path, text):
        checker = Path(__file__).resolve().parents[1] / "check_conventions.py"
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)

            def git(*args):
                return subprocess.run(
                    ["git", "-c", "user.name=Conventions Test", "-c", "user.email=test@example.com",
                     "-c", "commit.gpgsign=false", *args], cwd=root,
                    capture_output=True, text=True, check=True)

            git("init", "-q")
            (root / "README.md").write_text("Example\n", encoding="utf-8")
            git("add", ".")
            git("commit", "-qm", "docs: add readme")
            source = root / path
            source.parent.mkdir(parents=True)
            source.write_text(text, encoding="utf-8")
            git("add", ".")
            git("commit", "-qm", "fix: add a lock")
            environment = {key: value for key, value in os.environ.items()
                           if key not in ("GITHUB_EVENT_PATH", "GITHUB_STEP_SUMMARY", "GITHUB_ACTIONS")}
            return subprocess.run([sys.executable, str(checker), "--base", "HEAD~1"],
                                  cwd=root, capture_output=True, text=True, env=environment)

    def test_std_shared_mutex_in_core_is_reported(self):
        result = self.check("core/libs/prx/libSceExample/src/Example.cpp", "#include <shared_mutex>\n\nstd::shared_mutex mutex;\n")
        self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
        self.assertIn("[shared-mutex]", result.stdout)

    def test_host_pthread_rwlock_in_core_is_reported(self):
        result = self.check("core/shader/Example.cpp", "#include <pthread.h>\n\npthread_rwlock_t lock;\n")
        self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
        self.assertIn("[shared-mutex]", result.stdout)

    def test_native_shared_mutex_is_accepted(self):
        result = self.check("core/libs/prx/libSceExample/src/Example.cpp", "#include \"prx/libc/include/NativeSharedMutex.hpp\"\n\nNativeSharedMutex mutex;\n")
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_tests_and_the_native_header_are_exempt(self):
        for path in ("core/libs/prx/libSceExample/tests/Example.cpp", "core/libs/prx/libc/include/NativeSharedMutex.hpp"):
            with self.subTest(path=path):
                result = self.check(path, "#include <shared_mutex>\n\nusing Lock = std::shared_mutex;\n")
                self.assertNotIn("[shared-mutex]", result.stdout)

    def test_code_outside_core_is_exempt(self):
        result = self.check("tools/example/Example.cpp", "#include <shared_mutex>\n\nstd::shared_mutex mutex;\n")
        self.assertNotIn("[shared-mutex]", result.stdout)


if __name__ == "__main__":
    unittest.main()
