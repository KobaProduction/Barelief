import contextlib
import io
import json
import os
from pathlib import Path
import sys
import subprocess
import tempfile
import time
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "scripts"))
from quality_execution import CommandTask, resolve_jobs, run_jobs
import quality_execution


class ResourceBudgetTests(unittest.TestCase):
    def setUp(self):
        self.environment = patch.dict(os.environ, {"QUALITY_JOBS": "auto"})
        self.environment.start()
        self.addCleanup(self.environment.stop)

    def test_auto_scales_above_four_for_large_machine(self):
        self.assertEqual(resolve_jobs(cpu_count=32, memory_bytes=64 * 1024**3), 32)

    def test_ram_limits_concurrency(self):
        self.assertEqual(resolve_jobs(cpu_count=32, memory_bytes=4 * 1024**3), 4)
        self.assertEqual(resolve_jobs(cpu_count=32, memory_bytes=1), 1)

    def test_cpu_limits_concurrency(self):
        self.assertEqual(resolve_jobs(cpu_count=3, memory_bytes=64 * 1024**3), 3)

    def test_cli_override_precedes_environment(self):
        with patch.dict(os.environ, {"QUALITY_JOBS": "19"}):
            self.assertEqual(resolve_jobs(7, cpu_count=2, memory_bytes=1), 7)
            self.assertEqual(resolve_jobs(cpu_count=2, memory_bytes=1), 19)

    def test_invalid_overrides_fail(self):
        for value in ["0", "-2", "two"]:
            with self.subTest(value=value), patch.dict(os.environ, {"QUALITY_JOBS": value}):
                with self.assertRaises(ValueError):
                    resolve_jobs()
        with self.assertRaises(ValueError):
            resolve_jobs(0)
        with self.assertRaises(ValueError):
            resolve_jobs(memory_per_worker_bytes=0)


class ProcessSchedulerTests(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.addCleanup(self.directory.cleanup)
        self.root = Path(self.directory.name)
        self.progress_file = self.root / "progress.jsonl"
        self.environment = patch.dict(os.environ, {
            "QUALITY_PROGRESS_FILE": str(self.progress_file), "QUALITY_JOBS": "auto",
        })
        self.environment.start()
        self.addCleanup(self.environment.stop)

    def child(self, label, code):
        return CommandTask(label, [sys.executable, "-c", code], cwd=str(self.root))

    def run_quiet(self, tasks, jobs=2):
        output = io.StringIO()
        progress = []
        with contextlib.redirect_stdout(output):
            result = run_jobs(tasks, jobs=jobs, progress=progress.append)
        return result, output.getvalue(), progress

    def test_parallel_workers_are_bounded_and_overlap(self):
        tasks = [self.child(str(index), (
            "import json,time; from pathlib import Path; "
            "start=time.monotonic(); time.sleep(0.25); "
            f"Path('{index}.json').write_text(json.dumps([start,time.monotonic()]))"
        )) for index in range(6)]
        self.assertEqual(self.run_quiet(tasks, jobs=2)[0], 0)
        events = []
        for path in self.root.glob("[0-9].json"):
            start, end = json.loads(path.read_text())
            events.extend([(start, 1), (end, -1)])
        running = peak = 0
        for _, change in sorted(events):
            running += change
            peak = max(peak, running)
        self.assertEqual(len(events), 12)
        self.assertEqual(peak, 2)

    def test_failure_does_not_skip_other_tasks_and_diagnostics_are_kept(self):
        tasks = [self.child("bad", "import sys; print('compiler failure'); sys.exit(7)")]
        tasks += [self.child(str(index), f"from pathlib import Path; Path('{index}').touch()")
                  for index in range(3)]
        result, output, progress = self.run_quiet(tasks)
        self.assertEqual(result, 1)
        self.assertIn("compiler failure", output)
        self.assertTrue(all((self.root / str(index)).exists() for index in range(3)))
        self.assertIn("failed=1", progress[-1])
        records = [json.loads(line) for line in self.progress_file.read_text().splitlines()]
        finished = [record for record in records if record["event"] in {"passed", "failed"}]
        self.assertEqual(len(finished), 4)
        self.assertEqual(finished[-1]["completed"], 4)
        status = json.loads(self.progress_file.with_suffix(".status.json").read_text())
        self.assertEqual(status["jobs"], 2)
        self.assertEqual(status["running"], 0)
        self.assertEqual(status["failed"], 1)
        self.assertEqual(status["completed"], status["total"])

    def test_terminal_verdict_is_failure_even_when_last_real_worker_succeeds(self):
        tasks = [self.child("bad-first", "import sys; sys.exit(7)"),
                 self.child("good-last", "print('last worker succeeds')")]
        result, _, _ = self.run_quiet(tasks, jobs=1)
        self.assertEqual(result, 1)
        status = json.loads(self.progress_file.with_suffix(".status.json").read_text())
        self.assertEqual(status["event"], "complete")
        self.assertEqual(status["exit_code"], 1)
        self.assertEqual(status["verdict"], "failed")
        self.assertEqual(status["failed"], 1)
        self.assertEqual(status["completed"], 2)
        self.assertEqual(status["running"], 0)
        records = [json.loads(line) for line in self.progress_file.read_text().splitlines()]
        self.assertEqual(records[-2]["event"], "passed")
        self.assertEqual(records[-2]["exit_code"], 0)
        self.assertEqual(records[-1]["exit_code"], 1)

    def test_redirected_ansi_stream_preserves_utf8_diagnostics(self):
        code = (
            "from quality_execution import CommandTask, run_jobs; import sys; "
            "sys.exit(run_jobs([CommandTask('unicode', [sys.executable, '-c', "
            "\"import sys; sys.stdout.buffer.write(chr(215).encode('utf-8'))\"])]))"
        )
        environment = os.environ.copy()
        environment.update(PYTHONIOENCODING="cp1251", PYTHONPATH=str(Path(quality_execution.__file__).parent))
        result = subprocess.run([sys.executable, "-c", code], env=environment, capture_output=True)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn(chr(215), result.stdout.decode("utf-8"))
        status = json.loads(self.progress_file.with_suffix(".status.json").read_text())
        self.assertEqual((status["completed"], status["running"], status["exit_code"]), (1, 0, 0))

    def test_publication_failure_is_accounted_and_does_not_skip_next_worker(self):
        class BrokenOnce(io.StringIO):
            def write(self, value):
                if "first diagnostic" in value:
                    raise OSError("test output unavailable")
                return super().write(value)

        tasks = [self.child("first", "print('first diagnostic')"),
                 self.child("second", "print('second diagnostic')")]
        with contextlib.redirect_stdout(BrokenOnce()), contextlib.redirect_stderr(io.StringIO()):
            self.assertEqual(run_jobs(tasks, jobs=1, progress=lambda message: None), 1)
        status = json.loads(self.progress_file.with_suffix(".status.json").read_text())
        self.assertEqual((status["completed"], status["running"], status["failed"]), (2, 0, 1))
        self.assertEqual(status["exit_code"], 1)

    def test_process_start_failure_is_nonzero_without_skipping_success(self):
        tasks = [CommandTask("missing", [str(self.root / "not-installed")]),
                 self.child("good", "print('still checked')")]
        result, output, _ = self.run_quiet(tasks)
        self.assertEqual(result, 1)
        self.assertIn("Cannot run missing", output)
        self.assertIn("still checked", output)

    def test_unicode_diagnostics_preserved_across_buffer_boundary(self):
        code = "import sys; sys.stdout.buffer.write(('a'*65535+'Ошибка').encode('utf-8'))"
        result, output, _ = self.run_quiet([self.child("unicode", code)])
        self.assertEqual(result, 0)
        self.assertTrue(output.endswith("Ошибка"))
        self.assertNotIn("\ufffd", output)

    def test_task_environment_and_working_directory(self):
        environment = os.environ.copy()
        environment["QUALITY_TEST_VALUE"] = "isolated"
        code = "import os; from pathlib import Path; print(os.environ['QUALITY_TEST_VALUE']); Path('cwd-marker').touch()"
        task = CommandTask("environment", [sys.executable, "-c", code],
                           cwd=str(self.root), env=environment)
        result, output, _ = self.run_quiet([task])
        self.assertEqual(result, 0)
        self.assertIn("isolated", output)
        self.assertTrue((self.root / "cwd-marker").is_file())

    def test_empty_schedule_is_success(self):
        self.assertEqual(self.run_quiet([])[0], 0)

    def test_interruption_stops_owned_worker_and_returns_nonzero(self):
        marker = self.root / "running"
        task = self.child("long-worker", (
            "import time; from pathlib import Path; "
            "Path('running').touch(); time.sleep(30)"
        ))

        def interrupt_after_start(futures):
            deadline = time.monotonic() + 10
            while not marker.exists() and time.monotonic() < deadline:
                time.sleep(0.01)
            self.assertTrue(marker.exists(), "Owned worker did not start")
            raise KeyboardInterrupt

        started = time.monotonic()
        with patch.object(quality_execution, "as_completed", interrupt_after_start):
            result = self.run_quiet([task])[0]
        self.assertEqual(result, 130)
        self.assertLess(time.monotonic() - started, 10)
        status = json.loads(self.progress_file.with_suffix(".status.json").read_text())
        self.assertEqual(status["event"], "interrupted")
        self.assertEqual(status["running"], 0)


if __name__ == "__main__":
    unittest.main()
