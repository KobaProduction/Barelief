"""Independent metric edge cases; MIDI fixture uses the optional evaluator env."""
import importlib.util
from pathlib import Path
import tempfile
import unittest
import sys

SOURCE = Path(__file__).parents[1] / "tools/rhythm/evaluate_replay.py"
spec = importlib.util.spec_from_file_location("evaluate_replay", SOURCE)
evaluation = importlib.util.module_from_spec(spec)
spec.loader.exec_module(evaluation)
sys.path.insert(0, str(SOURCE.parent))
import run_development


class MatchingTests(unittest.TestCase):
    def test_direct_float_features_need_no_quantized_companion(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            rows = ['stream,frame,t_obs,feature,value']
            rows.extend(f'compact_float,1,0,{band},{1.25 + band / 10000}' for band in range(96))
            (root / 'features.csv').write_text('\n'.join(rows) + '\n')
            data = evaluation.read_features(root)
        self.assertGreater(data['compact_float'][0][0], 1)
        self.assertIsNone(evaluation.quantization_metrics(data))

    def test_refined_candidates_recall_is_not_rank1_accuracy(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / 'tempo_candidates.csv').write_text(
                'frame,t_obs,available,rank,bpm,score,harmonic_ambiguity\n'
                '480,8,1,0,60,.9,1\n480,8,1,1,120,.8,1\n495,8.25,0,0,0,0,0\n')
            result = run_development.refined_candidates(root, {'tempo_metadata': [{'time': 0, 'bpm': 120}]})
        self.assertEqual(result['eligible_snapshots'], 2)
        self.assertEqual(result['answers'], 1)
        self.assertEqual(result['rank1_strict_accuracy'], 0)
        self.assertEqual(result['top8_reference_recall'], 1)
        self.assertEqual(result['coverage'], .5)
        self.assertFalse(result['confirmed_tempo'])

    def test_periodicity_excludes_dc_future_tempo_and_zero_answers(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / 'tempograms.csv').write_text(
                'window,frame,t_obs,filled,bin,hz,magnitude\n'
                '4096,475,7.9,475,1,2,.9\n'
                '4096,480,8,480,0,0,1\n4096,480,8,480,1,2,.9\n'
                '4096,495,8.25,495,1,2,0\n')
            (root / 'correlations.csv').write_text(
                'window,frame,t_obs,filled,lag,seconds,coefficient\n'
                '4096,480,8,480,0,0,1\n4096,480,8,480,30,.5,.9\n'
                '4096,495,8.25,495,30,.5,0\n')
            result = run_development.periodicity(root, {'tempo_metadata': [
                {'time': 0, 'bpm': 120}, {'time': 9, 'bpm': 60}]})
        for method in ('fourier_argmax_4096', 'acf_argmax_4096'):
            self.assertEqual(result[method]['eligible_snapshots'], 2)
            self.assertEqual(result[method]['answers'], 1)
            self.assertEqual(result[method]['coverage'], .5)
            self.assertEqual(result[method]['strict_accuracy_vs_midi_clock'], 1)
            self.assertEqual(result[method]['full_history_snapshots'], 0)
            self.assertEqual(result[method]['partial_history_snapshots'], 2)

    def test_periodicity_does_not_forgive_octave_errors(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / 'tempograms.csv').write_text(
                'window,frame,t_obs,filled,bin,hz,magnitude\n512,512,9,512,1,1,.9\n')
            (root / 'correlations.csv').write_text(
                'window,frame,t_obs,filled,lag,seconds,coefficient\n512,512,9,512,60,1,.9\n')
            result = run_development.periodicity(root, {'tempo_metadata': [{'time': 0, 'bpm': 120}]})
        for method in result.values():
            self.assertEqual(method['strict_accuracy_vs_midi_clock'], 0)
            self.assertEqual(method['full_history_snapshots'], 1)

    def test_recorded_causal_fusion_is_a_separate_input(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / "events.csv").write_text("stream,t_obs,t_ready,strength\nbands,0.1,0.12,0.8\n")
            (root / "fused_events.csv").write_text("stream,t_obs,t_ready,strength,channels\nbands,0.09,0.2,0.9,3\n")
            raw = evaluation.read_predictions(root)["bands"][0]
            fused = evaluation.read_predictions(root, "fused_events.csv")["bands"][0]
        self.assertEqual((raw["time"], raw["ready"]), (.1, .12))
        self.assertEqual((fused["time"], fused["ready"]), (.09, .2))

    def test_cardinality_precedes_nearest_greedy(self):
        self.assertEqual(evaluation.optimal_matches([.05, .12], [0, .09], .06), [(0, 0), (1, 1)])

    def test_minimum_error_after_maximum_cardinality(self):
        self.assertEqual(evaluation.optimal_matches([.01, .04, .105], [0, .1], .06), [(0, 0), (2, 1)])

    def test_duplicate_predictions_are_false_positives(self):
        events = [{"time": .2, "ready": .22}, {"time": .2, "ready": .24}]
        reference = [{"time": .2}]
        result = evaluation.event_metrics(events, reference, .05)
        self.assertEqual(result["matches"], 1)
        self.assertEqual(result["precision"], .5)
        self.assertEqual(result["recall"], 1)

    def test_no_answers_do_not_imply_perfect_quality(self):
        result = evaluation.event_metrics([], [{"time": .3}], .05)
        self.assertIsNone(result["precision"])
        self.assertEqual(result["recall"], 0)
        self.assertEqual(result["f1"], 0)

    def test_grouping_is_anchored_and_keeps_all_used_readiness(self):
        events = [{"time": t, "ready": t+.01, "strength": 1} for t in [0, .02, .04]]
        result = evaluation.group_events(events)
        self.assertEqual([e["time"] for e in result], [0, .04])
        self.assertAlmostEqual(result[0]["ready"], .03)

    def test_empty_and_inclusive_tolerance(self):
        self.assertEqual(evaluation.optimal_matches([], [.1], .05), [])
        self.assertEqual(evaluation.optimal_matches([.15], [.1], .05), [(0, 0)])

    def test_midi_tempo_map_uses_seconds_and_ignores_note_off(self):
        try:
            import mido
        except ImportError:
            self.skipTest("Optional tools/rhythm/requirements.txt environment is not installed")
        midi = mido.MidiFile(ticks_per_beat=480)
        track = mido.MidiTrack()
        midi.tracks.append(track)
        track.extend([mido.MetaMessage("set_tempo", tempo=500000),
                      mido.Message("note_on", note=36, velocity=100, time=480),
                      mido.MetaMessage("set_tempo", tempo=1000000),
                      mido.Message("note_on", note=42, velocity=0, time=240),
                      mido.Message("note_on", note=38, velocity=90, time=240)])
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "timing.mid"
            midi.save(path)
            notes, groups, tempi = evaluation.midi_reference(path)
        self.assertEqual([n["time"] for n in notes], [.5, 1.5])
        self.assertEqual(len(groups), 2)
        self.assertEqual([t["bpm"] for t in tempi], [120, 60])


if __name__ == "__main__":
    unittest.main()
