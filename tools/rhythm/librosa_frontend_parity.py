"""Native librosa 1.0.0 parity for Mel, dB and their production graph chain.

The optional reference environment requires librosa==1.0.0 and NumPy. This tool
supplies explicit linear power S; it does not claim PCM framing/window parity.
"""
import argparse
import itertools
from pathlib import Path
import subprocess
import tempfile
import warnings

import librosa
import numpy as np


def invoke(executable, root, operation, values, parameters):
    values.astype(np.float32).tofile(root / "input.f32")
    result = subprocess.run([
        str(executable), operation, str(root / "input.f32"), str(root / "output.bin"),
        *map(str, parameters),
    ], capture_output=True, text=True)
    if result.returncode:
        raise RuntimeError(f"{operation} {parameters}, exit={result.returncode}: {result.stdout}{result.stderr}")
    return (root / "output.bin").read_bytes()


def mel_cases(executable, root, rng):
    norms = ["slaney", None, 0, 2.5, np.inf, -np.inf]
    count, largest = 0, 0.0
    for sr, n_fft, bands in [(22050, 512, 128), (22050, 2048, 128), (48000, 4096, 96), (32000, 2049, 40)]:
        power = rng.lognormal(0, 3, (7, 1 + n_fft // 2)).astype(np.float32)
        for htk, norm_id, limited in itertools.product([False, True], range(6), [False, True]):
            fmin, fmax = (60, 7000) if limited else (0, 0)
            with warnings.catch_warnings():
                warnings.simplefilter("ignore", UserWarning)
                basis = librosa.filters.mel(sr=sr, n_fft=n_fft, n_mels=bands,
                    fmin=fmin, fmax=fmax or None, htk=htk, norm=norms[norm_id])
            payload = invoke(executable, root, "mel", power,
                [sr, n_fft, bands, fmin, fmax, int(htk), norm_id, 2.5, len(power)])
            result = np.frombuffer(payload, dtype=np.float32)
            actual_basis = result[:basis.size].reshape(basis.shape)
            actual = result[basis.size:].reshape(len(power), bands)
            np.testing.assert_allclose(actual_basis, basis, rtol=3e-6, atol=1e-8)
            expected = power @ basis.T
            np.testing.assert_allclose(actual, expected, rtol=3e-6, atol=2e-5)
            largest = max(largest, float(np.max(np.abs(actual_basis - basis))))
            count += 1
    return count, largest


def db_cases(executable, root, rng):
    count, largest = 0, 0.0
    values = rng.lognormal(-4, 7, (17, 29)).astype(np.float32)
    values[0, :3] = [0, -1, 1e-30]
    for maximum, ref, amin, top, axis in itertools.product([False, True], [1., -3.7], [1e-10, 1e-20], [None, 0., 80.], range(3)):
        expected = librosa.power_to_db(values, ref=np.max if maximum else ref,
            amin=amin, top_db=top, axes=[None, 0, 1][axis])
        payload = invoke(executable, root, "db", values,
            [*values.shape, int(maximum), ref, amin, top or 0, int(top is not None), axis])
        actual = np.frombuffer(payload, dtype=np.float32).reshape(values.shape)
        np.testing.assert_allclose(actual, expected, rtol=3e-6, atol=3e-5)
        largest = max(largest, float(np.max(np.abs(actual - expected))))
        count += 1
    return count, largest


def graph_cases(executable, root, rng):
    count, largest = 0, 0.0
    for sr, n_fft, bands, frames in [(22050, 2048, 128, 19), (48000, 4096, 96, 27), (32000, 512, 40, 21), (22050, 2049, 128, 31)]:
        power = rng.lognormal(-1, 4, (frames, 1 + n_fft // 2)).astype(np.float32)
        mel = librosa.feature.melspectrogram(S=power.T, sr=sr, n_fft=n_fft, n_mels=bands)
        log_power = librosa.power_to_db(mel)
        onset = librosa.onset.onset_strength(S=log_power, n_fft=n_fft, hop_length=257)
        payload = invoke(executable, root, "graph", power, [sr, n_fft, bands, 0, 0, 0, 0, 2, frames])
        size = frames * bands * 4
        actual_mel = np.frombuffer(payload[:size], np.float32).reshape(frames, bands).T
        actual_db = np.frombuffer(payload[size:2*size], np.float32).reshape(frames, bands).T
        actual_onset = np.frombuffer(payload[2*size:], np.float32)
        np.testing.assert_allclose(actual_mel, mel, rtol=3e-6, atol=2e-5)
        np.testing.assert_allclose(actual_db, log_power, rtol=3e-6, atol=3e-5)
        np.testing.assert_allclose(actual_onset, onset, rtol=5e-6, atol=3e-5)
        largest = max(largest, float(np.max(np.abs(actual_onset - onset))))
        count += 1
    return count, largest


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("executable", type=Path)
    parser.add_argument("--group", action="append", choices=["Mel", "dB", "graph"])
    args = parser.parse_args()
    if librosa.__version__ != "1.0.0":
        raise RuntimeError("Reference environment must provide librosa==1.0.0")
    rng = np.random.default_rng(27832)
    with tempfile.TemporaryDirectory(prefix="barelief-native-frontend-") as temporary:
        root = Path(temporary)
        for name, cases in [("Mel", mel_cases), ("dB", db_cases), ("graph", graph_cases)]:
            if args.group and name not in args.group:
                continue
            count, largest = cases(args.executable.resolve(), root, rng)
            metric = "max_basis_abs_error" if name == "Mel" else "max_abs_error"
            print(f"librosa=1.0.0 {name} cases={count} {metric}={largest:.9g} PASS", flush=True)


if __name__ == "__main__":
    main()
