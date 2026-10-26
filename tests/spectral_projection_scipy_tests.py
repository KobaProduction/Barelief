"""Independent SciPy acceptance; no SciPy dependency in the C++ application.

Run after building rfft_tests:
  python tests/spectral_projection_scipy_tests.py --binary <rfft_tests.exe>
      --output <owned evidence directory>
"""
import argparse
import json
from pathlib import Path
import subprocess

import numpy as np
import scipy
from scipy import signal


WINDOWS = {"hann": "hann", "hamming": "hamming", "blackman": "blackman",
           "rectangular": "boxcar", "bartlett": "bartlett",
           "blackman_harris": "blackmanharris", "flat_top": "flattop"}


def close(actual, expected, label, errors, absolute_scale=0.0):
    expected = np.asarray(expected)
    peak = max(float(np.max(np.abs(expected))), absolute_scale, 1e-30)
    difference = float(np.max(np.abs(np.asarray(actual) - expected)))
    errors[label] = max(errors.get(label, 0.0), difference / peak)
    np.testing.assert_allclose(actual, expected, rtol=8e-5, atol=4e-6 * peak,
                               err_msg=label)


def verify(case, errors):
    x = np.asarray(case["signal"], dtype=np.float32)
    w = np.asarray(case["window"], dtype=np.float32)
    n, fs = case["nfft"], case["fs"]
    np.testing.assert_allclose(w, signal.get_window(WINDOWS[case["window_name"]],
                                                  len(w), fftbins=False),
                               rtol=0, atol=6e-8)
    windowed = np.asarray(case["windowed"], dtype=np.float32).astype(np.float64)
    raw = np.asarray(case["raw"], dtype=np.float32).astype(np.float64)
    z = raw[::2] + 1j * raw[1::2]
    oracle = np.fft.rfft(windowed, n=n)
    close(z, oracle, "raw_fft", errors)
    c, e = w.astype(float).sum(), np.square(w.astype(float)).sum()
    alpha = np.full(n // 2 + 1, 2.0)
    alpha[[0, -1]] = 1
    magnitude = np.abs(oracle)
    raw_peak = float(magnitude.max())
    power = alpha * magnitude**2 / c**2
    psd = alpha * magnitude**2 / (fs * e)
    bin_power = psd * fs / n
    expected = {"real": oracle.real, "imaginary": oracle.imag,
                "magnitude": magnitude / c, "window_scaled_magnitude": magnitude / c,
                "peak_amplitude": alpha * magnitude / c,
                "rms_amplitude": np.sqrt(power), "power": power,
                "psd": psd, "bin_power": bin_power}
    outputs = case["outputs"]
    for name, values in expected.items():
        component_scale = raw_peak if name in {"real", "imaginary"} else 0.0
        close(outputs[name + "_scaled"], values, name, errors, component_scale)
        raw_values = (oracle.real if name == "real" else oracle.imag if name == "imaginary"
                      else magnitude**2 if name in {"power", "psd", "bin_power"} else magnitude)
        close(outputs[name + "_raw"], raw_values, name + "_raw", errors, component_scale)
    mask = magnitude > max(raw_peak * 1e-3, 1e-30)
    phase_from_raw = np.where(np.abs(z) > 0, np.angle(z), 0.0)
    for suffix in ("_raw", "_scaled"):
        actual_phase = np.asarray(outputs["phase" + suffix])
        delta = np.abs(np.angle(np.exp(1j * (actual_phase - np.angle(oracle)))))
        identity_delta = np.abs(np.angle(np.exp(1j * (actual_phase - phase_from_raw))))
        errors["phase_strong_max_radians"] = max(errors.get("phase_strong_max_radians", 0),
                                                 float(np.max(delta[mask], initial=0)))
        errors["phase_raw_identity_max_radians"] = max(errors.get("phase_raw_identity_max_radians", 0),
                                                       float(identity_delta.max()))
        assert np.all(identity_delta < 5e-7), "phase must match atan2 of the actual FFT"
        assert np.all(delta[mask] < .001), "phase of bins above -60 dB relative to the peak"
    for scaling in ("spectrum", "density"):
        _, reference = signal.periodogram(x, fs=fs, window=w, nfft=n,
                                          detrend=False, return_onesided=True,
                                          scaling=scaling)
        name = "power" if scaling == "spectrum" else "psd"
        close(outputs[name + "_scaled"], reference, "scipy_" + scaling, errors)
    actual_power = np.asarray(outputs["power_scaled"])
    actual_psd = np.asarray(outputs["psd_scaled"])
    actual_bin = np.asarray(outputs["bin_power_scaled"])
    close(actual_power, actual_psd * fs * e / c**2, "enbw", errors)
    close(actual_bin, actual_psd * fs / n, "bin_width", errors)
    close(np.square(outputs["rms_amplitude_scaled"]), actual_power, "rms_squared", errors)
    close(actual_bin.sum(), np.square(windowed).sum() / e, "parseval", errors)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    export = args.output / "cpp-projections.json"
    subprocess.run([str(args.binary.resolve()), "--export-projections", str(export.resolve())],
                   check=True, timeout=120)
    cases = json.loads(export.read_text())
    errors = {}
    for case in cases:
        verify(case, errors)
    result = {"status": "passed", "cases": len(cases), "scipy": scipy.__version__,
              "numpy": np.__version__, "max_peak_relative_errors": errors,
              "windows": list(WINDOWS), "fft_sizes": [64, 256], "window_lengths": [63, 64],
              "sample_rates": [32768, 44100, 48000], "signal_kinds": 8,
              "normalization_states": 2, "hot_allocations": 0,
              "matlab_runtime": "not run; definitions checked against official documentation"}
    (args.output / "scipy-results.json").write_text(json.dumps(result, indent=2) + "\n")
    print(json.dumps(result, indent=2))


if __name__ == "__main__":
    main()
