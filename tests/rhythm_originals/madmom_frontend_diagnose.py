"""Read-only pinned frontend diagnosis on the immutable whole-input PCM."""
import argparse
import hashlib
import json
from pathlib import Path
import time

import numpy as np
import madmom_pipeline_oracle as oracle

ROWS = np.asarray([0, 1, 2, *range(92, 98), *range(192, 198)], dtype=np.int64)
PCM_SHA256 = "bee1949278115373423368e84f274b722ef7f3b2fe2d27c1d1145b0b3c767c5a"


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def array_difference(actual, expected):
    actual, expected = np.asarray(actual), np.asarray(expected)
    assert actual.shape == expected.shape
    indices = np.flatnonzero(actual != expected)
    flat_a, flat_e = actual.ravel(), expected.ravel()
    return {"shape": list(actual.shape), "different": int(indices.size),
            "max_abs": float(np.max(np.abs(actual.astype(float) - expected))) if actual.size else 0,
            "first": [{"index": int(i), "actual": float(flat_a[i]), "expected": float(flat_e[i])}
                      for i in indices[:24]]}


def compensated_rows(values):
    """Diagnostic replay of the existing Float32Accumulator, not a new kernel."""
    result = np.empty(values.shape[0], dtype=np.float32)
    for row_index, row in enumerate(values):
        total = error = np.float32(0)
        for value in row:
            corrected = np.float32(value - error)
            following = np.float32(total + corrected)
            error = np.float32(np.float32(following - total) - corrected)
            total = following
        result[row_index] = total
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("cache", type=Path)
    parser.add_argument("capture", type=Path)
    parser.add_argument("bound_original", type=Path)
    parser.add_argument("output", type=Path)
    args = parser.parse_args()
    args.output.mkdir(exist_ok=False)
    started = time.monotonic()
    pcm_path = args.capture / "pcm.f32"
    assert digest(pcm_path) == PCM_SHA256
    pin = oracle.verify_sources(args.cache)
    original = oracle.original_sources.madmom(args.cache)
    pcm = np.fromfile(pcm_path, dtype="<f4")
    assert pcm.shape == (514761,) and np.isfinite(pcm).all()
    count = 1 + (pcm.size - 2048) // 240
    magnitude, filtered, logarithmic, onset = oracle.front_end(original, pcm, count)
    stft = magnitude.stft
    positive = original.spectrogram.SpectrogramDifference(
        logarithmic, diff_frames=2, diff_max_bins=3, positive_diffs=True)
    positive = np.asarray(positive)
    source_sum = np.sum(positive, axis=1)
    assert np.array_equal(onset, source_sum)
    bound = np.load(args.bound_original, allow_pickle=False)
    assert np.array_equal(onset, bound), "Native original frontend changed against saved v44 execution"
    frames = np.asarray([stft.frames[int(i)] for i in ROWS])
    window = np.asarray(stft.fft_window)
    compensated = compensated_rows(positive)
    arrays = {"rows": ROWS, "pcm_frames": frames, "window": window,
              "windowed": np.multiply(frames, window), "stft": np.asarray(stft)[ROWS],
              "magnitude": np.asarray(magnitude)[ROWS], "filtered": np.asarray(filtered)[ROWS],
              "log": np.asarray(logarithmic)[ROWS], "positive": positive[ROWS],
              "positive_all": positive, "odf": np.asarray(onset), "compensated_odf": compensated}
    files = {}
    for name, value in arrays.items():
        path = args.output / (name + ".npy")
        with path.open("xb") as output:
            np.save(output, value, allow_pickle=False)
        files[name] = {"file": path.name, "dtype": value.dtype.str,
                       "shape": list(value.shape), "sha256": digest(path)}
    graph = np.fromfile(args.capture / "envelope.f32", dtype="<f4")
    report = {"pin": pin["revision"], "pcm_sha256": digest(pcm_path), "frames": count,
              "bound_original": str(args.bound_original.resolve()), "bound_original_sha256": digest(args.bound_original),
              "native_original_exact_saved_v44": True, "arrays": files,
              "graph_vs_original": array_difference(graph, onset),
              "compensated_original_bands_vs_original": array_difference(compensated, onset),
              "graph_vs_compensated_original_bands": array_difference(graph, compensated),
              "source_modules_sha256": {name: digest(Path(module.__file__)) for name, module in vars(original).items()},
              "script_sha256": digest(Path(__file__)), "elapsed_seconds": time.monotonic() - started,
              "scope": "Actual native original frontend only; no tempo/CRF invocation and no production mutation"}
    with (args.output / "result.json").open("x", encoding="utf8") as output:
        json.dump(report, output, indent=2)
    print(json.dumps({key: value for key, value in report.items() if key not in ("arrays", "source_modules_sha256")}), flush=True)


if __name__ == "__main__":
    main()
