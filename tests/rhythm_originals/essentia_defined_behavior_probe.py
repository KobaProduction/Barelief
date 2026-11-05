"""Owner-run causal probes; never modify the pin, native cache, or old evidence.

This first diagnostic packet applies ONLY reserve -> resize in one fresh copy.
Output clearing and streaming-capacity repairs require the measured follow-up.
_GLIBCXX_DEBUG is used only by an isolated standalone STL executable. Native
Essentia objects use ABI-compatible _GLIBCXX_ASSERTIONS, not debug containers.
"""
import argparse
from datetime import datetime, timezone
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
import sys

PIN = "b9fa6cb674ca43dfb94d28d293aeda441c6745db"
GLOBAL = "src/algorithms/rhythm/onsetdetectionglobal.cpp"
GLOBAL_SHA = "666915af9e462571f3a29a4c2cb0271a085191f5b6772453597014edf69a124a"
LIB_SHA = "29120a49b87824bc64132f50b58f5a51be0f30a40685ddea31ee65c82263e1b5"


def sha(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def write_json(path, value):
    path.write_text(json.dumps(value, indent=2), encoding="utf8")


def identity(args):
    pin = json.loads(args.pin_manifest.read_text(encoding="utf8"))
    assert pin["revision"] == PIN, "unexpected source pin"
    names = (GLOBAL, "src/algorithms/rhythm/onsetdetectionglobal.h",
             "src/algorithms/rhythm/beattrackermultifeature.cpp",
             "src/algorithms/rhythm/rhythmextractor2013.cpp")
    hashes = {name: sha(args.source / name) for name in names}
    assert hashes[GLOBAL] == GLOBAL_SHA
    for name, actual in hashes.items():
        assert actual == pin["sha256"][name], name
    library = args.native / "build" / "libessentia_original.a"
    assert sha(library) == LIB_SHA, "native library identity changed"
    assert args.compiler.is_file(), "existing compiler is required; no installation is performed"
    return {"revision": PIN, "pin_manifest": str(args.pin_manifest),
            "pin_manifest_sha256": sha(args.pin_manifest), "source_hashes": hashes, "library": str(library),
            "library_sha256": LIB_SHA, "compiler": str(args.compiler),
            "compiler_sha256": sha(args.compiler)}


def command(args, folder, name, argv, timeout):
    env = os.environ.copy()
    env["PATH"] = str(args.compiler.parent) + os.pathsep + env.get("PATH", "")
    record = {"argv": [str(value) for value in argv], "timeout_seconds": timeout}
    try:
        done = subprocess.run(record["argv"], capture_output=True, timeout=timeout,
                              env=env, creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0))
        record.update(returncode=done.returncode,
                      returncode_hex=f"0x{done.returncode & 0xffffffff:08x}")
        stdout, stderr = done.stdout, done.stderr
    except subprocess.TimeoutExpired as error:
        record["status"] = "TIMEOUT"
        stdout, stderr = error.stdout or b"", error.stderr or b""
    (folder / f"{name}.stdout").write_bytes(stdout)
    (folder / f"{name}.stderr").write_bytes(stderr)
    record.update(stdout_sha256=hashlib.sha256(stdout).hexdigest(),
                  stderr_sha256=hashlib.sha256(stderr).hexdigest())
    write_json(folder / f"{name}.json", record)
    return record, stdout.decode("utf8", errors="replace"), stderr.decode("utf8", errors="replace")


def compile_one(args, build, name, argv):
    record, _, _ = command(args, build, name, [args.compiler, *argv], 180)
    if record.get("returncode") != 0:
        raise RuntimeError(f"build/setup failed: {name}; see its immutable stdout/stderr")


def stage_sources(args, evidence):
    original = (args.source / GLOBAL).read_bytes()
    old, new = b"sorted.reserve(_numberERBBands);", b"sorted.resize(_numberERBBands);"
    assert original.count(old) == 1
    variants = {"raw": original, "storage-sized": original.replace(old, new)}
    result = {}
    for name, content in variants.items():
        folder = evidence / "source" / name
        folder.mkdir(parents=True)
        target = folder / "onsetdetectionglobal.cpp"
        target.write_bytes(content)
        result[name] = {"path": str(target), "sha256": sha(target),
                        "patch": "none" if name == "raw" else "one reserve -> resize; no other repair"}
    return result


def build(args, evidence, sources):
    folder, binaries = evidence / "build", evidence / "bin"
    folder.mkdir(); binaries.mkdir()
    own = Path(__file__).resolve().parent
    fragment = own / "essentia_storage_repro.cpp"
    native_probe = own / "essentia_global_native_probe.cpp"
    base = ["-std=gnu++11", "-O1", "-g", "-DNOMINMAX", "-D_USE_MATH_DEFINES",
            "-DEIGEN_PERMANENTLY_DISABLE_STUPID_WARNINGS"]
    result = {}
    for mode, flag in (("assertions", "-D_GLIBCXX_ASSERTIONS"), ("debug", "-D_GLIBCXX_DEBUG")):
        output = binaries / f"storage-{mode}.exe"
        compile_one(args, folder, f"storage-{mode}", [*base, flag, fragment, "-o", output])
        result[f"storage-{mode}"] = str(output)
    source = args.source / "src"
    includes = [args.native, source, source / "essentia", source / "essentia/scheduler",
                source / "essentia/streaming", source / "essentia/streaming/algorithms",
                source / "essentia/utils", source / "3rdparty", source / "3rdparty/kiss_fft130",
                source / "algorithms/rhythm", args.native / "eigen-3.4.0"]
    flags = [*base, "-D_GLIBCXX_ASSERTIONS", *[f"-I{path}" for path in includes]]
    probe_object = folder / "native-probe.o"
    compile_one(args, folder, "native-probe", [*flags, "-c", native_probe, "-o", probe_object])
    for name, variant in sources.items():
        obj, output = folder / f"global-{name}.o", binaries / f"native-{name}-checked.exe"
        compile_one(args, folder, f"global-{name}", [*flags, "-c", variant["path"], "-o", obj])
        compile_one(args, folder, f"link-{name}", [probe_object, obj,
                    args.native / "build/libessentia_original.a", "-o", output])
        result[f"native-{name}"] = str(output)
    return {name: {"path": path, "sha256": sha(path)} for name, path in result.items()}


def classify(record, stdout, stderr):
    if record.get("status") == "TIMEOUT":
        return "TIMEOUT"
    if record.get("returncode") == 64 or "PROBE_SETUP_ERROR:" in stderr:
        return "SETUP_ERROR"
    if record.get("returncode"):
        checked = re.search(r"assertion|Assertion|attempt to|valid iterator", stderr)
        if checked:
            return "CHECKED_STL_FAILURE"
        if record["returncode_hex"] == "0xc0000005":
            return "NATIVE_ACCESS_VIOLATION"
        if "NATIVE_EXCEPTION:" in stderr:
            return "CPP_EXCEPTION"
        return "NATIVE_EXIT"
    if "REUSE " in stdout:
        return "OUTPUT_PREFILL_AFFECTS_RESULT" if "equal=0" in stdout else "OUTPUT_PREFILL_INDEPENDENT"
    return "COMPLETED"


def run_case(args, evidence, binaries, tag, binary, values, expectation):
    folder = evidence / "cases" / tag
    folder.mkdir(parents=True)
    argv = [binaries[binary]["path"], *values]
    record, stdout, stderr = command(args, folder, "process", argv, args.timeout)
    observed = classify(record, stdout, stderr)
    valid = observed in expectation
    if tag.endswith("327681") and observed == "CPP_EXCEPTION":
        valid = valid and "output buffer is full" in stderr
    if "reuse-" in tag:
        valid = valid and "fresh_instances=1" in stdout and "empty_count=" in stdout
    artifacts = {path.name: {"sha256": sha(path), "bytes": path.stat().st_size}
                 for path in folder.glob("o*.f32")}
    return {"tag": tag, "binary": binary, "binary_sha256": binaries[binary]["sha256"],
            "observed_status": observed, "observation_expected": valid,
            "expectation": expectation, "process": record,
            "numeric_artifacts": artifacts,
            "native_crash_claim": observed == "NATIVE_ACCESS_VIOLATION"}


def cases(args, evidence, binaries, rows):
    for checked in ("assertions", "debug"):
        for storage, pattern in (("raw", "descending"), ("sized", "descending"), ("sized", "ties")):
            tag = f"stl-{checked}-{storage}-{pattern}"
            expected = ["CHECKED_STL_FAILURE"] if storage == "raw" else ["COMPLETED"]
            rows.append(run_case(args, evidence, binaries, tag, f"storage-{checked}", [storage, pattern], expected))
    for variant in ("raw", "storage-sized"):
        tag = f"native-{variant}-bounds"
        prefix = evidence / "cases" / tag / "o"
        expected = ["CHECKED_STL_FAILURE"] if variant == "raw" else ["COMPLETED"]
        rows.append(run_case(args, evidence, binaries, tag, f"native-{variant}", ["bounds", str(prefix)], expected))
    for variant, method in (("raw", "reuse-info"), ("storage-sized", "reuse-info"), ("storage-sized", "reuse-emphasis")):
        tag = f"native-{variant}-{method}"
        prefix = evidence / "cases" / tag / "o"
        rows.append(run_case(args, evidence, binaries, tag, f"native-{variant}", [method, str(prefix)],
                             ["OUTPUT_PREFILL_AFFECTS_RESULT", "OUTPUT_PREFILL_INDEPENDENT"]))
    for count in (327679, 327680, 327681):
        tag = f"native-raw-stream-{count}"
        prefix = evidence / "cases" / tag / "o"
        expected = ["CPP_EXCEPTION"] if count > 327680 else ["COMPLETED"]
        rows.append(run_case(args, evidence, binaries, tag, "native-raw", ["stream", str(prefix), str(count)], expected))
    return rows


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", type=Path, required=True)
    parser.add_argument("--pin-manifest", type=Path, default=Path(__file__).resolve().parents[2] /
                        ".agent-workspace/rhythm-pc-max/upstream-full/essentia-b9fa6cb6/PIN.json")
    parser.add_argument("--native", type=Path, required=True)
    parser.add_argument("--evidence", type=Path, required=True)
    parser.add_argument("--compiler", type=Path, default=Path(
        "C:/Program Files/JetBrains/CLion 2025.3.3/bin/mingw/bin/g++.exe"))
    parser.add_argument("--timeout", type=int, default=90)
    args = parser.parse_args()
    assert os.name == "nt", "this assigned probe is Windows-only"
    assert 1 <= args.timeout <= 180
    args.source, args.native, args.evidence = args.source.resolve(), args.native.resolve(), args.evidence.resolve()
    args.pin_manifest = args.pin_manifest.resolve()
    assert not args.evidence.exists(), "fresh evidence path required; old results are immutable"
    assert len(str(args.evidence)) < 165, "choose a shorter fresh evidence root for native paths"
    info = identity(args)
    args.evidence.mkdir(parents=True)
    report = {"created_utc": datetime.now(timezone.utc).isoformat(), "identity": info,
              "python": sys.version, "complete": False, "variants": {}, "binaries": {}, "cases": [],
              "scope": "Causal checked-source probes, not a new whole-MultiFeature parity claim"}
    target = args.evidence / "report.json"
    try:
        report["variants"] = stage_sources(args, args.evidence)
        report["binaries"] = build(args, args.evidence, report["variants"])
        cases(args, args.evidence, report["binaries"], report["cases"])
        report["complete"] = True
        report["observations_match_protocol"] = all(row["observation_expected"] for row in report["cases"])
    except Exception as error:
        report["failure"] = {"type": type(error).__name__, "message": str(error)}
    finally:
        write_json(target, report)
    print(json.dumps({"report": str(target), "complete": report["complete"],
                      "observations_match_protocol": report.get("observations_match_protocol", False)}))
    return int(not report.get("observations_match_protocol", False))


if __name__ == "__main__":
    raise SystemExit(main())
