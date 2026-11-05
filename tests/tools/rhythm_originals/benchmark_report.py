"""Collect finite prepared-DSP timings without substituting numerical acceptance."""
import argparse
import hashlib
import json
from pathlib import Path

import numpy as np

from common import DTYPES, PEAK_ATOL, RTOL, compare


def load_outputs(directory):
    original = json.loads((directory / "original.json").read_text())["outputs"]
    port = {item["name"]: item for item in json.loads((directory / "port.json").read_text())["outputs"]}
    expected = {name: np.fromfile(item["path"], dtype=item["dtype"]).reshape(item["shape"])
                for name, item in original.items()}
    actual = {name: np.fromfile(directory / ("port." + name + ".bin"), dtype=DTYPES[port[name]["dtype"]]).reshape(port[name]["shape"])
              for name in expected}
    return actual, expected


def errors(actual, expected):
    output = {}
    for name, reference in expected.items():
        value = actual[name]
        try:
            output[name] = {"status": "passed", **compare(name, value, reference, expected)}
        except AssertionError as error:
            output[name] = {"status": "failed", "error": str(error)}
        if value.shape == reference.shape and reference.dtype.kind not in "biu":
            finite = np.isfinite(reference) & np.isfinite(value)
            peak = max(float(np.abs(reference[finite]).max(initial=0)), 1e-30)
            delta = np.abs(value[finite] - reference[finite])
            output[name].update(max_absolute_error=float(delta.max(initial=0)),
                                max_peak_relative_error=float(delta.max(initial=0)) / peak)
            if name != "phase":
                output[name]["exceeds_fixed_budget_elements"] = int(np.sum(delta > RTOL * np.abs(reference[finite]) + PEAK_ATOL * peak))
    return output


def records(path):
    report = json.loads(path.read_text())
    environment = json.loads((path.parent / "execution-environment.json").read_text())
    for case in report["cases"]:
        directory = Path(case.get("fixture_directory", path.parent / case["case_id"]))
        metadata = json.loads((directory / "case.json").read_text())
        metrics = json.loads((directory / "port.benchmark.json").read_text())
        actual, expected = load_outputs(directory)
        comparisons = errors(actual, expected)
        definition = {"source": {key: metadata["source"][key] for key in ("library", "revision")},
                      "settings": {key: value for key, value in metadata["settings"].items()
                                   if key not in ("precision", "public_dtype")},
                      "inputs": {name: {key: value[key] for key in ("dtype", "shape", "order", "sha256")}
                                 for name, value in metadata["inputs"].items()}}
        yield {"case_id": case["case_id"], "base_case_id": case["case_id"].removesuffix(".desktop_reference"),
               "precision": report["precision"], "source": report["source"],
               "binary_sha256": environment["binary_sha256"],
               "execution_status": case["status"], "execution_error": case.get("error"),
               "metrology_status": "passed" if metrics["passed"] else "failed",
               "numerical_status": "passed" if all(item["status"] == "passed" for item in comparisons.values()) else "failed",
               "outputs": comparisons,
               "input_sha256": metadata["inputs"]["values"]["sha256"],
               "settings": {key: value for key, value in metadata["settings"].items()
                            if key not in ("precision", "public_dtype")},
               "definition": definition, "metrics": metrics, "results_manifest": str(path),
               "results_manifest_sha256": hashlib.sha256(path.read_bytes()).hexdigest()}


def pairs(rows):
    grouped = {}
    for row in rows:
        grouped.setdefault(row["base_case_id"], []).append(row)
    output = []
    for name, items in grouped.items():
        assert len(items) == 2, (name, "one Float32 and one explicit reference replay required")
        by_precision = {item["precision"]: item for item in items}
        assert set(by_precision) == {"portable-f32", "desktop-reference"}, name
        portable, desktop = by_precision["portable-f32"], by_precision["desktop-reference"]
        output.append({"case_id": name, "same_input_and_settings": portable["input_sha256"] == desktop["input_sha256"]
                       and portable["settings"] == desktop["settings"] and portable["source"] == desktop["source"],
                       "portable_numeric_status": portable["numerical_status"], "desktop_numeric_status": desktop["numerical_status"],
                       "portable_dsp_p95_us": portable["metrics"]["latency"]["p95_nearest_rank"],
                       "desktop_dsp_p95_us": desktop["metrics"]["latency"]["p95_nearest_rank"],
                       "portable_owned_workspace_bytes": portable["metrics"]["memory"]["owned_workspace_bytes"],
                       "desktop_owned_workspace_bytes": desktop["metrics"]["memory"]["owned_workspace_bytes"]})
    return output


def verify_binaries(rows, path):
    if path is None:
        assert len({row["binary_sha256"] for row in rows}) == 1, "one immutable representative exporter required"
        return None
    approved = json.loads(path.read_text())
    declared = {(item["base_case_id"], item["precision"]): item["binary_sha256"] for item in approved["profiles"]}
    observed = {(row["base_case_id"], row["precision"]): row["binary_sha256"] for row in rows}
    assert len(declared) == len(approved["profiles"]) and observed == declared, "exact approved per-case/profile binary map required"
    for artifact in [*approved["binary_artifacts"], approved["frozen_production_link_proof"]]:
        assert hashlib.sha256(Path(artifact["path"]).read_bytes()).hexdigest() == artifact["sha256"], artifact["path"]
    assert set(declared.values()) == {item["sha256"] for item in approved["binary_artifacts"]}, "all mapped binary artifacts required"
    return {"path": str(path), "sha256": hashlib.sha256(path.read_bytes()).hexdigest(), "approved": approved}


def representatives(rows, path, binary_map=None):
    manifest = json.loads(path.read_text())
    plans = [*manifest["representatives"], *manifest["explicit_boundary_children"]]
    expected = {(plan["base_case_id"], profile): plan for plan in plans for profile in plan["profiles"]}
    observed = {(row["base_case_id"], row["precision"]): row for row in rows}
    assert len(observed) == len(rows) and observed.keys() == expected.keys(), "exact representative/profile set required"
    binary_provenance = verify_binaries(rows, binary_map)
    for key, row in observed.items():
        plan = expected[key]
        if "definition" in plan:
            assert row["definition"] == plan["definition"], (key, "canonical input/settings/source drift")
        else:
            settings = row["definition"]["settings"]
            for field in ("n_fft", "hop", "samples", "seed"):
                assert settings[field] == plan[field], (key, field)
        row["kind"] = plan["kind"]
        row["explicit_fft_boundary"] = "definition" not in plan
    paired_names = {plan["base_case_id"] for plan in plans if len(plan["profiles"]) == 2}
    paired = pairs([row for row in rows if row["base_case_id"] in paired_names])
    for pair in paired:
        profile_rows = [row for row in rows if row["base_case_id"] == pair["case_id"]]
        pair["same_input_and_settings"] = profile_rows[0]["definition"] == profile_rows[1]["definition"]
    passed = all(row["metrics"]["passed"] for row in rows) and all(pair["same_input_and_settings"] for pair in paired)
    return {"format": "barelief-prepared-dsp-35-cost-v1", "representative_cost_passed": passed,
            "representative_kinds": manifest["representative_kinds"], "explicit_boundary_cases": len(manifest["explicit_boundary_children"]),
            "profiles": len(rows), "paired_cases": len(paired), "numerical_passed": sum(row["numerical_status"] == "passed" for row in rows),
            "numerical_failed_retained": sum(row["numerical_status"] == "failed" for row in rows),
            "all_numerically_accepted": all(row["numerical_status"] == "passed" for row in rows),
            "binary_provenance": binary_provenance,
            "all_heap_measured": False, "manifest": str(path), "manifest_sha256": hashlib.sha256(path.read_bytes()).hexdigest(),
            "scope": manifest["scope"], "pairs": paired, "cases": rows}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--results", type=Path, nargs="+", required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--expected-pairs", type=int, default=13)
    parser.add_argument("--representatives-manifest", type=Path)
    parser.add_argument("--approved-binary-map", type=Path)
    args = parser.parse_args()
    rows = [row for path in args.results for row in records(path)]
    if args.representatives_manifest:
        report = representatives(rows, args.representatives_manifest, args.approved_binary_map)
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(json.dumps(report, indent=2) + "\n")
        print(json.dumps({key: report[key] for key in ("representative_cost_passed", "representative_kinds", "profiles", "numerical_passed", "numerical_failed_retained")}))
        return not report["representative_cost_passed"]
    paired = pairs(rows)
    passed = (len(paired) == args.expected_pairs and all(item["same_input_and_settings"] for item in paired)
              and all(row["metrics"]["passed"] for row in rows))
    report = {"format": "barelief-prepared-dsp-paired-cost-v1", "cost_pilot_passed": passed,
              "paired_cases": len(paired), "numerical_passed": sum(row["numerical_status"] == "passed" for row in rows),
              "numerical_failed_retained": sum(row["numerical_status"] == "failed" for row in rows),
              "all_numerically_accepted": all(row["numerical_status"] == "passed" for row in rows),
              "pairs": paired, "cases": rows,
              "scope": "finite pilot of complete prepared PCM novelty/tempogram process calls; no whole-35-method, all-mode, long-history, or all-heap measurement claim"}
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(report, indent=2) + "\n")
    print(json.dumps({key: report[key] for key in ("cost_pilot_passed", "paired_cases", "numerical_passed", "numerical_failed_retained")}))
    return not passed


if __name__ == "__main__":
    raise SystemExit(main())
