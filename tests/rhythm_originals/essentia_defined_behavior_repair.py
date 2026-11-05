"""Owner-run follow-up to the sealed causal v2; three new repaired-source cases.

The pin/cache and prior evidence remain unchanged. Source repairs are only
reserve->resize and InfoGain entry clear. Streaming uses its existing public
SourceBase.setBufferInfo before network construction, without a source patch.
"""
import argparse
import json
import os
from pathlib import Path

import essentia_defined_behavior_probe as causal

CAUSAL_SHA = "41c3815170b04cce8c27a52e08177d3d29532f150fd531d5553e6b5836cfb2e7"
HELPER_SHA = "957fe0ce21553ca26e6f81ebf16b607bdba159b02ba4dae587f233ac265dee5d"


def prior_evidence(folder):
    manifest = folder / "report.json"
    assert causal.sha(manifest) == CAUSAL_SHA, "requires the accepted immutable causal v2"
    report = json.loads(manifest.read_text(encoding="utf8"))
    assert report["complete"] and report["observations_match_protocol"]
    assert len(report["cases"]) == 14
    rows = {row["tag"]: row for row in report["cases"]}
    baselines = {}
    for tag, name in (("native-storage-sized-reuse-info", "o.empty.f32"),
                      ("native-storage-sized-bounds", "o.f32")):
        path = folder / "cases" / tag / name
        assert causal.sha(path) == rows[tag]["numeric_artifacts"][name]["sha256"]
        baselines[tag] = {"path": str(path), "sha256": causal.sha(path)}
    return {"manifest": str(manifest), "sha256": CAUSAL_SHA, "baselines": baselines}


def defined_source(args):
    source = (args.source / causal.GLOBAL).read_bytes()
    reserve = b"sorted.reserve(_numberERBBands);"
    entry = (b"void OnsetDetectionGlobal::computeInfoGain() {\n"
             b"  vector<Real>& onsetDetections = _onsetDetections.get();")
    assert source.count(reserve) == 1 and source.count(entry) == 1
    content = source.replace(reserve, b"sorted.resize(_numberERBBands);")
    content = content.replace(entry, entry + b"\n  onsetDetections.clear();")
    folder = args.evidence / "source"
    folder.mkdir()
    target = folder / "onsetdetectionglobal-defined.cpp"
    target.write_bytes(content)
    return {"path": str(target), "sha256": causal.sha(target),
            "patches": ["one reserve -> resize", "InfoGain entry clear only"],
            "streaming_source_patch": False}


def build(args, variant):
    folder, binaries = args.evidence / "build", args.evidence / "bin"
    folder.mkdir(); binaries.mkdir()
    source = args.source / "src"
    includes = [args.native, source, source / "essentia", source / "essentia/scheduler",
                source / "essentia/streaming", source / "essentia/streaming/algorithms",
                source / "essentia/utils", source / "3rdparty", source / "3rdparty/kiss_fft130",
                source / "algorithms/rhythm", args.native / "eigen-3.4.0"]
    flags = ["-std=gnu++11", "-O1", "-g", "-DNOMINMAX", "-D_USE_MATH_DEFINES",
             "-DEIGEN_PERMANENTLY_DISABLE_STUPID_WARNINGS", "-D_GLIBCXX_ASSERTIONS",
             *[f"-I{path}" for path in includes]]
    probe = Path(__file__).with_name("essentia_global_native_probe.cpp")
    obj, native_obj = folder / "native-probe.o", folder / "global-defined.o"
    causal.compile_one(args, folder, "native-probe", [*flags, "-c", probe, "-o", obj])
    causal.compile_one(args, folder, "global-defined", [*flags, "-c", variant["path"], "-o", native_obj])
    output = binaries / "native-defined-checked.exe"
    causal.compile_one(args, folder, "link-defined", [obj, native_obj,
                       args.native / "build/libessentia_original.a", "-o", output])
    return {"defined": {"path": str(output), "sha256": causal.sha(output)}}


def compare_artifact(folder, name, baseline):
    path = folder / name
    actual = causal.sha(path)
    return {"pass": actual == baseline["sha256"], "actual_sha256": actual,
            "expected_sha256": baseline["sha256"], "expected_path": baseline["path"]}


def cases(args, report):
    specs = (("defined-reuse-info", ["reuse-info"], ["OUTPUT_PREFILL_INDEPENDENT"]),
             ("defined-emphasis-baseline", ["bounds"], ["COMPLETED"]),
             ("defined-stream-provisioned-327681", ["stream-provisioned"], ["COMPLETED"]))
    for tag, mode, expectation in specs:
        prefix = args.evidence / "cases" / tag / "o"
        values = [*mode, str(prefix), *(["327681"] if "stream" in tag else [])]
        row = causal.run_case(args, args.evidence, report["binaries"], tag, "defined", values, expectation)
        report["cases"].append(row)
        folder = prefix.parent
        if "reuse" in tag:
            expected = report["causal_v2"]["baselines"]["native-storage-sized-reuse-info"]
            row["exact_comparisons"] = {name: compare_artifact(folder, name, expected)
                                        for name in ("o.empty.f32", "o.prefilled.f32")}
        elif "emphasis" in tag:
            expected = report["causal_v2"]["baselines"]["native-storage-sized-bounds"]
            row["exact_comparisons"] = {"fresh_emphasis_unchanged": compare_artifact(folder, "o.f32", expected)}
        else:
            expected = {"path": str(folder / "o.reference.f32"), "sha256": causal.sha(folder / "o.reference.f32")}
            row["exact_comparisons"] = {"stream_matches_actual_standard": compare_artifact(folder, "o.f32", expected)}
            text = (folder / "process.stdout").read_text(encoding="utf8", errors="replace")
            row["cold_public_capacity_observed"] = "PROVISION phase=before_network capacity=327681" in text
        row["pass"] = row["observation_expected"] and all(value["pass"] for value in row["exact_comparisons"].values())
        if "stream" in tag:
            row["pass"] = row["pass"] and row["cold_public_capacity_observed"]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", type=Path, required=True)
    parser.add_argument("--native", type=Path, required=True)
    parser.add_argument("--causal-evidence", type=Path, required=True)
    parser.add_argument("--evidence", type=Path, required=True)
    parser.add_argument("--pin-manifest", type=Path, default=Path(__file__).resolve().parents[2] /
                        ".agent-workspace/rhythm-pc-max/upstream-full/essentia-b9fa6cb6/PIN.json")
    parser.add_argument("--compiler", type=Path, default=Path(
        "C:/Program Files/JetBrains/CLion 2025.3.3/bin/mingw/bin/g++.exe"))
    parser.add_argument("--timeout", type=int, default=90)
    args = parser.parse_args()
    assert os.name == "nt" and 1 <= args.timeout <= 180
    assert causal.sha(Path(causal.__file__)) == HELPER_SHA, "causal helper changed"
    for field in ("source", "native", "evidence", "causal_evidence", "pin_manifest"):
        setattr(args, field, getattr(args, field).resolve())
    assert not args.evidence.exists() and len(str(args.evidence)) < 165
    info, prior = causal.identity(args), prior_evidence(args.causal_evidence)
    args.evidence.mkdir(parents=True)
    report = {"identity": info, "causal_v2": prior, "complete": False, "cases": [],
              "script_sha256": causal.sha(__file__),
              "probe_sha256": causal.sha(Path(__file__).with_name("essentia_global_native_probe.cpp")),
              "scope": "three new defined-source regressions; not a full MultiFeature graph parity claim"}
    try:
        report["variant"] = defined_source(args)
        report["binaries"] = build(args, report["variant"])
        cases(args, report)
        report["complete"] = True
        report["pass"] = all(row["pass"] for row in report["cases"])
    except Exception as error:
        report["failure"] = {"type": type(error).__name__, "message": str(error)}
    finally:
        causal.write_json(args.evidence / "report.json", report)
    print(json.dumps({"report": str(args.evidence / "report.json"),
                      "complete": report["complete"], "pass": report.get("pass", False)}))
    return int(not report.get("pass", False))


if __name__ == "__main__":
    raise SystemExit(main())
