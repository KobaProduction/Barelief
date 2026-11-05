"""Compare Aubio graph capture to the separately compiled pinned original.

No DSP is reimplemented here. The existing original-comparison policies apply.
Raw source rejection/timeouts are recorded separately from numerical success.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools/rhythm_originals"))
import native_compare as nc


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def authority(root):
    pin = json.loads((root / "PIN.json").read_text(encoding="utf-8"))
    assert pin["revision"] == "ad5cf975aed08cc4562dd008cf9f83b12b82ffb8"
    hashes = {name: digest(root / name) for name in pin["files"]}
    assert hashes == pin["sha256"], "pinned source changed"
    return hashes


def graph(root, name, dtype="<f4", shape=None):
    value = np.fromfile(root / name, dtype=dtype)
    return value if shape is None else value.reshape(shape)


def comparisons(root, prefix):
    frames = (root / "pcm.f32").stat().st_size // (4 * 512)
    norm = nc.load(prefix, "norm", [frames, 513])[0]
    source_onsets = nc.load(prefix, "onsets", [frames, 9, 3])[0][:, 1, :]
    actual_onsets = graph(root, "onsets.f32", shape=(frames, 3))
    source_samples = nc.load(prefix, "onset_last", [frames, 9])[0][:, 1]
    source_tempo = nc.load(prefix, "tempo", [frames, 5])[0]
    result = {
        "magnitude": nc.compare("norm", graph(root, "norm.f32", shape=(frames, 513)), norm, None),
        "phase": nc.compare("phase", graph(root, "phase.f32", shape=(frames, 513)), nc.load(prefix, "phase", [frames, 513])[0], norm),
        "hfc_picker_decision": nc.compare("onsets", actual_onsets, source_onsets, None),
        "onset_samples": nc.compare("onset_last", graph(root, "onset_last.u64", "<u8"), source_samples, None),
        "onset_flags": nc.compare("onset_flags", (actual_onsets[:, 2] > 0).astype("u1"), (source_onsets[:, 2] > 0).astype("u1"), None),
        "tempo": nc.compare("tempo", graph(root, "tempo.f32", shape=(frames, 4)), source_tempo[:, :4], None),
        "beat_samples": nc.compare("tempo_last", graph(root, "tempo_last.u64", "<u8"), nc.load(prefix, "tempo_last", [frames])[0], None),
        "tatum_poll": nc.compare("tatum", graph(root, "tatum.u64", "<u8"), nc.load(prefix, "tatum", [frames])[0], None),
    }
    return result


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("evidence", type=Path)
    parser.add_argument("--source", required=True, type=Path)
    parser.add_argument("--original", required=True, type=Path)
    parser.add_argument("--wrapper", required=True, type=Path)
    parser.add_argument("--runtime-bin", type=Path)
    args = parser.parse_args()
    root = args.evidence.resolve()
    report_path = root / "aubio-parity.json"
    if report_path.exists():
        raise FileExistsError("Use fresh evidence; prior parity results are immutable")
    report = {"pin": authority(args.source), "policy": nc.POLICY,
              "binary_sha256": digest(args.original), "wrapper_sha256": digest(args.wrapper),
              "input_sha256": digest(root / "pcm.f32"), "profile": "44100/1024/512; hanningz; HFC onset/default specflux tempo",
              "limitations": "Tatum sample output is post-poll, source export is pre-poll and is not compared. FFT backend and source fidelity are established separately by the frozen graph and measured comparisons."}
    env = os.environ.copy()
    if args.runtime_bin:
        env["PATH"] = str(args.runtime_bin) + os.pathsep + env["PATH"]
    run_root = root / "raw-original-run"
    run_root.mkdir()
    prefix = run_root / "output"
    command = [str(args.original), "aubio", "1024", "512", "44100", str(root / "pcm.f32"), str(prefix), "0"]
    try:
        run = subprocess.run(command, capture_output=True, text=True, timeout=180, env=env)
        report["original_run"] = {"arguments": command[1:], "returncode": run.returncode, "stdout": run.stdout, "stderr": run.stderr}
        if run.returncode:
            report["status"] = "ORIGINAL_REJECT"
        else:
            report["original_output_sha256"] = {path.name: digest(path) for path in sorted(run_root.glob("output.*")) if path.is_file()}
            report["comparisons"] = comparisons(root, prefix)
            report["status"] = "PASS" if all(x["pass"] for x in report["comparisons"].values()) else "STRICT_FAIL"
    except subprocess.TimeoutExpired:
        report["status"] = "ORIGINAL_TIMEOUT"
    with report_path.open("x", encoding="utf-8") as output:
        json.dump(report, output, indent=2, allow_nan=False)
    print(json.dumps({"status": report["status"], "comparisons": report.get("comparisons", {})}), flush=True)
    return int(report["status"] != "PASS")


if __name__ == "__main__":
    raise SystemExit(main())
