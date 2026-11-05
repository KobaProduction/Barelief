"""Compare the shipped 44100 Hz causal graph with pinned librosa 1.0.0.

The PCM reference is the recorded production SRC output. This isolates native
STFT/Mel/dB/onset correctness from the different SRC filters used by libraries.
The dB/onset reference uses each available prefix (up to History capacity), not
future peak information. Descriptor math is checked separately on the recorded
complex FFT so silent-bin phase noise from different FFT backends is not hidden.
"""
import argparse
import csv
import hashlib
import json
from pathlib import Path
import subprocess
import tempfile
import wave

import librosa
import numpy as np


def make_audio(path, rate, silent=False, seconds=.5):
    time = np.arange(int(rate * seconds) + 17, dtype=np.float64) / rate
    envelope = np.where((time * 7) % 1 < .3, 1., .03)
    signal = np.zeros_like(time)
    for tone in range(1, 7):
        phase = 2 * np.pi * (tone * 173 * time + 19 * time ** 2)
        phase += np.where(time > .23, .7 * tone, 0.)
        signal += .035 * envelope * np.sin(phase)
    if seconds > 1:
        signal *= np.where(time < .4, 1., .01)
    pcm = np.zeros(len(time), dtype='<i2') if silent else np.rint(signal * 32767).astype('<i2')
    with wave.open(str(path), 'wb') as audio:
        audio.setparams((1, 2, rate, len(pcm), 'NONE', 'not compressed'))
        audio.writeframes(pcm.tobytes())


def replay(executable, root, rate, packet, cadence, silent=False, reuse=False, seconds=.5):
    root.mkdir(exist_ok=reuse)
    audio = root / 'input.wav'
    output = root / 'replay'
    if reuse and (output / 'summary.json').exists():
        summary = json.loads((output / 'summary.json').read_text(encoding='utf-8'))
        assert summary['input_sha256'] == hashlib.sha256(audio.read_bytes()).hexdigest()
        assert (summary['source_rate'], summary['packet_samples'], summary['analysis_rate']) == (rate, packet, cadence)
        return output
    make_audio(audio, rate, silent, seconds)
    result = subprocess.run([str(executable), '--input', str(audio), '--out', str(output),
                             '--packet-samples', str(packet), '--analysis-rate', str(cadence),
                             '--reference-trace'], capture_output=True, text=True)
    if result.returncode:
        raise RuntimeError(f'replay exit={result.returncode}: {result.stdout}{result.stderr}')
    (output / 'producer.json').write_text(json.dumps({
        'executable_sha256': hashlib.sha256(executable.read_bytes()).hexdigest()
    }), encoding='utf-8')
    return output


def load(output):
    summary = json.loads((output / 'summary.json').read_text(encoding='utf-8'))['reference']
    assert (summary['sample_rate'], summary['fft_size'], summary['hop_samples']) == (44100, 2048, 512)
    assert summary['center'] is False and summary['periodic_window'] is True
    stages = {}
    for stage in summary['trace']:
        values = np.fromfile(output / stage['file'], dtype='<f4')
        assert len(values) == stage['frames'] * stage['width'], stage
        stages[stage['file']] = values.reshape(stage['frames'], stage['width'])
    with (output / 'reference_envelopes.csv').open(encoding='utf-8', newline='') as file:
        rows = list(csv.DictReader(file))
    assert len(rows) == 6 * summary['frames']
    methods = {entry['method']: [] for entry in summary['methods']}
    for row in rows:
        methods[row['method']].append(row)
    return summary, stages, methods


def compare(actual, expected, rtol, atol):
    assert actual.shape == expected.shape, (actual.shape, expected.shape)
    np.testing.assert_allclose(actual, expected, rtol=rtol, atol=atol)
    return float(np.max(np.abs(actual - expected))) if actual.size else 0.


def descriptor_math(spectrum):
    z = spectrum.astype(np.complex128)
    magnitude = np.abs(z)
    phase = np.where(magnitude > 0, np.angle(z), 0.)
    previous = np.vstack([np.zeros_like(magnitude[:1]), magnitude[:-1]])
    p1 = np.vstack([np.zeros_like(phase[:1]), phase[:-1]])
    p2 = np.vstack([np.zeros_like(phase[:2]), phase[:-2]])
    predicted = 2 * p1 - p2
    phase_error = np.where(magnitude > 0, np.abs(np.angle(np.exp(1j * (phase - predicted)))), 0.)
    return {
        'spectral_flux': np.maximum(magnitude - previous, 0).sum(axis=1),
        'complex_difference': np.abs(z - previous * np.exp(1j * predicted)).sum(axis=1),
        'phase_deviation': phase_error.mean(axis=1),
        'high_frequency_content': (magnitude * np.arange(1, magnitude.shape[1] + 1)).sum(axis=1),
    }


def verify_times(summary, methods):
    for rows in methods.values():
        assert len(rows) == summary['frames']
        for i, row in enumerate(rows):
            begin = summary['pcm_begin'] + i * 512
            assert int(row['frame']) == i + 1 and int(row['window_begin']) == begin
            assert int(row['sample_rate']) == 44100 and int(row['window_samples']) == 2048
            for field, offset in [('t_start', 0), ('t_center', 1023.5), ('t_end', 2047)]:
                assert abs(float(row[field]) - (begin + offset) / 44100) < 1e-12
            assert float(row['t_ready']) >= float(row['t_end'])
            assert float(row['upstream_latency_s']) >= 0


def verify(output):
    summary, stages, methods = load(output)
    pcm = stages['reference_pcm.f32'].ravel()
    assert len(pcm) == summary['pcm_samples']
    reference = librosa.stft(pcm, n_fft=2048, hop_length=512, win_length=2048,
                             window='hann', center=False).T
    fft = stages['reference_fft.f32'].reshape(-1, 1025, 2)
    spectrum = fft[:, :, 0] + 1j * fft[:, :, 1]
    errors = {'fft': compare(spectrum, reference, 2e-5, 4e-5)}
    power = np.abs(reference) ** 2
    errors['power'] = compare(stages['reference_power.f32'], power, 3e-5, 2e-4)
    mel = librosa.feature.melspectrogram(S=power.T, sr=44100, n_fft=2048, n_mels=128)
    errors['mel'] = compare(stages['reference_mel.f32'].T, mel, 2e-5, 4e-5)
    log_latest, envelope, max_filtered = [], [], []
    for frame in range(mel.shape[1]):
        history = mel[:, max(0, frame + 1 - 512):frame + 1]
        db = librosa.power_to_db(history)
        log_latest.append(db[:, -1])
        envelope.append(librosa.onset.onset_strength(S=db, n_fft=2048, hop_length=512, center=False)[-1])
        max_filtered.append(librosa.onset.onset_strength(S=db, n_fft=2048, hop_length=512,
                                                        center=False, lag=2, max_size=3)[-1])
    errors['db'] = compare(stages['reference_db.f32'], np.asarray(log_latest), 2e-5, 3e-3)
    actual = np.array([float(row['value']) for row in methods['mel_flux']])
    errors['mel_flux'] = compare(actual, np.array(envelope), 2e-5, 3e-3)
    actual = np.array([float(row['value']) for row in methods['max_filter_flux']])
    errors['max_filter_flux'] = compare(actual, np.array(max_filtered), 2e-5, 3e-3)
    for name, expected in descriptor_math(spectrum).items():
        actual = np.array([float(row['value']) for row in methods[name]])
        errors[name] = compare(actual, expected, 2e-5, 2e-5)
    verify_times(summary, methods)
    producer = output / 'producer.json'
    return {'frames': summary['frames'], 'pcm_samples': len(pcm), 'max_abs': errors,
            'producer': json.loads(producer.read_text(encoding='utf-8')) if producer.exists() else None}


def run(executable, root, reuse=False):
    results = []
    for rate, packet, cadence, silent, seconds in [(44100, 137, 60, False, .5), (48000, 256, 45, False, .5),
                                                 (32000, 1024, 30, False, .5), (48000, 137, 60, True, .5),
                                                 (44100, 1024, 30, False, 6.5)]:
        output = replay(executable, root / f'{rate}-{packet}-{cadence}-{int(silent)}', rate, packet, cadence, silent, reuse, seconds)
        result = verify(output)
        results.append({'source_rate': rate, 'packet': packet, 'baseline_cadence': cadence, 'silent': silent, 'seconds': seconds, **result})
    return {'oracle': 'librosa==1.0.0', 'numpy': np.__version__, 'reference_input': 'recorded production SRC PCM',
            'centering': 'causal center=False; prefix dB range; no future samples', 'cases': results}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('executable', type=Path)
    parser.add_argument('--output', type=Path)
    parser.add_argument('--reuse', action='store_true', help='Recheck completed traces without replaying; input hashes must match')
    args = parser.parse_args()
    if librosa.__version__ != '1.0.0':
        raise RuntimeError(f'Expected librosa==1.0.0, got {librosa.__version__}')
    if args.output:
        args.output.mkdir(parents=True, exist_ok=args.reuse)
        result = run(args.executable.resolve(), args.output, args.reuse)
        (args.output / 'evidence.json').write_text(json.dumps(result, indent=2), encoding='utf-8')
    else:
        with tempfile.TemporaryDirectory() as directory:
            result = run(args.executable.resolve(), Path(directory))
    print(json.dumps(result, indent=2))


if __name__ == '__main__':
    main()
