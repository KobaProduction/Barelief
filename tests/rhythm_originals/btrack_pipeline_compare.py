"""Compare the graph trace to separately compiled pinned BTrack; no DSP oracle.

The raw oracle and an optional explicitly repaired oracle have separate results.
Use the existing src_simple observation bridge to compare pre-threshold SRC.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess

import numpy as np


def digest(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def load(prefix: Path, name: str) -> np.ndarray:
    paths = [Path(f"{prefix}.{name}.{suffix}") for suffix in ("f32", "f64", "u64")]
    paths = [path for path in paths if path.exists()]
    if len(paths) != 1:
        raise ValueError(f"Expected one file for {prefix}.{name}; found {paths}")
    return np.fromfile(paths[0], dtype={".f32": "<f4", ".f64": "<f8", ".u64": "<u8"}[paths[0].suffix])


def compare(actual: np.ndarray, expected: np.ndarray, exact: bool = False) -> dict:
    if actual.shape != expected.shape:
        return {"status": "FAIL", "reason": "shape", "actual": list(actual.shape), "expected": list(expected.shape)}
    finite = np.isfinite(actual) & np.isfinite(expected)
    error = np.abs(actual.astype(np.float64) - expected)
    # Declared Float32 profile tolerance; events and rounded periods are exact.
    allowed = 0 if exact else .002 + .0005 * np.abs(expected)
    bad = ~finite | (error > allowed)
    return {"status": "FAIL" if np.any(bad) else "PASS", "mismatches": int(np.count_nonzero(bad)),
            "max_abs_error": float(np.max(error[finite], initial=0)), "exact": exact}


def trace_comparisons(graph: Path, original: Path, raw_src: bool) -> dict:
    beat = load(graph, "beat")
    count = beat.size
    source_beat = load(original, "beat")
    actual_state = load(graph, "state").reshape(count, 3)
    state = load(original, "state").reshape(count, 3)
    source_onsets = load(original, "odf").reshape(count, 10)[:, 6]
    due = source_beat.astype(bool)
    result = {
        "odf_positive_complex_difference": compare(load(graph, "odf"), source_onsets),
        "beat_flags": compare(beat, source_beat, exact=True),
        "bpm": compare(actual_state[:, 0], state[:, 0]),
        "score": compare(actual_state[:, 1], state[:, 1]),
        "period": compare(actual_state[:, 2], state[:, 2], exact=True),
        "tempo_state_each_hop": compare(load(graph, "delta").reshape(count, 41), load(original, "delta").reshape(count, 41)),
        "comb_on_original_beats": compare(load(graph, "comb").reshape(count, 128)[due], load(original, "comb").reshape(count, 128)[due]),
    }
    if raw_src:
        result["sinc_before_threshold_on_original_beats"] = compare(
            load(graph, "resampled").reshape(count, 512)[due], load(original, "resampled").reshape(count, 512)[due])
    else:
        result["sinc_before_threshold_on_original_beats"] = {
            "status": "NOT_OBSERVED", "reason": "raw src_simple observation bridge not declared; in-place thresholded source is not raw SRC"}
    return result


def run_original(binary: Path, kind: str, options: argparse.Namespace) -> dict:
    # Each invocation owns a newly created directory. Returncode0 cannot cause
    # files from a previous binary/run to be mistaken for this oracle's output.
    directory = options.run_root / kind
    directory.mkdir()
    prefix = directory / "trace"
    command = [str(binary), "btrack", "1024", "512", "44100", f"{options.graph_prefix}.pcm.f32", str(prefix), "1"]
    result = {"kind": kind, "binary": str(binary), "sha256": digest(binary), "command": command}
    try:
        run = subprocess.run(command, capture_output=True, text=True, timeout=180, env=os.environ.copy())
    except subprocess.TimeoutExpired:
        return {**result, "status": "ORIGINAL_REJECT", "reason": "timeout"}
    result.update(returncode=run.returncode, stdout=run.stdout, stderr=run.stderr)
    if run.returncode:
        return {**result, "status": "ORIGINAL_REJECT"}
    try:
        comparisons = trace_comparisons(options.graph_prefix, prefix, options.original_captures_raw_src)
    except (OSError, ValueError) as error:
        return {**result, "status": "ORACLE_OUTPUT_REJECT", "reason": str(error)}
    result["comparisons"] = comparisons
    result["output_sha256"] = {path.name: digest(path) for path in directory.iterdir() if path.is_file()}
    result["status"] = "FAIL" if any(value["status"] == "FAIL" for value in comparisons.values()) else "PASS"
    result["complete_intermediate_scope"] = options.original_captures_raw_src
    return result


def fresh_run_root(output: Path) -> Path:
    if output.exists():
        raise FileExistsError(f"Prior evidence report exists: {output}; choose a fresh output")
    run_root = output.parent / f"{output.stem}-runs"
    # mkdir's exclusive creation also rejects an interrupted previous run or
    # concurrent invocation, without deleting or overwriting any evidence.
    run_root.mkdir(parents=True, exist_ok=False)
    return run_root


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--graph-prefix", type=Path, required=True)
    parser.add_argument("--original", type=Path, required=True)
    parser.add_argument("--safe-original", type=Path)
    parser.add_argument("--original-captures-raw-src", action="store_true")
    parser.add_argument("--pins", type=Path, required=True, help="BTrack pinned source root containing PIN.json")
    parser.add_argument("--output", type=Path, required=True)
    options = parser.parse_args()
    pin = json.loads((options.pins / "PIN.json").read_text(encoding="utf8"))
    assert pin["revision"] == "9d6127618a5679e9caa74c594b88f1d74f0e035f"
    assert all(digest(options.pins / name) == pin["sha256"][name] for name in pin["files"])
    options.run_root = fresh_run_root(options.output)
    results = [run_original(options.original, "raw-original", options)]
    if options.safe_original:
        results.append(run_original(options.safe_original, "safe-original", options))
    report = {"source_revision": pin["revision"], "pcm_sha256": digest(Path(f"{options.graph_prefix}.pcm.f32")),
              "recipe": "BTrack() 44100/N1024/hop512/Hann/CSDHWR/source sinc", "results": results,
              "limits": "One bounded eight-changing-tone synthetic PCM trace; not general music quality or exhaustive library parity."}
    with options.output.open("x", encoding="utf8") as output:
        output.write(json.dumps(report, indent=2, allow_nan=False))
    print(json.dumps({result["kind"]: result["status"] for result in results}))
    return int(any(result["status"] != "PASS" for result in results))


if __name__ == "__main__":
    raise SystemExit(main())
