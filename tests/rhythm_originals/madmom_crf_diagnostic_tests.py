"""Native probability-domain controls for the CRF diagnostic; owner runs only.

Needs the fresh diagnostic module built by madmom_crf_diagnose.py. This suite
does not substitute a Python DP implementation for either Cython variant.
"""
import argparse
import json
from pathlib import Path
import sys
import unittest

import numpy as np
import madmom_crf_diagnose as diagnosis


class NativeCrfControls(unittest.TestCase):
    original = variant = output = None

    def trace_root(self):
        root = self.output / self.id().split(".")[-1]
        root.mkdir()
        self.variant.TRACE_ROOT = str(root)
        self.variant.TRIAL_COUNTER = 0
        self.variant.CURRENT_TRIAL = None
        return root

    def valid(self, envelope, interval):
        root = self.trace_root()
        actual, score = self.variant.best_sequence(envelope, interval, .18)
        expected, expected_score = self.original.crf.best_sequence(envelope, interval, .18)
        np.testing.assert_array_equal(actual, expected)
        self.assertEqual(np.float64(score).tobytes(), np.float64(expected_score).tobytes())
        self.assertTrue(np.isfinite(score))
        self.assertEqual(len(actual), len(envelope) // interval)
        self.assertTrue(np.all(np.diff(actual) > 0))
        self.assertTrue(np.all((actual >= 0) & (actual < len(envelope))))
        (root / "comparison.json").write_text(json.dumps({
            "score": float(score), "path": actual.tolist(),
            "native_score_bits_equal": True, "native_path_equal": True,
        }, indent=2), encoding="utf8")

    def fault(self, envelope, interval, reason):
        root = self.trace_root()
        with self.assertRaisesRegex(RuntimeError, reason):
            self.variant.best_sequence(envelope, interval, .18)
        records = [json.loads(line) for line in (root / "trial-0000/trace.jsonl").read_text().splitlines()]
        self.assertEqual(records[-1]["event"], "FIRST_ILLEGAL_OPERATION")
        self.assertEqual(records[-1]["reason"], reason)

    def test_uniform_positive_probabilities_keep_native_scores_and_path(self):
        self.valid(np.full(192, .2, dtype=np.float32), 24)

    def test_positive_probability_peaks_keep_native_scores_and_path(self):
        values = np.full(192, .01, dtype=np.float32)
        values[12::24] = .9
        self.valid(values, 24)

    def test_single_beat_variable_keeps_native_scores_and_path(self):
        self.valid(np.full(16, .2, dtype=np.float32), 16)

    def test_nonprobability_positive_control_is_not_silently_normalized(self):
        self.valid(np.full(192, 20., dtype=np.float32), 24)

    def test_zero_probability_input_stops_before_uninitialized_terminal(self):
        self.fault(np.zeros(16, dtype=np.float32), 4, "uninitialized_terminal")

    def test_zero_initial_support_with_later_probability_evidence(self):
        values = np.zeros(160, dtype=np.float32)
        values[100:130] = .5
        self.fault(values, 50, "uninitialized_terminal")

    def test_short_nonempty_probability_input_has_no_beat_variables(self):
        self.fault(np.full(8, .5, dtype=np.float32), 24, "no_beat_variables")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("cache", type=Path)
    parser.add_argument("module_root", type=Path)
    parser.add_argument("output", type=Path)
    args = parser.parse_args()
    diagnosis.pinned_source(args.cache)
    args.output.mkdir(parents=True, exist_ok=False)
    NativeCrfControls.original = diagnosis.load_original(args.cache)
    NativeCrfControls.variant = diagnosis.load_variant(args.module_root, args.output)
    NativeCrfControls.output = args.output
    suite = unittest.defaultTestLoader.loadTestsFromTestCase(NativeCrfControls)
    result = unittest.TextTestRunner(verbosity=2).run(suite)
    return 0 if result.wasSuccessful() else 1


if __name__ == "__main__":
    sys.exit(main())
