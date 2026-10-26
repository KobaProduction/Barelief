"""Release package probe contract and failure cleanup, without GUI or Linux tools."""
import importlib.util
import json
from pathlib import Path
import subprocess
import tempfile
import unittest
from unittest.mock import MagicMock, patch


SCRIPT = Path(__file__).resolve().parents[1] / "scripts/verify-linux-package.py"
SPEC = importlib.util.spec_from_file_location("verify_linux_package", SCRIPT)
PACKAGE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(PACKAGE)


class PackageGuiTests(unittest.TestCase):
    def run_probe(self, report=None, output="", missing=False, failure=None):
        if report is None:
            report = {"schema": 1, "kind": "packaged_gui_delivery", "sample_started": True,
                      "numeric_frames_delivered": 7, "history_rows_delivered": 4,
                      "history_snapshots_delivered": 1}
        engine = MagicMock()
        engine.poll.return_value = None

        def gui(arguments, **options):
            self.gui_arguments, self.gui_options = arguments, options
            if failure:
                raise failure
            options["stdout"].write(output)
            if not missing:
                Path(options["env"]["BARELIEF_PACKAGE_PROBE_FILE"]).write_text(json.dumps(report))
            return subprocess.CompletedProcess(arguments, 0)

        with tempfile.TemporaryDirectory() as directory:
            temporary = Path(directory)
            evidence = temporary / "evidence"
            evidence.mkdir()
            with patch.object(PACKAGE.subprocess, "Popen", return_value=engine), \
                    patch.object(PACKAGE.subprocess, "run", side_effect=gui), \
                    patch.object(PACKAGE, "wait_for_server"):
                try:
                    return PACKAGE.check_gui(temporary / "installed", {}, temporary, evidence)
                finally:
                    self.engine_stopped = engine.terminate.called

    def test_release_uses_passive_probe_and_retains_delivery_checks(self):
        self.assertEqual(self.run_probe(), {"numeric_frames_delivered": 7,
                                         "history_rows_delivered": 4,
                                         "history_snapshots_delivered": 1})
        self.assertTrue(self.engine_stopped)
        self.assertEqual(Path(self.gui_arguments[0]).name, "Barelief")
        self.assertIn("--remote-host", self.gui_arguments)
        self.assertNotIn("CORE_APP_TRACE_FILE", self.gui_options["env"])
        self.assertEqual(self.gui_options["timeout"], 35)

    def test_no_delivery_is_rejected(self):
        report = {"schema": 1, "kind": "packaged_gui_delivery", "sample_started": True,
                  "numeric_frames_delivered": 0, "history_rows_delivered": 0,
                  "history_snapshots_delivered": 0}
        with self.assertRaisesRegex(RuntimeError, "did not receive"):
            self.run_probe(report)
        self.assertTrue(self.engine_stopped)

    def test_missing_release_probe_is_rejected(self):
        with self.assertRaisesRegex(RuntimeError, "did not produce"):
            self.run_probe(missing=True)
        self.assertTrue(self.engine_stopped)

    def test_old_developer_trace_is_not_package_evidence(self):
        with self.assertRaisesRegex(RuntimeError, "unsupported delivery report"):
            self.run_probe({"sample_started": True, "numeric_frames_delivered": 7})

    def test_qml_dependency_failure_is_rejected(self):
        with self.assertRaisesRegex(RuntimeError, "QML dependencies"):
            self.run_probe(output='module "QtQuick.Controls" is not installed')
        self.assertTrue(self.engine_stopped)

    def test_timeout_cleans_up_synthetic_engine(self):
        with self.assertRaises(subprocess.TimeoutExpired):
            self.run_probe(failure=subprocess.TimeoutExpired("Barelief", 35))
        self.assertTrue(self.engine_stopped)


if __name__ == "__main__":
    unittest.main()
