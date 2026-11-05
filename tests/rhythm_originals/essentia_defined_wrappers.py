"""Sole owner builds two canonical observers against the sealed defined object.

No cached source, archive, binary or prior evidence is modified. This creates
new wrapper binaries and provenance, without executing any parity cases.
"""
import argparse
import json
import os
from pathlib import Path

import essentia_defined_behavior_probe as causal

REPAIR_SHA = "a7a538fa8b5a2b123cd4826f3705607c44c90470263480335d512597b6d232e3"
DEFINED_SHA = "557b22ea5df429003c0dfe035541cd379faececd73e88983e227b63502b07c67"
OBJECT_SHA = "7e42d172ebb802ad7425d09c6f87cf76bb52722aaae158698ee90b00dd130409"
HELPER_SHA = "957fe0ce21553ca26e6f81ebf16b607bdba159b02ba4dae587f233ac265dee5d"
WRAPPERS = {
    "tempo": ("essentia_tempo_original_driver.cpp", "72cf37e8c0f2db540d1811d7ff34f2c2bc461eb18cdc15fe02f532854c94ab26"),
    "summary": ("essentia_family_original_driver.cpp", "a29641e0e269305d24fca2f3cb6d99c74e52b43c762068ae831dac31f2aa54ea"),
}


def bound_file(path, expected=None):
    actual = causal.sha(path)
    assert expected is None or actual == expected, str(path)
    return {"path": str(path), "sha256": actual}


def repair_identity(folder):
    manifest = folder / "report.json"
    proof = bound_file(manifest, REPAIR_SHA)
    report = json.loads(manifest.read_text(encoding="utf8"))
    assert report["complete"] and report["pass"] and len(report["cases"]) == 3
    assert report["variant"]["sha256"] == DEFINED_SHA
    return {"repair_report": proof,
            "defined_source": bound_file(Path(report["variant"]["path"]), DEFINED_SHA),
            "defined_object": bound_file(folder / "build/global-defined.o", OBJECT_SHA)}


def snapshots(args):
    folder = args.evidence / "source"
    folder.mkdir()
    result = {}
    for family, (name, expected) in WRAPPERS.items():
        original = args.native / name
        proof = bound_file(original, expected)
        target = folder / name
        target.write_bytes(original.read_bytes())
        result[family] = {**bound_file(target, expected), "canonical": proof}
    return result


def extra_sources(args):
    pin = json.loads(args.pin_manifest.read_text(encoding="utf8"))
    names = ("src/algorithms/rhythm/onsetrate.cpp", "src/algorithms/rhythm/rhythmtransform.cpp",
             "src/algorithms/extractor/rhythmdescriptors.cpp")
    return [bound_file(args.source / name, pin["sha256"][name]) for name in names]


def build(args, report):
    folder, binaries = args.evidence / "build", args.evidence / "bin"
    folder.mkdir(); binaries.mkdir()
    source = args.source / "src"
    includes = [args.native, source, source / "essentia", source / "essentia/scheduler",
                source / "essentia/streaming", source / "essentia/streaming/algorithms",
                source / "essentia/utils", source / "3rdparty", source / "3rdparty/kiss_fft130",
                args.native / "eigen-3.4.0"]
    flags = ["-O3", "-DNDEBUG", "-std=gnu++11", "-DNOMINMAX", "-D_USE_MATH_DEFINES",
             "-DEIGEN_PERMANENTLY_DISABLE_STUPID_WARNINGS", *[f"-I{path}" for path in includes]]
    for family, wrapper in report["wrappers"].items():
        output = binaries / f"essentia-{family}-defined.exe"
        extras = [row["path"] for row in report["extra_sources"]] if family == "summary" else []
        argv = [args.compiler, *flags, wrapper["path"], *extras, report["defined_object"]["path"],
                args.native / "build/libessentia_original.a", "-o", output]
        command, _, _ = causal.command(args, folder, f"link-{family}", argv, 180)
        report["commands"].append(command)
        if command.get("returncode") != 0:
            raise RuntimeError(f"canonical {family} wrapper build failed; see immutable build logs")
        report["binaries"][family] = bound_file(output)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("source", "native", "repair-evidence", "evidence"):
        parser.add_argument(f"--{name}", type=Path, required=True)
    parser.add_argument("--pin-manifest", type=Path, default=Path(__file__).resolve().parents[2] /
                        ".agent-workspace/rhythm-pc-max/upstream-full/essentia-b9fa6cb6/PIN.json")
    parser.add_argument("--compiler", type=Path, default=Path(
        "C:/Program Files/JetBrains/CLion 2025.3.3/bin/mingw/bin/g++.exe"))
    args = parser.parse_args()
    assert os.name == "nt"
    assert causal.sha(causal.__file__) == HELPER_SHA, "accepted causal build helper changed"
    for name in ("source", "native", "repair_evidence", "evidence", "pin_manifest"):
        setattr(args, name, getattr(args, name).resolve())
    assert not args.evidence.exists() and len(str(args.evidence)) < 165
    identity, repair = causal.identity(args), repair_identity(args.repair_evidence)
    assert identity["compiler_sha256"] == json.loads(Path(repair["repair_report"]["path"]).read_text())["identity"]["compiler_sha256"]
    args.evidence.mkdir(parents=True)
    report = {"variant": "defined-global", "complete": False, "identity": identity, **repair,
              "script_sha256": causal.sha(__file__), "commands": [], "binaries": {},
              "scope": "canonical observation wrappers; underlying algorithms unchanged except sealed two-line global repair"}
    try:
        report["wrappers"] = snapshots(args)
        report["extra_sources"] = extra_sources(args)
        build(args, report)
        report["complete"] = True
    except Exception as error:
        report["failure"] = {"type": type(error).__name__, "message": str(error)}
    finally:
        causal.write_json(args.evidence / "manifest.json", report)
    print(json.dumps({"manifest": str(args.evidence / "manifest.json"), "complete": report["complete"]}))
    return int(not report["complete"])


if __name__ == "__main__":
    raise SystemExit(main())
