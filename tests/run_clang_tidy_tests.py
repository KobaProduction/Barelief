import argparse
import contextlib
import io
import os
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "scripts"))
import run_clang_tidy
from quality_scope import ScopeSelection


def options(**overrides):
    values = dict(executable=None, build_path="cmake-build-release", jobs=None,
                  full=False, scope="auto", code_scope="all", from_ref=None, to_ref=None,
                  extra_arg=["-xc++"], extra_arg_before=["-std=c++23"], files=[])
    return argparse.Namespace(**(values | overrides))


class ScopeDispatchTests(unittest.TestCase):
    def setUp(self):
        self.environment = patch.dict(os.environ, {"QUALITY_FULL": "0"})
        self.environment.start()
        self.addCleanup(self.environment.stop)

    def selected(self, args):
        with contextlib.redirect_stdout(io.StringIO()):
            return run_clang_tidy.selected_files(args, Path.cwd())

    def test_explicit_focused_files_are_sorted_and_deduplicated(self):
        with patch.object(run_clang_tidy, "select_scope") as selector:
            result = self.selected(options(files=["b.hpp", "a.cpp", "b.hpp"]))
        self.assertEqual(result, tuple(str(Path(name).resolve()) for name in ["a.cpp", "b.hpp"]))
        selector.assert_not_called()

    def test_full_overrides_focused_files(self):
        for overrides in [{"full": True}, {"scope": "full"}]:
            with self.subTest(overrides=overrides), patch.object(
                run_clang_tidy, "select_scope", return_value=ScopeSelection(True, (str(Path("header.hpp").resolve()),), "full")
            ) as selector:
                self.assertEqual(self.selected(options(files=["ignored.cpp"], **overrides)), (str(Path("header.hpp").resolve()),))
                self.assertTrue(selector.call_args.kwargs["full"])

    def test_environment_full_overrides_focused_files(self):
        with patch.dict(os.environ, {"QUALITY_FULL": "1"}), patch.object(
            run_clang_tidy, "select_scope", return_value=ScopeSelection(True, (str(Path("header.hpp").resolve()),), "full")
        ) as selector:
            self.assertEqual(self.selected(options(files=["ignored.cpp"])), (str(Path("header.hpp").resolve()),))
            self.assertTrue(selector.call_args.kwargs["full"])

    def test_git_range_is_forwarded_before_file_filtering(self):
        with patch.object(run_clang_tidy, "select_scope", return_value=ScopeSelection(False, (), "range")) as selector:
            self.selected(options(from_ref="base", to_ref="head"))
        self.assertEqual(selector.call_args.kwargs["from_ref"], "base")
        self.assertEqual(selector.call_args.kwargs["to_ref"], "head")

    def test_default_production_and_explicit_test_category_are_separate(self):
        root = Path.cwd()
        production = str(root / "apps" / "sample.cpp")
        test = str(root / "tests" / "sample.cpp")
        scope = ScopeSelection(True, (production, test), "full")
        with patch.object(run_clang_tidy, "select_scope", return_value=scope):
            self.assertEqual(self.selected(options(code_scope="production")), (production,))
            self.assertEqual(self.selected(options(code_scope="tests")), (test,))
            self.assertEqual(self.selected(options(code_scope="all")), (production, test))

    def test_explicit_cross_category_fails_even_with_force_full(self):
        for full in (False, True):
            with self.subTest(full=full), self.assertRaises(ValueError):
                self.selected(options(files=["tests/sample.cpp"], code_scope="production", full=full))

    def test_quality_full_does_not_change_test_category(self):
        root = Path.cwd()
        test = str(root / "tests" / "sample.hpp")
        scope = ScopeSelection(True, (str(root / "apps/sample.cpp"), test), "full")
        with patch.dict(os.environ, {"QUALITY_FULL": "1"}), patch.object(
            run_clang_tidy, "select_scope", return_value=scope
        ):
            self.assertEqual(self.selected(options(code_scope="tests")), (test,))


class CommandDispatchTests(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.addCleanup(self.directory.cleanup)
        self.root = Path(self.directory.name)
        (self.root / "compile_commands.json").write_text("[]")
        self.header = self.root / "inline.hpp"
        self.header.write_text("inline int value() { return 1; }")
        self.environment = patch.dict(os.environ, {
            "QUALITY_FULL": "0", "QUALITY_JOBS": "auto",
            "PRE_COMMIT_FROM_REF": "", "PRE_COMMIT_TO_REF": "",
            "QUALITY_PROGRESS_FILE": str(self.root / "progress.jsonl"),
        })
        self.environment.start()
        self.addCleanup(self.environment.stop)

    def test_keeps_header_main_file_and_existing_policy_arguments(self):
        args = options(build_path=str(self.root), files=[str(self.header)], jobs=3)
        with patch.object(run_clang_tidy.shutil, "which", return_value="clang-tidy"), patch.object(
            run_clang_tidy, "run_jobs", return_value=1
        ) as scheduler, contextlib.redirect_stdout(io.StringIO()):
            self.assertEqual(run_clang_tidy.run(args), 1)
        tasks = scheduler.call_args.args[0]
        self.assertEqual(len(tasks), 1)
        self.assertEqual(tasks[0].command[-1], str(self.header))
        self.assertIn("--extra-arg=-xc++", tasks[0].command)
        self.assertIn("--extra-arg-before=-std=c++23", tasks[0].command)
        self.assertEqual(scheduler.call_args.kwargs["jobs"], 3)
        self.assertFalse(any("header-filter" in value for value in tasks[0].command))

    def test_missing_database_fails(self):
        with self.assertRaisesRegex(ValueError, "compile_commands"):
            run_clang_tidy.run(options(build_path=str(self.root / "missing")))

    def test_missing_tool_fails(self):
        with patch.object(run_clang_tidy.shutil, "which", return_value=None):
            with self.assertRaisesRegex(ValueError, "Install clang-tidy"):
                run_clang_tidy.run(options(build_path=str(self.root)))

    def test_missing_explicit_file_fails(self):
        args = options(build_path=str(self.root), files=[str(self.root / "missing.cpp")])
        with patch.object(run_clang_tidy.shutil, "which", return_value="clang-tidy"), contextlib.redirect_stdout(io.StringIO()):
            with self.assertRaisesRegex(ValueError, "source is missing"):
                run_clang_tidy.run(args)


if __name__ == "__main__":
    unittest.main()
