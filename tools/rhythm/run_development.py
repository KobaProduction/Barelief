"""Replay frozen development pairs through Barelief and compare recorded evidence.

The analyzer receives only audio. MIDI is read afterwards by the evaluator.
Fourier/ACF argmax is a deliberately untracked diagnostic baseline, not an
accepted beat tracker. Partial and full histories are reported separately.
"""
from __future__ import annotations

import argparse
import csv
import hashlib
import json
from pathlib import Path
import subprocess

import evaluate_replay as evaluator


def sha(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def periodicity(directory, report):
    results = {}
    for filename, method in [('tempograms.csv', 'fourier_argmax'), ('correlations.csv', 'acf_argmax')]:
        groups = {}
        with (directory / filename).open() as stream:
            for row in csv.DictReader(stream):
                time = float(row['t_obs'])
                if time < evaluator.WARMUP_SECONDS:
                    continue
                window, frame = int(row['window']), int(row['frame'])
                key = window, frame
                group = groups.setdefault(key, {'time': time, 'filled': int(row['filled']), 'peak': None})
                coordinate = float(row['hz' if method.startswith('fourier') else 'seconds'])
                bpm = 60 * coordinate if method.startswith('fourier') else 60 / coordinate if coordinate > 0 else 0
                score = float(row['magnitude' if method.startswith('fourier') else 'coefficient'])
                if 40 <= bpm <= 240 and (group['peak'] is None or score > group['peak'][0]):
                    group['peak'] = score, bpm
        for (window, _), group in groups.items():
            key = f'{method}_{window}'
            result = results.setdefault(key, {'eligible_snapshots': 0, 'answers': 0, 'strict_correct': 0,
                                               'full_history_snapshots': 0, 'partial_history_snapshots': 0})
            result['eligible_snapshots'] += 1
            result['full_history_snapshots' if group['filled'] == window else 'partial_history_snapshots'] += 1
            peak = group['peak']
            if peak is None or peak[0] <= 0:
                continue
            result['answers'] += 1
            tempo = max((entry for entry in report['tempo_metadata'] if entry['time'] <= group['time']),
                        key=lambda entry: entry['time'], default={'bpm': 120})['bpm']
            result['strict_correct'] += abs(peak[1] - tempo) <= .02 * tempo
    for result in results.values():
        result['coverage'] = result['answers'] / result['eligible_snapshots'] if result['eligible_snapshots'] else None
        result['strict_accuracy_vs_midi_clock'] = result['strict_correct'] / result['answers'] if result['answers'] else None
    return results


def refined_candidates(directory, report):
    path = directory / 'tempo_candidates.csv'
    if not path.exists():
        return None
    groups = {}
    with path.open() as file:
        for row in csv.DictReader(file):
            if float(row['t_obs']) < evaluator.WARMUP_SECONDS:
                continue
            groups.setdefault(int(row['frame']), []).append(row)
    answers = correct = contains = ambiguous = 0
    for rows in groups.values():
        if [int(row['rank']) for row in rows] != list(range(len(rows))) or len(rows) > 8:
            raise ValueError('Invalid candidate ranks')
        first = rows[0]
        if int(first['available']) == 0:
            if len(rows) != 1 or float(first['bpm']) != 0:
                raise ValueError('Malformed unavailable candidate snapshot')
            continue
        if any(int(row['available']) != 1 or not 0 <= float(row['score']) <= 1 or
               not 40 <= float(row['bpm']) <= 240 for row in rows):
            raise ValueError('Malformed candidate values')
        target = max((t for t in report['tempo_metadata'] if t['time'] <= float(first['t_obs'])),
                     key=lambda t: t['time'], default={'bpm': 120})['bpm']
        matches = [abs(float(row['bpm']) - target) <= .02 * target for row in rows]
        answers += 1
        correct += matches[0]
        contains += any(matches)
        ambiguous += int(first['harmonic_ambiguity']) == 1
    return {'eligible_snapshots': len(groups), 'answers': answers, 'rank1_correct': correct,
            'top8_contains_reference': contains, 'harmonic_ambiguity_snapshots': ambiguous,
            'coverage': answers / len(groups) if groups else None,
            'rank1_strict_accuracy': correct / answers if answers else None,
            'top8_reference_recall': contains / answers if answers else None,
            'confirmed_tempo': False}


def run(dataset: Path, executable: Path, output: Path):
    acquisition_path = dataset / 'acquisition.json'
    acquisition = json.loads(acquisition_path.read_text())
    if acquisition['holdout_payload_accessed'] or any(entry['role'] != 'development' for entry in acquisition['entries']):
        raise ValueError('This runner accepts development inputs only')
    executable = executable.resolve(strict=True)
    output.mkdir(parents=True, exist_ok=False)
    summary = {'schema': 'rhythm-development-comparison.v1', 'role': 'DEVELOPMENT_ONLY',
               'acquisition_sha256': sha(acquisition_path), 'binary_sha256': sha(executable),
               'runner_sha256': sha(__file__), 'analyzer_labels': False,
               'diagnostic_policy': 'argmax in 40..240 BPM, no interpolation/tracking; after fixed 8 s; not confirmed tempo',
               'cases': []}
    for entry in acquisition['entries']:
        metadata = entry['metadata']
        identity = metadata['id']
        if Path(identity).is_absolute() or '..' in Path(identity).parts:
            raise ValueError('Unsafe case identity')
        audio, midi = dataset / 'groove' / metadata['audio_filename'], dataset / 'groove' / metadata['midi_filename']
        for member in entry['members']:
            if sha(dataset / member['path']) != member['sha256']:
                raise ValueError('Acquired member changed')
        destination = output / identity.replace('/', '_')
        with (output / (identity.replace('/', '_') + '.log')).open('x') as log:
            subprocess.run([str(executable), '--input', str(audio.resolve()), '--out', str(destination.resolve())],
                           stdout=log, stderr=subprocess.STDOUT, check=True, timeout=300)
        report = evaluator.evaluate(destination, midi)
        with (destination / 'evaluation.json').open('x', encoding='utf8') as file:
            json.dump(report, file, indent=2, allow_nan=False)
        summary['cases'].append({'id': identity, 'style': metadata['style'], 'bpm_metadata': metadata['bpm'],
                                 'replay': str(destination.resolve()), 'runtime': report['runtime'],
                                 'onsets_float': report['event_metrics']['compact_float'],
                                 'periodicity': periodicity(destination, report),
                                 'refined_candidates': refined_candidates(destination, report),
                                 'evaluation_sha256': sha(destination / 'evaluation.json')})
        print('COMPLETE', identity, 'confirmed coverage', report['runtime']['coverage'], flush=True)
    with (output / 'comparison.json').open('x', encoding='utf8') as file:
        json.dump(summary, file, indent=2, allow_nan=False)
        file.write('\n')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--dataset', type=Path, required=True)
    parser.add_argument('--replay-exe', type=Path, required=True)
    parser.add_argument('--out', type=Path, required=True)
    arguments = parser.parse_args()
    run(arguments.dataset, arguments.replay_exe, arguments.out)
