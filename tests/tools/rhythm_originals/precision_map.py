"""Observe pinned source operation dtypes; no arithmetic replacement oracle."""
import argparse
import hashlib
import json
from pathlib import Path
import numpy as np
import scipy.fft

import original_sources


class Trace:
    __ua_domain__ = "numpy.scipy.fft"

    def __init__(self, output):
        self.output, self.records = output, []

    def array(self, name, values):
        values = np.ascontiguousarray(values)
        path = self.output / (name + ".bin")
        values.tofile(path)
        return {"path": str(path), "shape": list(values.shape), "dtype": values.dtype.str,
                "sha256": hashlib.sha256(path.read_bytes()).hexdigest()}

    def __ua_function__(self, method, args, kwargs):
        with scipy.fft.skip_backend(self):
            result = method(*args, **kwargs)
        index = len(self.records)
        self.records.append({"function": method.__name__,
                             "input": self.array(f"fft{index}.input", args[0]),
                             "output": self.array(f"fft{index}.output", result)})
        return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source-cache", type=Path, required=True)
    parser.add_argument("--evidence", type=Path, required=True)
    args = parser.parse_args()
    directory = args.evidence / "precision-map"
    directory.mkdir(exist_ok=True)
    lib = original_sources.librosa(args.source_cache)
    pcm = np.fromfile(args.evidence / "librosa/python.librosa.pcm_strength.a0.max1/input.f32", dtype="<f4")
    envelope = np.fromfile(args.evidence / "librosa/python.librosa.tempogram.method2.n128.center1/input.f32", dtype="<f4")
    trace = Trace(directory)
    with scipy.fft.set_backend(trace):
        stft = lib.stft(pcm, n_fft=256, hop_length=64, window="hann", center=True)
        acf = lib.feature.tempogram(onset_envelope=envelope, sr=100, hop_length=1, win_length=128)
        fourier = lib.feature.fourier_tempogram(onset_envelope=envelope, sr=100, hop_length=1, win_length=128)
        hybrid = lib.feature.hybrid_tempogram(onset_envelope=envelope, sr=100, hop_length=1, win_length=128)
    magnitude, power = np.abs(stft), np.abs(stft)**2
    weights = lib.filters.mel(sr=16000, n_fft=256, n_mels=128, fmin=0, fmax=8000)
    mel = lib.feature.melspectrogram(S=power, sr=16000, n_fft=256, n_mels=128)
    db = lib.power_to_db(mel)
    strength = lib.onset.onset_strength(S=db, sr=16000, hop_length=64, n_fft=256)
    arrays = {"window": lib.filters.get_window("hann", 256, fftbins=True), "stft": stft,
              "magnitude": magnitude, "power": power, "mel_weights": weights, "mel": mel,
              "db": db, "onset": strength, "acf": acf, "fourier_tempogram": fourier, "hybrid": hybrid}
    report = {"source": original_sources.source_record("librosa"), "fft_calls": trace.records,
              "stages": {name: trace.array(name, value) for name, value in arrays.items()}}
    (directory / "dtypes.json").write_text(json.dumps(report, indent=2) + "\n")
    print(json.dumps({name: str(value.dtype) for name, value in arrays.items()}))
    print(json.dumps([{key: value[key] if key == "function" else value[key]["dtype"]
                       for key in ("function", "input", "output")} for value in trace.records]))


if __name__ == "__main__":
    main()
