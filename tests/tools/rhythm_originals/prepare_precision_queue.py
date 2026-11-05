"""Materialize finite inputs and seal actual-original caches before the run."""
import argparse
import hashlib
import json
from pathlib import Path
import numpy as np

from common import input_record
import original_sources
import librosa_cases
import composition_cases
import precision_cases


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source-cache", type=Path, required=True)
    parser.add_argument("--evidence", type=Path, required=True)
    args = parser.parse_args()
    output = args.evidence / "precision-queue"
    output.mkdir(exist_ok=True)
    lib = original_sources.librosa(args.source_cache)
    cases = [*librosa_cases.cases(lib), *composition_cases.cases(lib), *precision_cases.cases(lib)]
    inputs = []
    for case in cases:
        directory = output / "inputs" / case.name
        directory.mkdir(parents=True, exist_ok=True)
        records = {"values": input_record(directory / "input.f32", case.values)}
        for suffix, values in case.sidecars.items():
            records[suffix] = input_record(Path(str(directory / "input.f32") + suffix), values)
        inputs.append({"case_id": case.name, "inputs": records, "arguments": case.arguments,
                       "settings": case.settings, "reference_policy": case.policy})
    integrity = {}
    for name in ("librosa", "librosa-recheck1", "librosa-compositions", "madmom", "madmom-recheck1", "madmom-compositions"):
        for path in (args.evidence / name).rglob("*"):
            if path.is_file() and (path.name in ("case.json", "original.json") or path.name.startswith(("original.", "input.f32"))):
                integrity[str(path.resolve())] = hashlib.sha256(path.read_bytes()).hexdigest()
    (output / "reference-cache-shas.json").write_text(json.dumps(integrity, indent=2) + "\n")
    jobs = [
        {"name": "desktop-previous6", "packet": "base", "precision": "desktop-reference", "cache": "librosa-recheck1", "failed_manifest": "librosa-recheck1/results.json"},
        {"name": "desktop-composition4", "packet": "compositions", "precision": "desktop-reference", "cache": "librosa-compositions", "failed_manifest": "librosa-compositions/results.json"},
        {"name": "portable-holdouts4", "packet": "precision-holdouts", "precision": "portable-f32", "cache": None},
        {"name": "desktop-holdouts4", "packet": "precision-holdouts", "precision": "desktop-reference", "cache": "portable-holdouts4"},
        {"name": "desktop-full114", "packet": "librosa-full", "precision": "desktop-reference", "cache": "librosa"},
        {"name": "madmom-full84", "packet": "madmom-full", "precision": "portable-f32", "cache": "madmom"}]
    report = {"source": original_sources.source_record("librosa"), "inputs": inputs, "jobs": jobs,
              "state": "awaiting owner BASELINE_READY; no executable or original numerical reference calls executed",
              "policy": "strict existing budgets; original cache checked by source pin/settings/input bytes and sealed SHA256; PortableFloat32 failures retained separately"}
    (output / "queue.json").write_text(json.dumps(report, indent=2) + "\n")
    print(json.dumps({"inputs": len(inputs), "sealed_cache_files": len(integrity), "jobs": len(jobs)}))


if __name__ == "__main__":
    main()
