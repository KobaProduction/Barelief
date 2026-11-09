"""Regression checks for push-test selection and the release packaging gate."""

import json
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch
from types import SimpleNamespace

import yaml

import run_ctest


ROOT = Path(__file__).resolve().parent.parent


class CTestGateTests(unittest.TestCase):
    def prepared_build(self, root, executable=None):
        build = root / "cmake-build-release"
        build.mkdir()
        (build / "CTestTestfile.cmake").touch()
        line = f"CMAKE_CTEST_COMMAND:INTERNAL={executable}\n" if executable else ""
        (build / "CMakeCache.txt").write_text(line, encoding="utf-8")

    def test_configured_ctest_with_spaces_precedes_path(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            executable = root / "CMake tools" / "ctest.exe"
            executable.parent.mkdir()
            executable.touch()
            self.prepared_build(root, executable)
            with patch("run_ctest.shutil.which") as lookup:
                self.assertEqual(run_ctest.ctest_executable(root), str(executable))
                lookup.assert_not_called()

    def test_missing_build_cannot_report_a_successful_push(self):
        with tempfile.TemporaryDirectory() as directory, patch("run_ctest.subprocess.run") as execute:
            self.assertEqual(run_ctest.main(Path(directory)), 1)
            execute.assert_not_called()

    def test_stale_ctest_location_falls_back_to_path(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            self.prepared_build(root, root / "removed" / "ctest")
            with patch("run_ctest.shutil.which", return_value="installed-ctest"):
                self.assertEqual(run_ctest.ctest_executable(root), "installed-ctest")

    def test_test_failure_is_returned_to_the_hook(self):
        with patch("run_ctest.ctest_executable", return_value="ctest"), \
                patch("run_ctest.subprocess.run", return_value=SimpleNamespace(returncode=8)) as execute:
            self.assertEqual(run_ctest.main(ROOT), 8)
            execute.assert_called_once_with(
                ["ctest", "--preset", "local-push", "--no-tests=error"], cwd=ROOT)

    def test_push_and_release_presets_have_separate_selection(self):
        presets = {preset["name"]: preset for preset in json.loads((ROOT / "CMakePresets.json").read_text())["testPresets"]}
        self.assertEqual(presets["local-push"]["inherits"], "release")
        self.assertEqual(presets["release"]["inherits"], "quick-tests")
        self.assertEqual(presets["quick-tests"]["filter"]["include"]["label"], "quick")
        self.assertEqual(set(presets["quick-tests"]["filter"]["exclude"]["label"].split("|")),
                         {"integration", "audit", "native", "long"})
        self.assertEqual(presets["release-validation"]["inherits"], "release-all")
        for name in ("release-validation", "release-all", "parallel-tests"):
            self.assertNotIn("filter", presets[name])

    def test_release_promotion_and_manual_validation_require_full_gate(self):
        workflow = yaml.load((ROOT / ".github/workflows/release-validation.yml").read_text(), Loader=yaml.BaseLoader)
        self.assertEqual(workflow["on"]["push"]["branches"], ["release"])
        self.assertIn("workflow_dispatch", workflow["on"])
        job = workflow["jobs"]["validate-and-package"]
        self.assertEqual(job["needs"], "version")
        self.assertEqual(job["uses"], "./.github/workflows/windows.yml")
        self.assertEqual(job["with"]["release_validation"], "true")
        publish = workflow["jobs"]["publish"]
        self.assertEqual(set(publish["needs"]), {"version", "validate-and-package", "quality", "esp32-sdk"})
        for name, filename in (("quality", "code-quality.yml"), ("esp32-sdk", "esp32-sdk.yml")):
            dependency = workflow["jobs"][name]
            self.assertEqual(dependency["uses"], f"./.github/workflows/{filename}")
            self.assertNotIn("if", dependency)
            required = yaml.load((ROOT / ".github/workflows" / filename).read_text(), Loader=yaml.BaseLoader)
            self.assertIn("workflow_call", required["on"])
            self.assertIn("release", required["on"]["push"]["branches-ignore"])
        self.assertEqual(publish["permissions"]["contents"], "write")
        self.assertIn("inputs.publish", publish["if"])
        self.assertEqual(workflow["on"]["workflow_dispatch"]["inputs"]["publish"]["default"], "false")
        self.assertEqual(workflow["concurrency"]["cancel-in-progress"], "false")

    def test_packaging_follows_full_tests_and_is_release_only(self):
        workflow = yaml.load((ROOT / ".github/workflows/windows.yml").read_text(), Loader=yaml.BaseLoader)
        self.assertNotIn("tags", workflow["on"]["push"])
        self.assertIn("release", workflow["on"]["push"]["branches-ignore"])
        self.assertIn("workflow_call", workflow["on"])
        windows = workflow["jobs"]["build"]["steps"]
        full = next(i for i, step in enumerate(windows) if step.get("name") == "Full release validation")
        package = next(i for i, step in enumerate(windows) if step.get("name") == "Package Windows release")
        self.assertLess(full, package)
        self.assertNotIn(" -L", windows[full]["run"])
        self.assertIn("inputs.release_validation", windows[package]["if"])
        self.assertNotIn("always()", windows[package]["if"])
        linux = workflow["jobs"]["linux"]["steps"]
        tests = next(i for i, step in enumerate(linux) if step.get("name") == "Test GUI using virtual X11 display")
        package = next(i for i, step in enumerate(linux) if step.get("name") == "Package Linux installer")
        self.assertLess(tests, package)
        self.assertIn("inputs.release_validation", linux[package]["if"])


if __name__ == "__main__":
    unittest.main()
