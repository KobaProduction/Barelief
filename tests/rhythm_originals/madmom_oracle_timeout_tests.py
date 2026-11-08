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
                 and node.name in ("worker_failure_status", "raw_result", "source_tracker", "verify_short_rejection")]
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


class SourceAuthorityTests(unittest.TestCase):
    def namespace(self):
        namespace = isolated_namespace(Path("unused"))
        namespace["np"].asarray = lambda values: values
        namespace["np"].isfinite = lambda values: types.SimpleNamespace(all=lambda: True)
        return namespace

    def test_failed_source_authority_never_falls_back_to_graph(self):
        for status in ("ORIGINAL_REJECT", "ORACLE_SETUP_REJECT", "ORACLE_OUTPUT_REJECT"):
            with self.subTest(status=status):
                namespace = self.namespace()
                namespace["raw_result"] = Mock(return_value=({"status": status}, None))
                with self.assertRaises(AssertionError):
                    namespace["source_tracker"](0, object(), Path("defined"))
                self.assertEqual(namespace["raw_result"].call_args.kwargs, {"bridge": True})

    def test_successful_empty_source_is_retained_for_strict_comparison(self):
        namespace = self.namespace()
        events = ReturnedEvents([])
        namespace["raw_result"] = Mock(return_value=({"status": "RETURNED"}, events))
        record, actual = namespace["source_tracker"](1, object(), Path("defined"))
        self.assertEqual(record["status"], "RETURNED")
        self.assertIs(actual, events)

    def test_defined_crf_has_no_unsafe_raw_fallback(self):
        namespace = self.namespace()
        expected = ({"status": "NO_RESULT"}, ReturnedEvents([]))
        namespace["defined_result"] = Mock(return_value=expected)
        namespace["raw_result"] = Mock(side_effect=AssertionError("unsafe raw fallback"))
        self.assertEqual(namespace["source_tracker"](2, object(), Path("defined")), expected)
        namespace["raw_result"].assert_not_called()

    def test_successful_empty_source_against_graph_rejection_stays_strict(self):
        namespace = self.namespace()
        rejected = {"status": "ORIGINAL_REJECT", "kind": "PYTHON_EXCEPTION", "exception": "IndexError"}
        namespace["NUMERICAL_FAILURES"] = []
        namespace["raw_result"] = Mock(side_effect=[({"status": "RETURNED"}, ReturnedEvents([])), (rejected, None)])
        record = namespace["verify_short_rejection"](0, object(), object(), rejected)
        self.assertEqual(record["status"], "STRICT_FAIL")
        self.assertEqual(len(namespace["NUMERICAL_FAILURES"]), 1)
        self.assertEqual(namespace["raw_result"].call_count, 2)
        self.assertEqual(record["original_pcm"], rejected)

    def test_nonfinite_successful_source_is_a_hard_failure(self):
        namespace = self.namespace()
        namespace["np"].isfinite = lambda values: types.SimpleNamespace(all=lambda: False)
        namespace["raw_result"] = Mock(return_value=({"status": "RETURNED"}, ReturnedEvents([math.nan])))
        with self.assertRaises(AssertionError):
            namespace["source_tracker"](0, object(), Path("defined"))


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
