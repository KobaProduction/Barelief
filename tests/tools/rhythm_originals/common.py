"""Typed same-input cases; references execute original libraries, never port formulas."""
from dataclasses import dataclass, field
import hashlib
import json
from pathlib import Path
import subprocess
import time

import numpy as np

SEED = 0x52485954
# Fixed before observing comparisons. This is the existing spectral acceptance
# budget; gamma_512 for sequential Float32 arithmetic is below the relative term.
RTOL, PEAK_ATOL = 8e-5, 4e-6
DTYPES = {"float32-le": "<f4", "float64-le": "<f8", "uint64-le": "<u8",
          "uint32-le": "<u4", "int32-le": "<i4", "uint8": "u1"}


@dataclass
class Case:
    name: str
    family: str
    method: str
    values: np.ndarray
    arguments: list
    reference: object
    settings: dict = field(default_factory=dict)
    sidecars: dict = field(default_factory=dict)
    policy: str = "unchanged original implementation"
    evidence_category: str = "literal-source"


def tones(count, sample_rate):
    time = np.arange(count, dtype=np.float64) / sample_rate
    value = np.zeros(count)
    for index in range(8):
        frequency = 23 + 31 * index
        value += .035 * (1 + .2 * np.sin((index + 1) * time)) * np.sin(
            2 * np.pi * (frequency * time + .2 * (index + 1) * time**2) + .13 * index)
    return value.astype(np.float32)


def envelope(count=256):
    index = np.arange(count)
    values = .01 + .002 * np.sin(index * .21)
    for frame in range(12, count, 25):
        values += .85 * np.exp(-.5 * ((index - frame) / 1.5)**2)
    return values.astype(np.float32)


def input_record(path, values):
    values = np.asarray(values, dtype="<f4", order="C")
    values.tofile(path)
    return {"path": str(path), "dtype": "float32-le", "shape": list(values.shape),
            "order": "C", "sha256": hashlib.sha256(path.read_bytes()).hexdigest()}


def compare(name, actual, reference, expected):
    actual, reference = np.asarray(actual), np.asarray(reference)
    if actual.shape != reference.shape:
        raise AssertionError(f"{name}: shape {actual.shape} != {reference.shape}")
    if reference.dtype.kind in "biu" or actual.dtype.kind in "biu":
        np.testing.assert_array_equal(actual, reference, err_msg=name)
        return {"comparison": "exact", "elements": int(actual.size)}
    np.testing.assert_array_equal(np.isnan(actual), np.isnan(reference), err_msg=name + " NaN")
    np.testing.assert_array_equal(np.isposinf(actual), np.isposinf(reference), err_msg=name + " +Inf")
    np.testing.assert_array_equal(np.isneginf(actual), np.isneginf(reference), err_msg=name + " -Inf")
    finite = np.isfinite(reference)
    if name == "phase" and "magnitude" in expected:
        magnitude = np.asarray(expected["magnitude"])
        finite &= magnitude > max(float(magnitude.max(initial=0)) * 1e-3, 1e-30)
        error = np.abs(np.angle(np.exp(1j * (actual[finite] - reference[finite]))))
        assert np.all(error < .001), "phase above -60 dB relative to peak"
        return {"comparison": "circular_above_minus60dB", "elements": int(error.size),
                "max_absolute_error": float(error.max(initial=0))}
    peak = max(float(np.abs(reference[finite]).max(initial=0)), 1e-30)
    if name == "window":
        np.testing.assert_allclose(actual[finite], reference[finite], rtol=0, atol=6e-8, err_msg=name)
    else:
        np.testing.assert_allclose(actual[finite], reference[finite], rtol=RTOL,
                                   atol=PEAK_ATOL * peak, err_msg=name)
    error = float(np.abs(actual[finite] - reference[finite]).max(initial=0))
    return {"comparison": "float32_budget", "elements": int(actual.size),
            "max_absolute_error": error, "max_peak_relative_error": error / peak}


def cached_original(case, values, source, directory, integrity):
    assert integrity is not None, "sealed original cache manifest required"
    cached_files = [directory / "case.json", directory / "input.f32", directory / "original.json"]
    cached_files += list(directory.glob("original.*.bin"))
    cached_files += [Path(str(directory / "input.f32") + suffix) for suffix in case.sidecars]
    for path in cached_files:
        assert hashlib.sha256(path.read_bytes()).hexdigest() == integrity[str(path.resolve())], str(path)
    metadata = json.loads((directory / "case.json").read_text())
    assert metadata["source"] == source, "cached original source pin/policy"
    assert (directory / "input.f32").read_bytes() == values.tobytes(order="C"), "cached original input bytes"
    assert metadata["inputs"]["values"]["sha256"] == hashlib.sha256((directory / "input.f32").read_bytes()).hexdigest(), "recorded cached input hash"
    assert metadata["reference_policy"] == case.policy, "cached original reference policy"
    assert (metadata["arguments"] == case.arguments or
            (case.settings.get("precision") == "DesktopReference" and metadata["arguments"] == case.arguments[:-1])), "only appended precision setting may differ"
    assert metadata["settings"] == {key: value for key, value in case.settings.items()
                                    if key not in ("precision", "public_dtype")}, "cached original settings"
    for suffix, data in case.sidecars.items():
        assert Path(str(directory / "input.f32") + suffix).read_bytes() == np.asarray(data, dtype="<f4").tobytes(order="C")
    manifest = json.loads((directory / "original.json").read_text())
    if manifest.get("status") == "unsupported_original_domain" and case.settings.get("original_unsupported"):
        raise ValueError(manifest["error"])
    outputs = manifest["outputs"]
    return {name: np.fromfile(item["path"], dtype=item["dtype"]).reshape(item["shape"])
            for name, item in outputs.items()}


def run_case(case, binary, directory, source, reference_cache=None, cache_integrity=None, fresh_on_cache_miss=False):
    directory.mkdir(parents=True, exist_ok=True)
    values = np.asarray(case.values, dtype=np.float32)
    shape = values.shape if values.ndim == 2 else (values.size, 1)
    path, prefix = directory / "input.f32", directory / "port"
    inputs = {"values": input_record(path, values)}
    for suffix, data in case.sidecars.items():
        inputs[suffix] = input_record(Path(str(path) + suffix), data)
    metadata = {"format": "barelief-original-case-v1", "case_id": case.name,
                "source": source, "reference_policy": case.policy, "settings": case.settings,
                "evidence_category": case.evidence_category,
                "arguments": case.arguments, "inputs": inputs, "seed": SEED,
                "tolerance": {"relative": RTOL, "peak_absolute": PEAK_ATOL,
                              "phase_strong_radians": .001, "window_absolute": 6e-8}}
    if reference_cache is not None:
        metadata["reference_cache"] = str(reference_cache)
    (directory / "case.json").write_text(json.dumps(metadata, indent=2) + "\n")
    try:
        if reference_cache is None:
            expected = case.reference(values)
        else:
            try:
                expected = cached_original(case, values, source, reference_cache, cache_integrity)
            except AssertionError as error:
                if not fresh_on_cache_miss:
                    raise
                metadata["cache_rejected"] = str(error)
                metadata["reference_execution"] = "fresh unchanged original after strict cache mismatch"
                (directory / "case.json").write_text(json.dumps(metadata, indent=2) + "\n")
                expected = case.reference(values)
    except ValueError as error:
        if case.settings.get("original_unsupported") and "broadcast" in str(error):
            (directory / "original.json").write_text(json.dumps({"status": "unsupported_original_domain", "error": str(error)}, indent=2) + "\n")
            return {"case_id": case.name, "status": "pending_original_domain", "error": str(error)}
        raise
    original_outputs = {}
    for name, reference in expected.items():
        reference = np.ascontiguousarray(reference)
        output_path = directory / ("original." + name + ".bin")
        reference.tofile(output_path)
        original_outputs[name] = {"path": str(output_path), "dtype": reference.dtype.str,
                                  "shape": list(reference.shape), "order": "C"}
    (directory / "original.json").write_text(json.dumps({"outputs": original_outputs}, indent=2) + "\n")
    started = time.perf_counter()
    process = subprocess.run([str(binary), case.family, case.method, str(path), str(prefix),
                              *map(str, shape), *map(str, case.arguments)],
                             text=True, capture_output=True, timeout=120)
    (directory / "port-execution.json").write_text(json.dumps({"seconds": time.perf_counter() - started,
                                                              "returncode": process.returncode}, indent=2) + "\n")
    (directory / "port.log").write_text(process.stdout + process.stderr)
    if process.returncode:
        raise AssertionError(f"port exit {process.returncode}: {process.stderr}")
    manifest = json.loads(Path(str(prefix) + ".json").read_text())
    assert manifest["prepared"] and manifest["processed"], "port processing status"
    outputs = {item["name"]: item for item in manifest["outputs"]}
    results = {}
    for name, reference in expected.items():
        item = outputs[name]
        actual = np.fromfile(str(prefix) + "." + name + ".bin", dtype=DTYPES[item["dtype"]]).reshape(item["shape"])
        reference = np.asarray(reference)
        results[name] = compare(name, actual, reference, expected)
    return {"case_id": case.name, "status": "passed", "evidence_category": case.evidence_category,
            "outputs": results}
