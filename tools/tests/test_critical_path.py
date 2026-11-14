import io
import json
import sys
import tempfile
import unittest
from contextlib import redirect_stdout
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import critical_path

MS = 1_000_000


def row(present, threads, depth=8):
    return {"present": present, "begin_ns": present * 30 * MS, "end_ns": (present + 1) * 30 * MS, "ring_depth": depth, "threads": threads}


def committer(busy, idle, cpu=None, **extra):
    thread = {"name": "draw committer", "role": "committer", "commit_busy_ns": busy * MS, "commit_idle_ns": idle * MS, "commits": 1000}
    if cpu is not None:
        thread["cpu_ns"] = cpu * MS
    thread.update(extra)
    return thread


def worker(enqueues, occupancy, room=0, **extra):
    thread = {"name": "queue worker 0x0", "role": "worker", "enqueues": enqueues, "occupancy_sum": occupancy * enqueues, "room_wait_ns": room * MS}
    thread.update(extra)
    return thread


ROWS = [
    row(1, [committer(29, 0), worker(100, 7, room=12)]),
    row(2, [committer(12, 16), worker(100, 1)]),
    row(3, [committer(15, 2), worker(100, 4, gpu_wait_ns=6 * MS, gpu_wait_behind_ns={"committer": 6 * MS})]),
    row(4, [committer(15, 2), worker(100, 4, collect_ns=9 * MS)]),
    row(5, [committer(15, 2, cpu=10), worker(100, 4)]),
]


class CriticalPathTests(unittest.TestCase):
    def write(self, rows, header=None):
        directory = tempfile.TemporaryDirectory()
        self.addCleanup(directory.cleanup)
        path = Path(directory.name) / "critical.jsonl"
        lines = [json.dumps(header or {"format": "aps5-critical/1", "cpu_ticks_per_ns": 1.0, "rows": len(rows)})]
        lines += [json.dumps(entry) for entry in rows]
        path.write_text("\n".join(lines) + "\n", encoding="utf-8")
        return path

    def run_json(self, path, *extra):
        out = io.StringIO()
        with redirect_stdout(out):
            self.assertEqual(critical_path.main([str(path), "--json", *extra]), 0)
        return json.loads(out.getvalue())

    def test_verdicts(self):
        result = self.run_json(self.write(ROWS))
        verdicts = [entry["verdict"] for entry in result["frames"]]
        self.assertEqual(verdicts, ["committer", "producer", "shared-lock", "write-watch", "mixed"])
        self.assertEqual(result["summary"]["verdicts"]["committer"], 1)
        self.assertEqual(result["summary"]["frames"], 5)

    def test_shares(self):
        result = self.run_json(self.write(ROWS[:1]))
        entry = result["frames"][0]
        self.assertAlmostEqual(entry["shares"]["committer_busy"], 29 / 30)
        self.assertAlmostEqual(entry["shares"]["producer_room_wait"], 12 / 30)
        self.assertAlmostEqual(entry["ring_occupancy"], 7.0)
        self.assertEqual(entry["producer"], "queue worker 0x0")
        self.assertAlmostEqual(entry["frame_ms"], 30.0)

    def test_lock_owner_and_unrecorded_time(self):
        result = self.run_json(self.write(ROWS[2:5]))
        self.assertEqual(result["frames"][0]["gpu_wait_behind_worker_side"], {"committer": 6 * MS})
        self.assertAlmostEqual(result["frames"][2]["not_running_unrecorded"]["draw committer"], (30 - 10 - 2) / 30)

    def test_thresholds(self):
        result = self.run_json(self.write(ROWS[:1]), "--busy", "0.99")
        self.assertEqual(result["frames"][0]["verdict"], "mixed")

    def test_table(self):
        out = io.StringIO()
        with redirect_stdout(out):
            self.assertEqual(critical_path.main([str(self.write(ROWS)), "--frames", "2"]), 0)
        text = out.getvalue()
        self.assertIn("5 frames, frame p50 30.0 ms; verdicts: committer 1, producer 1, shared-lock 1, write-watch 1, mixed 1", text)
        self.assertIn("... 3 more frames", text)

    def test_rejects_other_files(self):
        path = self.write([], header={"format": "other"})
        with redirect_stdout(io.StringIO()):
            self.assertEqual(critical_path.main([str(path)]), 1)


if __name__ == "__main__":
    unittest.main()
