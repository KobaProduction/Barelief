"""Shared numerical comparison, input and I/O utilities for native pipeline tests."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess
import shutil
import time
import numpy as np

PINS = {
    "aubio": ("aubio/aubio", "ad5cf975aed08cc4562dd008cf9f83b12b82ffb8", "Ooura bundled, Float32"),
    "btrack": ("adamstark/BTrack", "9d6127618a5679e9caa74c594b88f1d74f0e035f", "KISS130 bundled Float32 FFT, double ODF; libsamplerate 0.2.2 c96f5e3d"),
    "essentia": ("MTG/essentia", "b9fa6cb674ca43dfb94d28d293aeda441c6745db", "KISS130 bundled, Float32"),
}
# Declared before executing any comparison. Spectral bounds are the existing
# independent FFT acceptance envelope. Descriptor arithmetic adds Float32
# reductions and state accumulation; integer events/status remain exact.
POLICY = {
    "spectral": {"rtol": 8e-5, "peak_atol": 4e-6},
    "descriptor": {"rtol": 3e-4, "peak_atol": 8e-6},
    "phase": {"circular_atol": .001, "minimum_relative_magnitude": .001},
    "integer": "exact dtype, shape and values; no floating point conversion",
    "nonfinite": "exact NaN/+Inf/-Inf classification at each element",
}

def save_json(path: Path, value: object) -> None:
    path.write_text(json.dumps(value, indent=2, allow_nan=False), encoding="utf8")

def signal(name: str, length: int, rate: int) -> np.ndarray:
    x = np.zeros(length, dtype="<f4")
    if name == "constant":
        x.fill(.25)
    elif name == "nyquist":
        x[::2] = .25
        x[1::2] = -.25
    elif name.startswith("impulse"):
        x[{"impulse_first": 0, "impulse_middle": length//2, "impulse_last": length-1}[name]] = .75
    elif name == "noise":
        x[:] = np.random.Generator(np.random.PCG64(0x52485954)).uniform(-.2, .2, length)
    elif name in {"tones8", "induction_tones8", "induction_transition"}:
        t = np.arange(length, dtype=np.float64)/rate
        for i, frequency in enumerate((97, 173, 293, 419, 701, 997, 1901, 3109)):
            x += (.045*np.sin(2*np.pi*(frequency*t+(i+1)*.7*t*t))).astype("<f4")
        x[::max(1, rate//2)] += .3
        if name=="induction_transition":
            x[:rate]=0
            x[5*rate:6*rate]=0
            x[6*rate:]=0
            for at in np.arange(6*rate,length,rate*2/3).astype(np.int64):
                stop=min(length,at+rate//25)
                time=np.arange(stop-at)/rate
                x[at:stop]=sum(.055*np.sin(2*np.pi*f*time) for f in (97,701,997,1901,3109)).astype("<f4")
    elif name not in {"silence","induction_silence"}:
        raise ValueError(name)
    return x





def load(prefix: Path, field: str, shape: list[int] | None = None):
    found = [prefix.with_name(prefix.name+"."+field+"."+kind) for kind in ("f32","f64","u64")]
    found = [path for path in found if path.exists()]
    if len(found) != 1:
        raise ValueError(f"expected one output for {prefix.name}.{field}: {found}")
    path = found[0]
    dtype = {".f32":"<f4", ".f64":"<f8", ".u64":"<u8"}[path.suffix]
    value = np.fromfile(path, dtype=dtype)
    if shape is not None:
        value = value.reshape(shape)
    return value, {"path":str(path),"dtype":{"<f4":"float32-le","<f8":"float64-le","<u8":"uint64-le"}[dtype],"shape":list(value.shape),"order":"C","sha256":hashlib.sha256(path.read_bytes()).hexdigest()}

def compare(field: str, actual: np.ndarray, expected: np.ndarray, norm: np.ndarray | None):
    if actual.shape != expected.shape:
        return {"pass":False,"reason":"shape", "actual":list(actual.shape),"expected":list(expected.shape)}
    if actual.dtype.kind == "u" or expected.dtype.kind == "u":
        bad = actual != expected
        return {"pass":bool(not np.any(bad)),"mismatches":int(np.count_nonzero(bad)),"policy":"integer"}
    classes = lambda a: np.where(np.isnan(a),1,np.where(np.isposinf(a),2,np.where(np.isneginf(a),3,0)))
    nonfinite_bad = classes(actual) != classes(expected)
    finite = np.isfinite(actual) & np.isfinite(expected)
    error = np.zeros(actual.shape, dtype=np.float64)
    error[finite] = np.abs(actual[finite].astype(np.float64)-expected[finite])
    if field == "phase":
        assert norm is not None
        finite &= norm > np.max(norm, axis=-1, keepdims=True)*POLICY["phase"]["minimum_relative_magnitude"]
        error[finite] = np.abs((actual[finite]-expected[finite]+np.pi)%(2*np.pi)-np.pi)
        bad = finite & (error > POLICY["phase"]["circular_atol"])
        policy = "phase"
    else:
        policy = "spectral" if field in {"norm","window","complex","transform"} else "descriptor"
        p = POLICY[policy]
        # Per-method scale: HFC cannot hide error in another ODF column.
        axes = (-1,-2) if field=="complex" else -1 if policy=="spectral" else 0
        peak = np.max(np.where(np.isfinite(expected),np.abs(expected),0),axis=axes,keepdims=True,initial=0)
        allowed = p["rtol"]*np.abs(expected)+p["peak_atol"]*peak
        bad = finite & (error > allowed)
    bad |= nonfinite_bad
    indices = np.argwhere(bad)
    return {"pass":bool(not len(indices)),"policy":policy,"mismatches":len(indices),
            "nonfinite_class_mismatches":int(np.count_nonzero(nonfinite_bad)),
            "max_abs_error":float(np.max(error[finite],initial=0)),
            "first_index":indices[0].tolist() if len(indices) else None,
            "compared_finite_values":int(np.count_nonzero(finite))}

def execute(binary: str, args: list[str], env: dict[str,str]):
    before = time.perf_counter()
    result = subprocess.run([binary,*args], capture_output=True, text=True, timeout=180, env=env)
    return {"returncode":result.returncode,"seconds":time.perf_counter()-before,
            "stdout":result.stdout,"stderr":result.stderr}
