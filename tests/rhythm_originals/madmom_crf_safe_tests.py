"""Owner-only native controls for defined CRF, including measured mixed trials."""
import argparse
import json
from pathlib import Path
import sys
import types
import unittest

import numpy as np
import madmom_crf_diagnose as diagnosis
import madmom_crf_safe_oracle as defined


class DefinedCrfTests(unittest.TestCase):
    original = module = module_root = output = None

    def compare_valid(self, values, interval):
        actual, score = self.module.best_sequence(values, interval, .18)
        expected, raw_score = self.original.crf.best_sequence(values, interval, .18)
        np.testing.assert_array_equal(actual, expected)
        self.assertEqual(np.float64(score).tobytes(), np.float64(raw_score).tobytes())
        self.assertTrue(np.isfinite(score))
        self.assertTrue(np.all(np.diff(actual) > 0))
        self.assertTrue(np.all((actual >= 0) & (actual < len(values))))

    def no_path(self, values, interval):
        path, score = self.module.best_sequence(values, interval, .18)
        self.assertEqual(path.size, 0)
        self.assertEqual(score, -np.inf)

    def envelope(self, name):
        base = diagnosis.REPO / "cmake-build-release/Testing/four-pipelines-20261102"
        return np.load(base / name / "envelope.npy", allow_pickle=False)

    def test_positive_probability_path_and_score_unchanged(self):
        values = np.full(192, .01, dtype=np.float32)
        values[12::24] = .9
        self.compare_valid(values, 24)

    def test_uniform_probability_path_and_score_unchanged(self):
        self.compare_valid(np.full(192, .2, dtype=np.float32), 24)

    def test_positive_nonprobability_control_not_normalized(self):
        self.compare_valid(np.full(192, 20., dtype=np.float32), 24)

    def test_single_beat_variable_unchanged(self):
        self.compare_valid(np.full(16, .2, dtype=np.float32), 16)

    def test_zero_probability_has_defined_no_path(self):
        self.no_path(np.zeros(16, dtype=np.float32), 4)

    def test_zero_prior_support_with_later_probability_evidence(self):
        values = np.zeros(160, dtype=np.float32)
        values[100:130] = .5
        self.no_path(values, 50)

    def test_short_probability_input_has_defined_no_path(self):
        self.no_path(np.full(8, .5, dtype=np.float32), 24)

    def test_empty_input_has_defined_no_path(self):
        self.no_path(np.empty(0, dtype=np.float32), 24)

    def test_invalid_activation_is_rejected_not_replaced(self):
        for value in (-.1, np.nan, np.inf):
            with self.subTest(value=value):
                values = np.full(160, .5, dtype=np.float32)
                values[80] = value
                with self.assertRaises(ValueError):
                    self.module.best_sequence(values, 50, .18)

    def test_invalid_interval_and_sigma_are_rejected(self):
        for interval, sigma in ((0, .18), (-1, .18), (1.5, .18), (24, 0), (24, np.nan)):
            with self.subTest(interval=interval, sigma=sigma):
                with self.assertRaises(ValueError):
                    self.module.best_sequence(np.full(160, .5, dtype=np.float32), interval, sigma)

    def test_measured_valid_trial_with_other_nan_states_is_kept(self):
        values = self.envelope("madmom-crf-v33-workers-4c1he0gv/0000")
        smoothed = self.original.signal.smooth(values, 18)
        self.compare_valid(np.ascontiguousarray(smoothed, dtype=np.float32), 100)

    def test_measured_full_mixed_trials_keep_valid_native_path(self):
        values = self.envelope("madmom-crf-v33-workers-4c1he0gv/0000")
        trials = []
        processor = defined.processor(self.original, self.module, self.module_root, trials)
        actual = processor.process(values)
        self.assertTrue(actual.size)
        self.assertTrue(any(row["status"] == "VALID" for row in trials))
        self.assertTrue(any(row["status"] == "NO_RESULT" for row in trials))
        first = next(row for row in trials if row["interval"] == 100)
        self.assertEqual(first["status"], "VALID")
        smoothed = np.ascontiguousarray(self.original.signal.smooth(values, 18), dtype=np.float32)
        expected, score = self.original.crf.best_sequence(smoothed, 100, .18)
        self.assertEqual(first["score"], score)
        self.assertEqual(first["positions"], expected.tolist())
        valid = [row for row in trials if row["status"] == "VALID"]
        winner = max(valid, key=lambda row: row["normalized_score"])
        np.testing.assert_array_equal(actual, np.asarray(winner["positions"], dtype=float) / 200)
        (self.output / "mixed-historical-full.json").write_text(json.dumps({
            "events": actual.tolist(), "trials": trials}, indent=2), encoding="utf8")

    def test_measured_short_all_trials_have_no_path(self):
        values = self.envelope("madmom-parity-v31-workers-omqq514p/0013")
        trials = []
        actual = defined.processor(self.original, self.module, self.module_root, trials).process(values)
        self.assertEqual(actual.size, 0)
        self.assertTrue(trials)
        self.assertTrue(all(row["status"] == "NO_RESULT" and row["score"] == -np.inf for row in trials))

    def test_wrapper_empty_and_silence_no_tempo_are_explicit_no_result(self):
        for values in (np.empty(0, dtype=np.float32), np.zeros(160, dtype=np.float32)):
            result = defined.processor(self.original, self.module, self.module_root).process(values)
            self.assertEqual(result.size, 0)

    def test_ordered_mixed_short_and_valid_wrapper_trials(self):
        values = np.full(192, .2, dtype=np.float32)
        trials = []
        processor = defined.processor(self.original, self.module, self.module_root, trials)
        processor.tempo_estimator = types.SimpleNamespace(
            process=lambda _: np.array([[40., .5], [500., .5]]), act_smooth=.09)
        result = processor.process(values)
        self.assertEqual([row["interval"] for row in trials], [300, 24])
        self.assertEqual([row["status"] for row in trials], ["NO_RESULT", "VALID"])
        np.testing.assert_array_equal(result, np.asarray(trials[1]["positions"], dtype=float) / 200)

    def test_double_trial_ranking_preserves_close_scores_and_first_tie(self):
        # Both are possible Float32 terminal scores; Float32 division collapses
        # their distinct native Double normalized scores to the same value.
        trials = [(np.zeros(3, dtype=int), 1.), (np.zeros(2, dtype=int), float(np.float32(2 / 3)))]
        scores = defined.defined_scores(trials)
        self.assertEqual(scores.dtype, np.dtype("float64"))
        self.assertEqual(int(scores.argmax()), 1)
        self.assertEqual(int(defined.defined_scores([trials[0], trials[0]]).argmax()), 0)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("cache", "module_root", "output"):
        parser.add_argument(name, type=Path)
    args = parser.parse_args()
    diagnosis.pinned_source(args.cache)
    args.output.mkdir(parents=True, exist_ok=False)
    DefinedCrfTests.original = diagnosis.load_original(args.cache)
    DefinedCrfTests.module = defined.load_variant(args.module_root)
    DefinedCrfTests.module_root, DefinedCrfTests.output = args.module_root, args.output
    result = unittest.TextTestRunner(verbosity=2).run(unittest.defaultTestLoader.loadTestsFromTestCase(DefinedCrfTests))
    return 0 if result.wasSuccessful() else 1


if __name__ == "__main__":
    sys.exit(main())
