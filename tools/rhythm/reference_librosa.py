"""Optional librosa reference on the recorded production activity, not another frontend.

Full-track beat/PLP outputs use future context. Prefix tempo fits receive only
available samples; they are computational references, not realtime benchmarks.
MIDI is inspected only after prediction. No human beat-label claim is made.
"""
from __future__ import annotations

import argparse
import csv
import hashlib
import importlib.metadata
import json
from pathlib import Path
import time

import librosa
import numpy as np


def sha(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def read_activity(directory):
    summary = json.loads((directory / 'summary.json').read_text())
    if sha(directory / 'graph.json') != summary['graph_sha256']:
        raise ValueError('Replay graph hash changed')
    times, values = [], []
    with (directory / 'runtime.csv').open() as file:
        for row in csv.DictReader(file):
            if int(row['port']) != 7:
                continue
            if int(row['available']) != 1:
                raise ValueError('Activity has unavailable frames')
            times.append(float(row['t_obs']))
            values.append(float(row['value']))
    times, values = np.asarray(times), np.asarray(values)
    rate = summary['analysis_rate']
    if len(values) != summary['analysis_frames'] or not len(values):
        raise ValueError('Activity frame count mismatch')
    if not np.all(np.isfinite(values)) or np.any(values < 0) or np.any(values > 1):
        raise ValueError('Activity must be finite and normalized 0..1')
    if np.any(np.abs(np.diff(times) - 1 / rate) > 2 / 32768):
        raise ValueError('Activity does not have the declared regular cadence')
    return summary, times, values


def predict(times, values, rate):
    start = time.perf_counter()
    tempo, frames = librosa.beat.beat_track(onset_envelope=values, sr=rate, hop_length=1,
                                          start_bpm=120, tightness=100, trim=True, units='frames')
    full = {'tempo': float(np.asarray(tempo).flat[0]), 'beats_t_obs': times[frames].tolist(),
            'context': 'entire clip, future data available', 'wall_seconds_including_first_JIT': time.perf_counter() - start}
    prefix = []
    for deadline in range(8, int(times[-1]) + 1):
        count = int(np.searchsorted(times, deadline, side='right'))
        start = time.perf_counter()
        tempo = librosa.feature.tempo(onset_envelope=values[:count], sr=rate, hop_length=1,
                                     start_bpm=120, ac_size=8, max_tempo=240)[0]
        prefix.append({'available_at': deadline, 'last_input_t_obs': float(times[count - 1]),
                       'frames': count, 'bpm': float(tempo), 'wall_seconds': time.perf_counter() - start})
    pulses = {}
    for size in (512, 4096):
        curve = librosa.beat.plp(onset_envelope=values, sr=rate, hop_length=1,
                                win_length=size, tempo_min=40, tempo_max=240)
        maxima = np.flatnonzero(librosa.util.localmax(curve) & (curve > 0))
        pulses[str(size)] = {'context': 'entire clip, centered windows and future data available',
                             'peaks_t_obs': times[maxima].tolist(), 'minimum': float(curve.min()),
                             'maximum': float(curve.max()), 'curve': curve.tolist()}
    return {'full_track_DP': full, 'prefix_tempo': prefix, 'full_track_PLP': pulses}


def score_clock(prediction, labels):
    result = []
    for row in prediction['prefix_tempo']:
        target = max((t for t in labels['tempo_metadata'] if t['time'] <= row['available_at']),
                     key=lambda t: t['time'], default={'bpm': 120})['bpm']
        result.append(abs(row['bpm'] - target) <= .02 * target)
    return {'prefix_answers': len(result), 'prefix_strict_correct': sum(result),
            'prefix_strict_accuracy_vs_midi_clock': sum(result) / len(result) if result else None,
            'beat_downbeat_accuracy_vs_human': None}


def run(comparisons, output):
    cases = []
    for path in comparisons:
        comparison = json.loads(path.read_text())
        if comparison['role'] != 'DEVELOPMENT_ONLY':
            raise ValueError('Only explicit development comparisons are accepted')
        cases.extend(comparison['cases'])
    output.mkdir(parents=True, exist_ok=False)
    result = {'schema': 'rhythm-librosa-reference.v1', 'role': 'DEVELOPMENT_ONLY',
              'activity_source': 'production RhythmAnalyzer port7; corrected normalized spectral pipeline',
              'defaults': {'start_bpm': 120, 'tightness': 100, 'hop_length': 1, 'prefix_ac_size_s': 8},
              'versions': {name: importlib.metadata.version(name) for name in ('librosa', 'numpy', 'scipy', 'numba')},
              'script_sha256': sha(__file__), 'cases': []}
    for case in cases:
        directory = Path(case['replay'])
        summary, times, values = read_activity(directory)
        prediction = predict(times, values, summary['analysis_rate'])
        # Labels are loaded after all predictions; never supplied as bpm/prior.
        labels = json.loads((directory / 'evaluation.json').read_text())
        entry = {'id': case['id'], 'source_runtime_sha256': sha(directory / 'runtime.csv'),
                 'source_graph_sha256': summary['graph_sha256'], 'labels_sha256': sha(directory / 'evaluation.json'),
                 'predictions': prediction, 'scores': score_clock(prediction, labels)}
        result['cases'].append(entry)
        with (output / (case['id'].replace('/', '_') + '.json')).open('x', encoding='utf8') as file:
            json.dump(entry, file, indent=2, allow_nan=False)
        print('REFERENCE', case['id'], entry['scores'], flush=True)
    with (output / 'reference.json').open('x', encoding='utf8') as file:
        json.dump(result, file, indent=2, allow_nan=False)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--comparison', type=Path, action='append', required=True)
    parser.add_argument('--out', type=Path, required=True)
    arguments = parser.parse_args()
    run(arguments.comparison, arguments.out)
