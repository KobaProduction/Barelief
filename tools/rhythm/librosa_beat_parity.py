"""Pinned DP beat-path parity, separate from tempo estimation and PCM frontend.

Requires librosa==1.0.0. Exercises production Float32 kernel through the test
binary, checking the final mask and intermediate local/cumulative DP scores.
This is mathematical compatibility, not human beat/grid accuracy.
"""
import argparse
import hashlib
import itertools
import json
from pathlib import Path
import subprocess
import sys
import tempfile

import librosa
import numpy as np


def fixtures():
    rng = np.random.default_rng(28041)
    for frames in [17, 257, 2048]:
        for rate in [60.0, 44100 / 512]:
            random = rng.random(frames).astype(np.float32)
            pulses = np.zeros(frames, dtype=np.float32)
            pulses[np.arange(5, frames, 31)] = rng.uniform(.5, 2, len(pulses[5::31]))
            for name, onset in [('random', random), ('pulses', pulses)]:
                for mode in ['scalar', 'constant', 'ramp', 'step']:
                    if mode == 'scalar':
                        bpm = np.array([123.0], dtype=np.float32)
                    elif mode == 'constant':
                        bpm = np.full(frames, 123, dtype=np.float32)
                    elif mode == 'ramp':
                        bpm = np.linspace(65, 180, frames, dtype=np.float32)
                    else:
                        bpm = np.where(np.arange(frames) < frames // 2, 90, 150).astype(np.float32)
                    yield f'{frames}-{rate}-{name}-{mode}', onset, bpm, rate
    # Banker's rounding of odd half-periods and nearest-even frame periods.
    for period in [2, 3, 5, 31.5, 32.5, 43.5]:
        yield f'round-{period}', rng.random(257).astype(np.float32), np.array([120], np.float32), period * 2


def expected(onset, bpm, rate, tightness, trim):
    periods = np.round(rate * 60 / bpm)
    local = np.empty_like(onset)
    librosa.beat.__beat_local_score(librosa.beat.__normalize_onsets(onset), periods, local)
    backlink, cumulative = librosa.beat.__beat_track_dp(local, periods, np.float32(tightness))
    _, beats = librosa.beat.beat_track(onset_envelope=onset, sr=rate, hop_length=1,
                                     bpm=bpm, tightness=tightness, trim=trim, sparse=False)
    return beats, local, cumulative, backlink


def run(executable):
    cases = 0
    tied_links = 0
    exact_masks = 0
    alternate_paths = []
    largest = {'local': 0., 'cumulative': 0.}
    with tempfile.TemporaryDirectory(prefix='barelief-beat-parity-') as temporary:
        root = Path(temporary)
        for name, onset, bpm, rate in fixtures():
            print(f'checking {name}', file=sys.stderr, flush=True)
            onset.tofile(root / 'onset.f32')
            bpm.tofile(root / 'tempo.f32')
            for tightness, trim in itertools.product([10., 100., 400.], [False, True]):
                wanted = expected(onset, bpm, rate, tightness, trim)
                subprocess.run([str(executable.resolve()), str(root / 'onset.f32'), str(root / 'tempo.f32'),
                                str(root / 'result'), str(len(onset)), str(len(bpm)), str(rate),
                                str(tightness), str(int(trim)), '2048'], check=True, capture_output=True)
                actual = np.fromfile(root / 'result.u8', dtype=np.uint8)
                diagnostics = {}
                for key, extension, expectation in [('local', 'local.f32', wanted[1]),
                                                     ('cumulative', 'cumulative.f32', wanted[2])]:
                    values = np.fromfile(root / f'result-{extension}', dtype=np.float32)
                    diagnostics[key] = values
                    np.testing.assert_allclose(values, expectation, rtol=2e-5, atol=1e-4,
                                               err_msg=f'{name}/{tightness}/{trim}/{key}')
                    largest[key] = max(largest[key], float(np.max(np.abs(values - expectation))))
                links = np.fromfile(root / 'result-backlink.i32', dtype=np.int32)
                ties = verify_links(links, wanted[3], wanted[2], bpm, rate, tightness)
                tied_links += ties
                verify_reconstruction(actual, links, diagnostics, trim)
                if np.array_equal(actual, wanted[0]):
                    exact_masks += 1
                else:
                    # A mixed-precision original and a Float32 implementation
                    # need not break numerically tied predecessor paths alike.
                    # Every alternate link above must have a tied DP objective;
                    # never accept a beat merely because it is nearby in time.
                    assert ties > 0, f'{name}/{tightness}/{trim}: changed mask without a tied predecessor'
                    alternate_paths.append({'case': name, 'tightness': tightness, 'trim': trim,
                                            'reference': np.flatnonzero(wanted[0]).tolist(),
                                            'float32': np.flatnonzero(actual).tolist()})
                cases += 1
    return {'oracle': 'librosa==1.0.0', 'cases': cases, 'max_abs': largest,
            'exact_masks': exact_masks, 'numerically_tied_alternate_links': tied_links,
            'alternate_paths': alternate_paths}


def verify_reconstruction(actual, links, diagnostics, trim):
    path = np.zeros_like(actual, dtype=bool)
    tail = librosa.beat.__last_beat(diagnostics['cumulative'])
    librosa.beat.__dp_backtrack(links, tail, path)
    reconstructed = np.empty_like(path)
    librosa.beat.__trim_beats(diagnostics['local'], path, trim, reconstructed)
    np.testing.assert_array_equal(actual, reconstructed, err_msg='original backtrack/tail/trim on Float32 DP state')


def verify_links(actual, expected, cumulative, bpm, rate, tightness):
    """Allow alternate predecessors only when their DP scores tie
    within Float32 roundoff; reconstruction is checked separately.
    The oracle uses a mixed Float32/64 log expression, our DSP is Float32.
    """
    mismatches = np.flatnonzero(actual != expected)
    periods = np.broadcast_to(np.round(rate * 60 / bpm), cumulative.shape)
    for i in mismatches:
        a, b = int(actual[i]), int(expected[i])
        assert 0 <= a < i and 0 <= b < i, (i, a, b)
        assert round(periods[i] / 2) <= i - a <= 2 * periods[i]
        penalty = tightness * (np.log(np.array([i - a, i - b])) - np.log(periods[i])) ** 2
        scores = cumulative[[a, b]].astype(np.float64) - penalty
        tolerance = 8 * np.finfo(np.float32).eps * (np.max(np.abs(cumulative[[a, b]])) + np.max(penalty) + 1)
        assert abs(scores[0] - scores[1]) <= tolerance, (i, a, b, scores, tolerance)
    return len(mismatches)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('executable', type=Path)
    parser.add_argument('--output', type=Path, help='Save full evidence, including alternative tied paths')
    args = parser.parse_args()
    if librosa.__version__ != '1.0.0':
        raise RuntimeError(f'Expected librosa==1.0.0, got {librosa.__version__}')
    result = run(args.executable)
    result['numpy'] = np.__version__
    result['producer_sha256'] = hashlib.sha256(args.executable.read_bytes()).hexdigest()
    if args.output:
        args.output.write_text(json.dumps(result, indent=2), encoding='utf-8')
    print(json.dumps({k: v for k, v in result.items() if k != 'alternate_paths'}))


if __name__ == '__main__':
    main()
