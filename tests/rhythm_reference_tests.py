"""Readiness and source-contract checks for the optional library reference."""
import hashlib
import json
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).parents[1] / 'tools/rhythm'))
import reference_librosa as reference
import numpy as np


class ReferenceTests(unittest.TestCase):
    def fixture(self, root, value='.25', available=1):
        (root / 'graph.json').write_text('{}')
        (root / 'summary.json').write_text(json.dumps({
            'graph_sha256': hashlib.sha256((root / 'graph.json').read_bytes()).hexdigest(),
            'analysis_rate': 45, 'analysis_frames': 2, 'source_rate': 48000}))
        (root / 'runtime.csv').write_text(
            f'frame,t_obs,port,available,value\n1,{1/45},7,{available},{value}\n2,{2/45},7,1,.5\n')

    def test_activity_uses_analysis_cadence_and_real_source_times(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            self.fixture(root)
            _, times, values = reference.read_activity(root)
            np.testing.assert_array_equal(times, [1/45, 2/45])
            np.testing.assert_array_equal(values, [.25, .5])
            (root / 'graph.json').write_text('{"changed":true}')
            with self.assertRaisesRegex(ValueError, 'hash'):
                reference.read_activity(root)

    def test_nonunit_or_unavailable_activity_is_not_silently_normalized(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            self.fixture(root, value='1.01')
            with self.assertRaisesRegex(ValueError, 'normalized'):
                reference.read_activity(root)
            self.fixture(root, available=0)
            with self.assertRaisesRegex(ValueError, 'unavailable'):
                reference.read_activity(root)

    def test_prefix_tempo_never_receives_future_samples(self):
        times = np.arange(1, 451) / 45
        values = np.linspace(0, 1, len(times))
        lengths = []

        def tempo(**kwargs):
            lengths.append(len(kwargs['onset_envelope']))
            return np.array([120.])

        with patch.object(reference.librosa.beat, 'beat_track', return_value=(120., np.array([], dtype=int))), \
                patch.object(reference.librosa.feature, 'tempo', side_effect=tempo), \
                patch.object(reference.librosa.beat, 'plp', side_effect=lambda **kwargs: np.zeros_like(kwargs['onset_envelope'])):
            result = reference.predict(times, values, 45)
        self.assertEqual(lengths, [360, 405, 450])
        for row in result['prefix_tempo']:
            self.assertLessEqual(row['last_input_t_obs'], row['available_at'])
        self.assertIn('future data available', result['full_track_DP']['context'])


if __name__ == '__main__':
    unittest.main()
