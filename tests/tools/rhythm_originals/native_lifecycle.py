"""Actual pinned aubio/BTrack lifecycle calls versus prepared fixed-hop ports."""
import argparse
import copy
import hashlib
import json
import os
from pathlib import Path
import subprocess

import numpy as np

from native_compare import POLICY, compare, signal
from native_lifecycle_cases import CASES
from run import case_directory


SHAPES = {0: (9, 1), 1: (8, 2), 2: (1, 4), 3: (684, 4)}


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def save(path, value):
    path.write_text(json.dumps(value, indent=2, allow_nan=False) + "\n")


def geometry(case):
    hop = case["settings"]["hop"]
    rows = case["settings"]["warm_hops"] + case["settings"]["probe_hops"]
    count = 0
    for step in range(rows):
        for action in case["program"]:
            if action[0] == step and action[1] == 3:
                hop = int(action[2])
        count += hop
    return rows, count


def plan_text(case, component):
    settings = case["settings"]
    header = (component, settings["frame"], settings["hop"], settings["sample_rate"],
              case["plan_method"], case["plan_window"], int(case["fresh_pair"]), len(case["program"]))
    return " ".join(map(str, header)) + "\n" + "".join(" ".join(map(str, action)) + "\n" for action in case["program"])


def outputs(prefix, component, rows, reference):
    width, integers = SHAPES[component]
    values = np.fromfile(str(prefix) + (".values.f64" if reference else ".values.f32"), dtype="<f8" if reference else "<f4").reshape(rows, width)
    exact = np.fromfile(str(prefix) + ".integers.u64", dtype="<u8").reshape(rows, integers)
    fresh = np.fromfile(str(prefix) + ".fresh.u64", dtype="<u8")
    assert fresh.shape == (rows,) and np.all(np.isin(fresh, (0, 1, 2))), "complete fresh-equivalence observations"
    result = {"integers": exact}
    if component == 3:
        result.update(state=values[:, :3], resampled=values[:, 3:515], comb=values[:, 515:643], delta=values[:, 643:684])
    elif component == 0:
        result.update(onsets=values[:, :3], controls=values[:, 3:])
    elif component == 1:
        result.update(tempo=values[:, :5], controls=values[:, 5:])
    else:
        result["odf"] = values
    return result, fresh


def compare_outputs(actual, expected):
    result = {}
    for name, value in expected.items():
        result[name] = ({"pass": bool(np.array_equal(actual[name], value)), "policy": "exact public control values"}
                        if name == "controls" else compare(name, actual[name], value, None))
    return result


def run(binary, plan, pcm, prefix, runtime_path):
    environment = os.environ.copy()
    environment["PATH"] = str(runtime_path) + os.pathsep + environment.get("PATH", "")
    command = [str(binary), str(plan), str(pcm), str(prefix)]
    dlls = [directory / name for directory in (binary.parent, runtime_path)
            for name in ("libstdc++-6.dll", "libgcc_s_seh-1.dll", "libwinpthread-1.dll") if (directory / name).exists()]
    execution = {"command": command, "binary_sha256": digest(binary), "timeout_seconds": 180,
                 "runtime_path_prepend": str(runtime_path), "actual_PATH": environment["PATH"],
                 "runtime_dll_candidates": [{"path": str(path), "sha256": digest(path)} for path in dlls],
                 "dll_provenance_scope": "declared runtime files/candidates; loaded-module tracing not performed"}
    try:
        result = subprocess.run(command, env=environment, capture_output=True, text=True, timeout=180)
    except subprocess.TimeoutExpired as error:
        save(prefix.with_suffix(".execution.json"), {**execution, "returncode": None, "timeout": True})
        raise RuntimeError(f"{binary.name}: exceeded 180 second producer timeout") from error
    save(prefix.with_suffix(".execution.json"), {**execution,
         "returncode": result.returncode, "stdout": result.stdout, "stderr": result.stderr})
    if result.returncode:
        raise RuntimeError(f"{binary.name}: {result.stderr.strip()}")


def subcase(case, component, directory, pcm, rows, args):
    folder = directory / str(component)
    folder.mkdir(exist_ok=True)
    plan = folder / "plan.txt"
    plan.write_text(plan_text(case, component))
    reference, port = folder / "reference", folder / "port"
    run(args.original, plan, pcm, reference, args.original_runtime_path or args.runtime_path)
    run(args.port, plan, pcm, port, args.runtime_path)
    expected, source_fresh = outputs(reference, component, rows, True)
    actual, port_fresh = outputs(port, component, rows, False)
    checks = compare_outputs(actual, expected)
    memory = np.fromfile(str(port) + ".memory.u64", dtype="<u8").reshape(rows, 3)
    bounded = bool(np.all(memory[:, 2] == 1) and np.all(memory[:, 1] <= memory[:, 0]))
    paired_rows = int(np.sum(port_fresh != 2))
    result = {"component": component, "outputs": checks,
              "source_reconstruction_repeatable": bool(np.all(source_fresh != 0)) if paired_rows else None,
              "port_restart_fresh_equivalent": bool(np.all(port_fresh != 0)) if paired_rows else None, "paired_rows": paired_rows}
    result["prepared_workspace"] = {"within_queried_bound": bounded, "maximum_queried_bytes": int(memory[:, 0].max()),
                                    "maximum_owned_bytes": int(memory[:, 1].max()), "hot_heap_measured": False}
    result["passed"] = all(value["pass"] for value in checks.values()) and result["port_restart_fresh_equivalent"] is not False and result["source_reconstruction_repeatable"] is not False and bounded
    if case["settings"].get("tail_samples"):
        result["host_tail"] = tail_check(case, component, folder, rows, args, expected, actual)
        result["passed"] &= result["host_tail"]["source_prefix_equal"] and result["host_tail"]["port_prefix_equal"]
    return result


def tail_check(case, component, folder, rows, args, expected, actual):
    count = rows * case["settings"]["hop"]
    values = np.fromfile(folder.parent / "input.f32", dtype="<f4")[:count]
    path = folder / "complete-prefix.f32"
    values.tofile(path)
    plan = folder / "plan.txt"
    run(args.original, plan, path, folder / "reference-prefix", args.original_runtime_path or args.runtime_path)
    run(args.port, plan, path, folder / "port-prefix", args.runtime_path)
    source_prefix, _ = outputs(folder / "reference-prefix", component, rows, True)
    port_prefix, _ = outputs(folder / "port-prefix", component, rows, False)
    exact = lambda a, b: all(a[name].tobytes() == b[name].tobytes() for name in a)
    return {"policy": "host passes complete fixed hops only; no source EOF call or flush invented",
            "dropped_tail_samples": case["settings"]["tail_samples"],
            "source_prefix_equal": exact(source_prefix, expected), "port_prefix_equal": exact(port_prefix, actual)}


def execute(case, args):
    directory = case_directory(args.output, case["case_id"], True)
    rows, count = geometry(case)
    pcm = signal("tones8", count + case["settings"].get("tail_samples", 0), case["settings"]["sample_rate"])
    path = directory / "input.f32"
    pcm.tofile(path)
    save(directory / "case.json", {**case, "input": {"dtype": "float32-le", "shape": [len(pcm)], "sha256": digest(path)},
         "rows": rows, "source_build_manifest_sha256": digest(args.source_build_manifest), "policy": POLICY})
    components = [0, 1] if case["prepared_kind"] == "PreparedPcmOnsetSequence+PreparedPcmContextTempo" else [case["component"]]
    try:
        results = [subcase(case, component, directory, path, rows, args) for component in components]
        return {"case_id": case["case_id"], "passed": all(item["passed"] for item in results),
                "behavior_class": case["behavior_class"], "evidence_category": case["evidence_category"],
                "subobservations": results, "fixture_directory": str(directory.resolve())}
    except Exception as error:
        return {"case_id": case["case_id"], "passed": False, "execution_error": str(error), "fixture_directory": str(directory.resolve())}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--original", type=Path, required=True)
    parser.add_argument("--port", type=Path, required=True)
    parser.add_argument("--source-build-manifest", type=Path, required=True)
    parser.add_argument("--runtime-path", type=Path, required=True)
    parser.add_argument("--original-runtime-path", type=Path)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--filter", default="")
    args = parser.parse_args()
    args.original, args.port = args.original.resolve(), args.port.resolve()
    args.output.mkdir(parents=True, exist_ok=True)
    identities = {str(path): digest(path) for path in (args.original, args.port)}
    results = []
    for case in CASES:
        if args.filter and args.filter not in case["case_id"]:
            continue
        assert all(digest(Path(path)) == value for path, value in identities.items()), "immutable lifecycle binaries"
        result = execute(copy.deepcopy(case), args)
        results.append(result)
        print("PASS" if result["passed"] else "FAIL", case["case_id"], flush=True)
    report = {"format": "barelief-native-lifecycle-results-v1", "cases": results, "binary_sha256": identities,
              "source_build_manifest": str(args.source_build_manifest.resolve()), "source_build_manifest_sha256": digest(args.source_build_manifest),
              "passed": sum(item["passed"] for item in results), "failed": sum(not item["passed"] for item in results),
              "scope": "finite fixed-hop source controls and explicitly labelled reconstruction/host-framing extensions; no variable-chunk or source EOF/reset API claim"}
    save(args.output / "results.json", report)
    return bool(report["failed"])


if __name__ == "__main__":
    raise SystemExit(main())
