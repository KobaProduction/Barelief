"""Clazy gate regressions: scope, validation and actual bounded worker dispatch."""
from contextlib import redirect_stderr, redirect_stdout
import io
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from types import SimpleNamespace
from unittest import mock

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "scripts"))
import run_clazy as clazy


class ClazyGateTests(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.addCleanup(self.directory.cleanup)
        self.root = Path(self.directory.name).resolve()
        environment = mock.patch.dict(os.environ, {"QUALITY_FULL": "0",
                                                   "QUALITY_PROGRESS_FILE": str(self.root / "progress.jsonl")})
        environment.start()
        self.addCleanup(environment.stop)
        self.sources = [self.root / name for name in ("bad.cpp", "first.cpp", "second.cpp")]
        for source in self.sources:
            source.write_text("// owned test translation unit\n", encoding="utf-8")
        self.write_database(self.sources)

    def write_database(self, sources):
        entries = [{"directory": str(self.root), "file": str(path),
                    "command": "compiler -c " + str(path)} for path in sources]
        (self.root / "compile_commands.json").write_text(json.dumps(entries), encoding="utf-8")

    def args(self, *extra):
        return clazy.arguments(["-p", str(self.root), "--code-scope", "all", *extra,
                                *(str(path) for path in self.sources)])

    def test_owning_units_are_validated_and_deduplicated(self):
        names = [str(self.sources[0]), str(self.sources[1]), str(self.sources[0])]
        self.assertEqual(clazy.translation_units(self.root, names), names[:2])

    def test_header_and_qml_are_rejected_as_explicit_units(self):
        for name in ("input.h", "Scene.qml"):
            with self.subTest(name=name), self.assertRaisesRegex(ValueError, "owning C\\+\\+"):
                clazy.translation_units(self.root, [str(self.root / name)])

    def test_missing_database_and_missing_unit_fail_closed(self):
        self.write_database(self.sources[1:])
        with self.assertRaisesRegex(ValueError, "absent from compile_commands"):
            clazy.translation_units(self.root, [str(self.sources[0])])
        (self.root / "compile_commands.json").unlink()
        with self.assertRaises(OSError):
            clazy.translation_units(self.root, [str(self.sources[1])])

    def test_malformed_database_fails_before_dispatch(self):
        for contents in ("not json", "{}"):
            (self.root / "compile_commands.json").write_text(contents, encoding="utf-8")
            with self.subTest(contents=contents), mock.patch.object(clazy, "run_jobs") as workers:
                with self.assertRaises(ValueError):
                    clazy.run(self.args())
                workers.assert_not_called()

    def test_check_selection_is_explicit_and_diagnostics_are_not_ignored(self):
        completed = subprocess.CompletedProcess([], 0, "Enabled checks:\n    range-loop-detach\n    qstring-arg\n", "")
        with mock.patch.object(clazy.subprocess, "run", return_value=completed) as process:
            self.assertEqual(clazy.enabled_checks("clazy", "level1", {"SAFE": "1"}),
                             ["range-loop-detach", "qstring-arg"])
            self.assertIn("--checks=level1", process.call_args.args[0])
            self.assertEqual(process.call_args.kwargs["env"], {"SAFE": "1"})
        for output, error in (("Enabled checks:\n", ""), (completed.stdout, "bad selection")):
            with self.subTest(output=output, error=error), mock.patch.object(
                    clazy.subprocess, "run", return_value=subprocess.CompletedProcess([], 0, output, error)):
                with self.assertRaises(ValueError):
                    clazy.enabled_checks("clazy", "level1")

    def test_tasks_preserve_checks_target_and_clean_environment(self):
        unsafe = {name: "unsafe" for name in ("CLAZY_IGNORE_DIRS", "CLAZY_HEADER_FILTER", "CLAZY_CHECKS", "CLAZY_NO_WERROR")}
        unsafe["CLAZY_CHECKS_AS_ERRORS"] = "incorrect-subset"
        with mock.patch.dict(os.environ, unsafe), mock.patch.object(clazy, "executable_path", return_value="clazy"), \
                mock.patch.object(clazy, "enabled_checks", return_value=["range-loop-detach", "qstring-arg"]), \
                mock.patch.object(clazy, "run_jobs", return_value=1) as dispatch:
            self.assertEqual(clazy.run(self.args("--jobs", "7", "--extra-arg=--target=x86_64-w64-windows-gnu")), 1)
        tasks = dispatch.call_args.args[0]
        self.assertEqual(len(tasks), len(self.sources))
        self.assertEqual(dispatch.call_args.kwargs["jobs"], 7)
        for task, source in zip(tasks, self.sources):
            self.assertEqual(task.command[-1], str(source))
            self.assertIn("--only-qt", task.command)
            self.assertIn("--checks=level1", task.command)
            self.assertIn("--extra-arg=--target=x86_64-w64-windows-gnu", task.command)
            self.assertEqual(task.env["CLAZY_CHECKS_AS_ERRORS"], "range-loop-detach,qstring-arg")
            for name in unsafe:
                if name != "CLAZY_CHECKS_AS_ERRORS":
                    self.assertNotIn(name, task.env)

    def test_git_scope_is_resolved_before_qt_translation_unit_filter(self):
        selection = SimpleNamespace(reason="header consumers", files=(
            str(ROOT / "presentation/qt/include/barelief/ui_text.h"),
            str(ROOT / "presentation/qt/src/theme/ui_text.cpp"),
            str(ROOT / "domain/src/arbitrary.cpp")))
        args = clazy.arguments(["--from-ref", "before", "--to-ref", "after"])
        with mock.patch.object(clazy, "select_scope", return_value=selection) as select, redirect_stdout(io.StringIO()):
            self.assertEqual(clazy.selected_files(args), [str(ROOT / "presentation/qt/src/theme/ui_text.cpp")])
        self.assertEqual(select.call_args.kwargs, {"full": False, "from_ref": "before", "to_ref": "after"})

    def test_force_full_overrides_positional_focused_scope(self):
        selection = SimpleNamespace(reason="full", files=(str(ROOT / "apps/registry/main_client.cpp"),))
        for extra, environment in ((["--full"], {}), (["--scope", "full"], {}),
                                   ([], {"QUALITY_FULL": "1"})):
            with self.subTest(extra=extra), mock.patch.dict(os.environ, environment), \
                    mock.patch.object(clazy, "select_scope", return_value=selection) as select, redirect_stdout(io.StringIO()):
                result = clazy.selected_files(self.args(*extra))
                self.assertEqual(result, list(selection.files))
                self.assertTrue(select.call_args.kwargs["full"])

    def test_default_code_scope_is_production(self):
        self.assertEqual(clazy.arguments([]).code_scope, "production")

    def test_production_tests_and_all_are_partitioned_after_git_fallback(self):
        production = str(ROOT / "apps/registry/main_client.cpp")
        test = str(ROOT / "tests/ui_persistence_tests.cpp")
        portable = str(ROOT / "domain/src/not_a_qt_source.cpp")
        header = str(ROOT / "tests/numeric_gui_fixture.h")
        selection = SimpleNamespace(reason="unknown Git: full fallback", files=(production, test, portable, header))
        expected = {"production": [production], "tests": [test], "all": [production, test]}
        for code_scope, names in expected.items():
            args = clazy.arguments(["--scope", "full", "--code-scope", code_scope])
            with self.subTest(code_scope=code_scope), mock.patch.object(
                    clazy, "select_scope", return_value=selection), redirect_stdout(io.StringIO()):
                self.assertEqual(clazy.selected_files(args), names)

    def test_explicit_wrong_category_is_rejected_even_with_full(self):
        examples = (("production", ROOT / "tests/ui_persistence_tests.cpp"),
                    ("tests", ROOT / "apps/registry/main_client.cpp"))
        for code_scope, name in examples:
            for full in ([], ["--full"], ["--scope", "full"]):
                args = clazy.arguments(["--code-scope", code_scope, *full, str(name)])
                with self.subTest(code_scope=code_scope, full=full), mock.patch.object(
                        clazy, "select_scope") as selector:
                    with self.assertRaises(ValueError):
                        clazy.selected_files(args)
                    selector.assert_not_called()

    def test_full_cannot_hide_a_missing_explicit_file(self):
        args = clazy.arguments(["-p", str(self.root), "--code-scope", "all", "--full",
                                str(self.root / "missing.cpp")])
        with mock.patch.object(clazy, "select_scope") as selector:
            with self.assertRaisesRegex(ValueError, "absent from compile_commands"):
                clazy.selected_files(args)
            selector.assert_not_called()

    def test_empty_qt_scope_does_not_launch_a_fake_successful_worker(self):
        with mock.patch.object(clazy, "selected_files", return_value=[]), \
                mock.patch.object(clazy, "run_jobs") as dispatch, redirect_stdout(io.StringIO()):
            self.assertEqual(clazy.run(self.args()), 0)
            dispatch.assert_not_called()

    def fake_worker(self):
        worker = self.root / "fake_clazy.py"
        worker.write_text(r"""import json, os, pathlib, sys, time
source = pathlib.Path(sys.argv[-1])
started = time.monotonic()
assert os.environ['CLAZY_CHECKS_AS_ERRORS'] == 'range-loop-detach'
source.with_suffix('.started').write_text('ready', encoding='utf-8')
if int(os.environ['CLAZY_TEST_WORKERS']) > 1:
    deadline = time.monotonic() + 5
    while len(list(source.parent.glob('*.started'))) < 2:
        assert time.monotonic() < deadline, 'parallel worker did not start'
        time.sleep(0.01)
time.sleep(0.25)
result = {'start': started, 'finish': time.monotonic()}
source.with_suffix('.worker.json').write_text(json.dumps(result), encoding='utf-8')
print('checked ' + source.name)
sys.exit(1 if source.name == 'bad.cpp' else 0)
""", encoding="utf-8")
        return worker

    def run_fake_gate(self, jobs):
        worker = self.fake_worker()
        for source in self.sources:
            source.with_suffix('.started').unlink(missing_ok=True)
            source.with_suffix('.worker.json').unlink(missing_ok=True)
        constructor = clazy.CommandTask
        def redirect_task(label, command, **kwargs):
            return constructor(label=label, command=[sys.executable, str(worker), *command[1:]], **kwargs)
        with mock.patch.dict(os.environ, {"CLAZY_TEST_WORKERS": str(jobs)}), \
                mock.patch.object(clazy, "CommandTask", side_effect=redirect_task), \
                mock.patch.object(clazy, "executable_path", return_value="clazy"), \
                mock.patch.object(clazy, "enabled_checks", return_value=["range-loop-detach"]), \
                redirect_stdout(io.StringIO()), redirect_stderr(io.StringIO()):
            code = clazy.run(self.args("--jobs", str(jobs)))
        records = [json.loads(path.with_suffix('.worker.json').read_text()) for path in self.sources]
        return code, records

    def test_real_dispatch_is_bounded_and_continues_after_worker_failure(self):
        code, records = self.run_fake_gate(2)
        self.assertEqual(code, 1)
        self.assertEqual(len(records), 3)
        self.assertTrue(any(a['start'] < b['start'] < a['finish'] for a in records for b in records if a is not b))
        points = [(record['start'], 1) for record in records] + [(record['finish'], -1) for record in records]
        active = peak = 0
        for _, delta in sorted(points):
            active += delta
            peak = max(peak, active)
        self.assertEqual(peak, 2)

    def test_serial_and_parallel_have_the_same_failure_verdict(self):
        serial, serial_records = self.run_fake_gate(1)
        parallel, parallel_records = self.run_fake_gate(2)
        self.assertEqual((serial, parallel), (1, 1))
        self.assertEqual(len(serial_records), len(parallel_records))
        self.assertFalse(any(a['start'] < b['start'] < a['finish']
                             for a in serial_records for b in serial_records if a is not b))


if __name__ == "__main__":
    unittest.main()
