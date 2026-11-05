"""Compare new executor evidence with actual pinned native Essentia wrappers.

Raw original and storage-corrected-global runs are always separate. This driver
does not change an original, synthesize source results, or reuse an old ledger.
"""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import sys

import numpy as np

sys.path.insert(0, str(Path(__file__).parents[1] / "tools" / "rhythm_originals"))
from native_compare import POLICY, compare

PIN = "b9fa6cb674ca43dfb94d28d293aeda441c6745db"


def digest(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def verify_sources(root):
    pin = json.loads((root / "PIN.json").read_text(encoding="utf8"))
    assert pin["revision"] == PIN
    files = ("rhythmextractor2013", "beattrackerdegara", "beattrackermultifeature",
             "tempotapdegara", "tempotapmaxagreement", "onsetdetection", "onsetdetectionglobal")
    result = {}
    for stem in files:
        name = f"src/algorithms/rhythm/{stem}.cpp"
        actual = digest(root / name)
        assert actual == pin["sha256"][name], name
        result[name] = actual
    return result


def run(binary, wrapper, method, case, family, variant, samples):
    # Narrow prefixes preserve the existing original wrapper's MAX_PATH limit.
    folder = case / variant / family
    folder.mkdir(parents=True, exist_ok=False)
    prefix = folder / "out"
    command = [str(binary), method, str(case / "pcm.f32"), str(prefix), "44100",
               str(samples) if family == "summary" else "2048",
               "1" if family == "summary" else "1024", "17"]
    record = {"binary_sha256": digest(binary), "wrapper_sha256": digest(wrapper),
              "arguments": command[1:], "variant": variant, "revision": PIN}
    try:
        completed = subprocess.run(command, capture_output=True, text=True, timeout=180)
        record.update(returncode=completed.returncode, stdout=completed.stdout, stderr=completed.stderr)
    except subprocess.TimeoutExpired as error:
        record.update(status="ORIGINAL_TIMEOUT", detail=str(error))
        return record
    if completed.returncode:
        record["status"] = "ORIGINAL_REJECT"
        return record
    fields = ["bpm", "confidence", "ticks", "intervals", "estimates"] if family == "summary" else (
        ["ticks"] + (["confidence"] if method == "multi" else []) +
        [f"{name}{i}" for i in range(5 if method == "multi" else 1) for name in ("curve", "candidate")])
    checks = {}
    for name in fields:
        actual_file, original_file = case / f"{name}.f32", Path(f"{prefix}.{name}.f32")
        try:
            actual = np.fromfile(actual_file, dtype="<f4")
            expected = np.fromfile(original_file, dtype="<f4")
            checks[name] = {**compare(name, actual, expected, None),
                            "graph_sha256": digest(actual_file), "original_sha256": digest(original_file)}
        except (OSError, ValueError) as error:
            checks[name] = {"pass": False, "error": str(error)}
    record.update(comparisons=checks, status="PASS" if all(v["pass"] for v in checks.values()) else "STRICT_FAIL")
    return record


def validate_recipe(case, recipe):
    assert recipe["revision"] == PIN and recipe["seed"] == 17
    assert recipe["sample_rate"] == 44100 and recipe["frame"] == 2048
    assert recipe["primary_hop"] == 1024 and recipe["global_hop"] == 512
    assert recipe["frame_tail"] == "CoverEnd"
    assert recipe["method"] in ("multifeature", "degara")
    samples = recipe["session_samples"]
    assert isinstance(samples, int) and 0 < samples <= 2147483647
    assert (case / "pcm.f32").stat().st_size == samples * 4, "actual PCM size disagrees with recipe"
    assert np.isfinite(np.fromfile(case / "pcm.f32", dtype="<f4")).all(), "nonfinite PCM"
    return samples


def agreement_observation(case, required):
    names = ("scores.f32", "selected.u64", "pairwise.f32")
    if not required and not any((case / name).exists() for name in names):
        return {"status": "NOT_MEASURED", "scope": "optional internal agreement diagnostics"}
    try:
        scores = np.fromfile(case / names[0], dtype="<f4")
        selected = np.fromfile(case / names[1], dtype="<u8")
        pairs = np.fromfile(case / names[2], dtype="<f4")
        assert scores.size == 5 and selected.size == 1 and pairs.size == 25
        assert np.isfinite(scores).all() and np.isfinite(pairs).all()
        index = int(selected[0])
        assert index == int(np.argmax(scores)), "first maximum selection differs"
        pairs = pairs.reshape(5, 5)
        assert np.array_equal(pairs, pairs.T) and not np.any(np.diag(pairs))
        ticks = np.fromfile(case / "ticks.f32", dtype="<f4")
        candidate = np.fromfile(case / f"candidate{index}.f32", dtype="<f4")
        assert np.array_equal(ticks, candidate), "selection must return the full original candidate"
        return {"status": "CONSISTENT", "selected": index,
                "scope": "graph diagnostics consistency; native wrappers compare candidates/ticks/confidence separately",
                "artifacts": {name: digest(case / name) for name in names}}
    except (OSError, ValueError, AssertionError) as error:
        return {"status": "STRICT_FAIL", "error": str(error)}


def defined_command(manifest, family):
    binary = manifest["binaries"][family]["path"]
    commands = [row for row in manifest["commands"]
                if "-o" in row["argv"] and row["argv"][row["argv"].index("-o") + 1] == binary]
    assert len(commands) == 1 and commands[0].get("returncode") == 0
    argv = commands[0]["argv"]
    assert argv[0] == manifest["identity"]["compiler"]
    assert manifest["defined_object"]["path"] in argv and manifest["identity"]["library"] in argv
    assert manifest["wrappers"][family]["path"] in argv
    assert argv.index(manifest["defined_object"]["path"]) < argv.index(manifest["identity"]["library"])


def canonical_text_proof(path, expected):
    content = Path(path).read_bytes()
    raw = hashlib.sha256(content).hexdigest()
    normalized = hashlib.sha256(content.replace(b"\r\n", b"\n")).hexdigest()
    assert raw == expected or normalized == expected, str(path)
    return {"path": str(path), "raw_sha256": raw, "lf_sha256": normalized,
            "expected_sha256": expected, "crlf_to_lf_applied": raw != expected}


def defined_authority(args, manifest):
    builder = "f55e028d6b7e72f45c25927948931093b32637b2aeebd3e3b6402913b2a5a5c3"
    assert manifest["script_sha256"] == builder
    builder_proof = canonical_text_proof(Path(__file__).with_name("essentia_defined_wrappers.py"), builder)
    repair_path = Path(manifest["repair_report"]["path"])
    assert digest(repair_path) == "a7a538fa8b5a2b123cd4826f3705607c44c90470263480335d512597b6d232e3"
    repair = json.loads(repair_path.read_text(encoding="utf8"))["identity"]
    for field in ("library_sha256", "compiler_sha256", "source_hashes", "revision", "pin_manifest_sha256"):
        assert manifest["identity"][field] == repair[field], field
    pin_path = args.source / "PIN.json"
    assert digest(pin_path) == repair["pin_manifest_sha256"]
    pin = json.loads(pin_path.read_text(encoding="utf8"))
    extras = ("src/algorithms/rhythm/onsetrate.cpp", "src/algorithms/rhythm/rhythmtransform.cpp",
              "src/algorithms/extractor/rhythmdescriptors.cpp")
    assert len(manifest["extra_sources"]) == len(extras)
    for name, proof in zip(extras, manifest["extra_sources"]):
        assert proof["sha256"] == pin["sha256"][name], name
    canonical = {"tempo": "72cf37e8c0f2db540d1811d7ff34f2c2bc461eb18cdc15fe02f532854c94ab26",
                 "summary": "a29641e0e269305d24fca2f3cb6d99c74e52b43c762068ae831dac31f2aa54ea"}
    for family, expected in canonical.items():
        assert manifest["wrappers"][family]["sha256"] == expected, family
        assert manifest["wrappers"][family]["canonical"]["sha256"] == expected, family
    return builder_proof


def defined_lineage(args):
    assert args.safe_build_manifest is not None, "defined-global requires its owner-built manifest"
    path = args.safe_build_manifest.resolve()
    manifest = json.loads(path.read_text(encoding="utf8"))
    assert manifest["complete"] and manifest["variant"] == "defined-global"
    assert manifest["identity"]["revision"] == PIN
    assert len(manifest["commands"]) == 2
    assert manifest["repair_report"]["sha256"] == "a7a538fa8b5a2b123cd4826f3705607c44c90470263480335d512597b6d232e3"
    assert manifest["defined_object"]["sha256"] == "7e42d172ebb802ad7425d09c6f87cf76bb52722aaae158698ee90b00dd130409"
    builder_proof = defined_authority(args, manifest)
    proofs = [manifest["repair_report"], manifest["defined_source"], manifest["defined_object"],
              *manifest["extra_sources"]]
    assert manifest["defined_source"]["sha256"] == "557b22ea5df429003c0dfe035541cd379faececd73e88983e227b63502b07c67"
    for family in ("tempo", "summary"):
        binary, wrapper = manifest["binaries"][family], manifest["wrappers"][family]
        assert getattr(args, f"safe_{family}").resolve() == Path(binary["path"]).resolve()
        assert getattr(args, f"safe_{family}_wrapper").resolve() == Path(wrapper["path"]).resolve()
        assert wrapper["sha256"] == wrapper["canonical"]["sha256"]
        defined_command(manifest, family)
        proofs.extend((binary, wrapper, wrapper["canonical"]))
    for proof in proofs:
        assert digest(proof["path"]) == proof["sha256"], proof["path"]
    assert digest(manifest["identity"]["library"]) == manifest["identity"]["library_sha256"]
    assert digest(manifest["identity"]["compiler"]) == manifest["identity"]["compiler_sha256"]
    return {"path": str(path), "sha256": digest(path), "variant": manifest["variant"],
            "defined_source": manifest["defined_source"], "scope": manifest["scope"],
            "builder_source": builder_proof}


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("evidence", type=Path)
    parser.add_argument("--source", type=Path, required=True)
    parser.add_argument("--require-full-agreement", action="store_true",
                        help="require scores.f32, selected.u64 and pairwise.f32 for whole-input MultiFeature cases")
    parser.add_argument("--safe-variant", choices=("storage-corrected-global", "defined-global"),
                        default="storage-corrected-global")
    parser.add_argument("--safe-build-manifest", type=Path)
    for name in ("tempo", "summary"):
        parser.add_argument(f"--original-{name}", type=Path, required=True)
        parser.add_argument(f"--original-{name}-wrapper", type=Path, required=True)
        parser.add_argument(f"--safe-{name}", type=Path)
        parser.add_argument(f"--safe-{name}-wrapper", type=Path)
    args = parser.parse_args()
    report = {"source_hashes": verify_sources(args.source), "comparison_policy": POLICY,
              "coverage": "independent complete graph sessions with each actual PCM length declared in recipe.json; only listed cases",
              "runs": []}
    if args.safe_variant == "defined-global":
        assert all(getattr(args, f"safe_{name}{suffix}") is not None
                   for name in ("tempo", "summary") for suffix in ("", "_wrapper"))
        report["safe_build_lineage"] = defined_lineage(args)
    for case in sorted(args.evidence.iterdir()):
        if not (case / "recipe.json").exists():
            continue
        recipe = json.loads((case / "recipe.json").read_text())
        samples = validate_recipe(case, recipe)
        report["runs"].append({"case": case.name, "input_sha256": digest(case / "pcm.f32"), "recipe": recipe})
        row = report["runs"][-1]
        if recipe["method"] == "multifeature":
            row["agreement_diagnostics"] = agreement_observation(case, args.require_full_agreement)
        for family in ("tempo", "summary"):
            method = ("multi" if recipe["method"] == "multifeature" else "single")
            if family == "summary":
                method = "summary_" + method
            for variant, key in (("raw-original", "original"), (args.safe_variant, "safe")):
                binary = getattr(args, f"{key}_{family}")
                wrapper = getattr(args, f"{key}_{family}_wrapper")
                if binary is not None:
                    if wrapper is None:
                        parser.error(f"--{key}-{family}-wrapper required with binary")
                    row[f"{variant}/{family}"] = run(binary, wrapper, method, case, family, variant, samples)
    assert report["runs"], "no graph evidence cases"
    output = args.evidence / "native-original-comparison.json"
    assert not output.exists(), "preserve previous immutable comparison; select a fresh evidence directory"
    output.write_text(json.dumps(report, indent=2), encoding="utf8")
    statuses = [value["status"] for row in report["runs"] for value in row.values()
                if isinstance(value, dict) and "status" in value]
    print(json.dumps({"statuses": statuses, "report": str(output)}))
    return int(any(status not in ("PASS", "CONSISTENT", "NOT_MEASURED") for status in statuses))


if __name__ == "__main__":
    raise SystemExit(main())
