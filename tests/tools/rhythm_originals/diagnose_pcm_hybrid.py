"""Bounded remaining PCM hybrid counterexample on unchanged actual originals."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import numpy as np

import original_sources
from common import DTYPES


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", type=Path, required=True)
    parser.add_argument("--source-cache", type=Path, required=True)
    parser.add_argument("--evidence", type=Path, required=True)
    args = parser.parse_args()
    lib = original_sources.librosa(args.source_cache)
    case = args.evidence / "desktop-current114/python.librosa.composition.pcm_tempogram.method2.desktop_reference"
    output = args.evidence / "pcm-hybrid-residual"
    output.mkdir(exist_ok=True)
    prefix = output / "port"
    input_path = case / "input.f32"
    x = np.fromfile(input_path, dtype="<f4")
    subprocess.run([str(args.binary.resolve()), "librosa", "pcm_strength", str(input_path), str(prefix),
                    str(x.size), "1", "8000", "2048", "64", "0", "1", "0", "1"], check=True)
    manifest = json.loads(Path(str(prefix) + ".json").read_text())
    arrays = {item["name"]: np.fromfile(str(prefix) + "." + item["name"] + ".bin", dtype=DTYPES[item["dtype"]]).reshape(item["shape"])
              for item in manifest["outputs"]}
    z = lib.stft(x, n_fft=2048, hop_length=64).T
    magnitude = np.abs(z)
    mel = lib.feature.melspectrogram(S=(magnitude**2).T, sr=8000, n_fft=2048).T
    db = lib.power_to_db(mel.T).T
    onset = lib.onset.onset_strength(S=db.T, sr=8000, n_fft=2048, hop_length=64)
    kwargs = dict(sr=8000, hop_length=64, win_length=128)
    injected = lib.feature.hybrid_tempogram(onset_envelope=arrays["values"].ravel(), **kwargs).T
    actual = np.fromfile(case / "port.values.bin", dtype="<f4").reshape(injected.shape)
    expected = np.fromfile(case / "original.values.bin", dtype="<f8").reshape(injected.shape)
    stages = {"complex": np.stack((z.real, z.imag), axis=-1), "magnitude": magnitude,
              "powered": magnitude**2, "features": mel, "db": db, "values": onset.reshape(-1, 1)}
    report = {"source": original_sources.source_record("librosa"),
              "binary_sha256": hashlib.sha256(args.binary.read_bytes()).hexdigest(), "index": [194, 64], "stages": {}}
    for name, reference in stages.items():
        reference.tofile(output / ("original." + name + ".bin"))
        error = np.abs(arrays[name] - reference)
        report["stages"][name] = {"dtype": reference.dtype.str, "shape": list(reference.shape),
                                  "max_error": float(error.max()), "unequal_elements": int(np.count_nonzero(error)),
                                  "max_error_index": list(map(int, np.unravel_index(error.argmax(), error.shape)))}
    injected.tofile(output / "original_hybrid_from_port_onset.f64")
    cached_onset = np.fromfile(case / "original.onset.bin", dtype="<f4")
    report["fresh_onset_equals_cached_bytes"] = onset.tobytes() == cached_onset.tobytes()
    report["composition_onset_equals_direct_kernel_bytes"] = (case / "port.onset.bin").read_bytes() == arrays["values"].ravel().tobytes()
    report["port_vs_original_on_same_port_onset_max"] = float(np.abs(actual - injected).max())
    report["point"] = {"port": float(actual[194, 64]), "original": float(expected[194, 64]), "original_with_port_onset": float(injected[194, 64])}
    (output / "diagnostic.json").write_text(json.dumps(report, indent=2) + "\n")
    print(json.dumps(report, indent=2))


if __name__ == "__main__":
    main()
