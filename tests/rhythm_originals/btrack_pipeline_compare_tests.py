"""Evidence freshness regression; no original/DSP execution or algorithm oracle."""
import argparse
import importlib.util
from pathlib import Path
import subprocess
import tempfile
import unittest
from unittest.mock import patch

MODULE = Path(__file__).with_name("btrack_pipeline_compare.py")
SPEC = importlib.util.spec_from_file_location("btrack_pipeline_compare", MODULE)
COMPARATOR = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(COMPARATOR)


class FreshOracleEvidenceTests(unittest.TestCase):
    def test_preserves_existing_report(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            output = Path(temporary) / "result.json"
            output.write_text("prior evidence", encoding="utf8")
            with self.assertRaises(FileExistsError):
                COMPARATOR.fresh_run_root(output)
            self.assertEqual(output.read_text(encoding="utf8"), "prior evidence")
            self.assertFalse((output.parent / "result-runs").exists())

    def test_rejects_interrupted_run_without_removing_outputs(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            output = Path(temporary) / "result.json"
            root = COMPARATOR.fresh_run_root(output)
            marker = root / "prior.odf.f32"
            marker.write_bytes(b"unchanged")
            with self.assertRaises(FileExistsError):
                COMPARATOR.fresh_run_root(output)
            self.assertEqual(marker.read_bytes(), b"unchanged")

    def test_empty_success_cannot_consume_old_prefix_files(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            parent = Path(temporary)
            output = parent / "result.json"
            (parent / "result-raw-original.odf.f32").write_bytes(b"stale")
            options = argparse.Namespace(output=output, run_root=COMPARATOR.fresh_run_root(output),
                                         graph_prefix=parent / "graph", original_captures_raw_src=False)
            binary = parent / "original.exe"
            binary.write_bytes(b"fixture identity only")
            returned = subprocess.CompletedProcess([], 0, stdout="", stderr="")
            with patch.object(COMPARATOR.subprocess, "run", return_value=returned):
                result = COMPARATOR.run_original(binary, "raw-original", options)
            self.assertEqual(result["status"], "ORACLE_OUTPUT_REJECT")
            self.assertFalse(output.exists())
            self.assertTrue((parent / "result-raw-original.odf.f32").exists())


if __name__ == "__main__":
    unittest.main()
