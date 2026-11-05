"""Mock tracker lifecycle and strict aggregation without original imports."""
import ast
import hashlib
import math
from pathlib import Path
import subprocess
import sys
import tempfile
import types
import unittest
from unittest.mock import Mock


def isolated_namespace(root):
    source = Path(__file__).with_name("madmom_pipeline_oracle.py")
    tree = ast.parse(source.read_text(encoding="utf8"))
    functions = [node for node in tree.body if isinstance(node, ast.FunctionDef)
                 and node.name in ("worker_failure_status", "raw_result",
                                   "returned_path_evidence", "verify_rejections")]
    namespace = {"Path": Path, "sys": sys, "os": types.SimpleNamespace(name="posix"),
                 "hashlib": hashlib, "math": math, "__file__": str(source), "TRACKER_ROOT": root,
                 "TRACKER_CACHE": root, "TRACKER_COUNTER": 0,
                 "np": types.SimpleNamespace(save=lambda output, *args, **kwargs: output.write(b"fixture"))}
    exec(compile(ast.Module(body=functions, type_ignores=[]), str(source), "exec"), namespace)
    return namespace


class ReturnedEvents:
    def __init__(self, values):
        self.values = values
        self.ndim = 1
        self.shape = (len(values),)

    def tolist(self):
        return list(self.values)


def rejection_namespace(ends, workers):
    namespace = isolated_namespace(Path("unused"))
    namespace["np"].fromfile = Mock(return_value=ends)
    namespace["read"] = Mock(return_value=list(range(240)))
    namespace["raw_result"] = Mock(side_effect=workers)
    namespace["NUMERICAL_FAILURES"] = []
    return namespace


class RejectionAggregationTests(unittest.TestCase):
    def test_returned_invalid_path_is_strict_and_later_snapshot_retained(self):
        returned = {"status": "RETURNED", "worker": "original-161", "count": 3}
        rejected = {"status": "ORIGINAL_REJECT", "worker": "original-201"}
        namespace = rejection_namespace([161, 201], [
            (returned, ReturnedEvents([.055, .02, .01])), (rejected, None)])
        report = {}
        namespace["verify_rejections"](None, Path("unused"), 2, report)
        self.assertEqual(report["rejected_snapshots_status"], "STRICT_FAIL")
        self.assertEqual(len(report["rejected_snapshots"]), 2)
        self.assertEqual(len(namespace["NUMERICAL_FAILURES"]), 1)
        row = report["rejected_snapshots"][0]
        self.assertEqual(row["raw_original"], returned)
        self.assertEqual(row["source_return"]["events"], [.055, .02, .01])
        self.assertEqual(row["source_return"]["classification"], "VIOLATES_PINNED_CRF_BACKTRACE_ORDER")
        self.assertEqual(report["rejected_snapshots"][1]["status"], "ORIGINAL_REJECT")
        self.assertEqual(namespace["raw_result"].call_count, 2)

    def test_successful_empty_bridge_is_still_strict_mismatch(self):
        namespace = rejection_namespace([161], [
            ({"status": "ORIGINAL_REJECT"}, None),
            ({"status": "RETURNED", "worker": "bridge-empty"}, ReturnedEvents([]))])
        report = {}
        namespace["verify_rejections"](None, Path("unused"), 0, report)
        row = report["rejected_snapshots"][0]
        self.assertEqual(row["status"], "STRICT_FAIL")
        self.assertEqual(row["explicit_estimator_bridge"]["status"], "RETURNED")
        self.assertEqual(row["source_return"]["classification"], "EMPTY_SOURCE_RETURN")
        self.assertEqual(namespace["raw_result"].call_count, 2)
        self.assertEqual(namespace["raw_result"].call_args.kwargs, {"bridge": True})

    def test_oracle_setup_and_output_failures_remain_hard(self):
        for status in ("ORACLE_SETUP_REJECT", "ORACLE_OUTPUT_REJECT"):
            with self.subTest(status=status):
                namespace = rejection_namespace([161], [({"status": status}, None)])
                with self.assertRaises(AssertionError):
                    namespace["verify_rejections"](None, Path("unused"), 2, {})
                self.assertEqual(namespace["NUMERICAL_FAILURES"], [])
                namespace = rejection_namespace([161], [
                    ({"status": "ORIGINAL_REJECT"}, None), ({"status": status}, None)])
                with self.assertRaises(AssertionError):
                    namespace["verify_rejections"](None, Path("unused"), 0, {})
                self.assertEqual(namespace["NUMERICAL_FAILURES"], [])

    def test_matching_source_rejections_remain_accepted(self):
        namespace = rejection_namespace([1, 41], [
            ({"status": "ORIGINAL_REJECT"}, None), ({"status": "ORIGINAL_REJECT"}, None)])
        report = {}
        namespace["verify_rejections"](None, Path("unused"), 2, report)
        self.assertEqual(report["rejected_snapshots_status"], "ORIGINAL_REJECT")
        self.assertEqual(namespace["NUMERICAL_FAILURES"], [])


class TrackerTimeoutTests(unittest.TestCase):
    def timeout_case(self, started, returned, expected):
        with tempfile.TemporaryDirectory() as name:
            root = Path(name)
            namespace = isolated_namespace(root)

            def deadline(*args, **kwargs):
                directory = root / "0000"
                if started:
                    (directory / "started").touch()
                if returned:
                    (directory / "returned").touch()
                raise subprocess.TimeoutExpired(args[0], 30, output=b"stdout", stderr=b"stderr")

            namespace["subprocess"] = types.SimpleNamespace(
                run=Mock(side_effect=deadline), TimeoutExpired=subprocess.TimeoutExpired)
            record, events = namespace["raw_result"](2, object())
            self.assertEqual(record["status"], expected)
            self.assertEqual(record["kind"], "TIMEOUT")
            self.assertIsNone(events)
            self.assertEqual((root / "0000" / "stdout.log").read_bytes(), b"stdout")
            namespace["subprocess"].run.assert_called_once()

    def test_timeout_before_source_call_is_setup_failure(self):
        self.timeout_case(False, False, "ORACLE_SETUP_REJECT")

    def test_timeout_inside_source_call_is_original_rejection(self):
        self.timeout_case(True, False, "ORIGINAL_REJECT")

    def test_timeout_after_source_return_is_output_failure(self):
        self.timeout_case(True, True, "ORACLE_OUTPUT_REJECT")


if __name__ == "__main__":
    unittest.main()
