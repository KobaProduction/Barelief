"""Optional native-library parity; requires librosa==1.0.0 and NumPy.

The executable is the production-kernel/graph test binary, in file-fixture mode.
No audio, corpus labels, fitted thresholds or independent formula oracle is used.
"""
import argparse
import itertools
from pathlib import Path
import subprocess
import tempfile

import librosa
import numpy as np


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("executable", type=Path)
    args = parser.parse_args()
    if librosa.__version__ != "1.0.0":
        raise RuntimeError("Parity is pinned to librosa==1.0.0")
    rng = np.random.default_rng(27831)
    cases, largest_error = 0, 0.0
    with tempfile.TemporaryDirectory(prefix="barelief-onset-parity-") as temporary:
        root = Path(temporary)
        for features, frames in [(1, 3), (8, 19), (128, 71), (513, 31), (2049, 27)]:
            spectrum = rng.uniform(-80, 9, (frames, features)).astype(np.float32)
            spectrum.tofile(root / "input.f32")
            for lag, width, center, detrend, median in itertools.product(
                [1, 2, 7], [1, 3, 4, 11], [False, True], [False, True], [False, True]
            ):
                expected = librosa.onset.onset_strength(
                    S=spectrum.T, lag=lag, max_size=width, center=center,
                    detrend=detrend, aggregate=np.median if median else np.mean,
                    n_fft=2049, hop_length=257,
                )
                subprocess.run([
                    str(args.executable.resolve()), str(root / "input.f32"),
                    str(root / "output.f32"), str(features), str(frames), str(lag),
                    str(width), "2049", "257", str(int(center)), str(int(detrend)),
                    str(int(median)),
                ], check=True, capture_output=True)
                actual = np.fromfile(root / "output.f32", dtype=np.float32)
                np.testing.assert_allclose(actual, expected, rtol=2e-6, atol=1e-5)
                largest_error = max(largest_error, float(np.max(np.abs(actual - expected))))
                cases += 1
    print(f"librosa={librosa.__version__} cases={cases} max_abs_error={largest_error:.9g} PASS")


if __name__ == "__main__":
    main()
