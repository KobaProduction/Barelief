"""Finite actual-original comparison; a passing subset never means all-five coverage."""
import argparse
import hashlib
import importlib.metadata
import json
import os
from pathlib import Path
import platform
import sys
import traceback

from common import run_case
import original_sources


def case_directory(root: Path, case_id: str, compact: bool) -> Path:
    if not compact:
        return root / case_id
    digest = hashlib.sha256(case_id.encode("utf8")).hexdigest()
    directory = root / digest[:16]
    directory.mkdir(parents=True, exist_ok=True)
    identity = directory / "case-identity.json"
    record = {"case_id": case_id, "case_id_sha256": digest}
    if identity.exists():
        if json.loads(identity.read_text()) != record:
            raise RuntimeError(f"compact fixture path collision: {directory}")
    else:
        identity.write_text(json.dumps(record, indent=2) + "\n")
    return directory


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--library", choices=("librosa", "madmom"), required=True)
    parser.add_argument("--binary", type=Path, required=True)
    parser.add_argument("--source-cache", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--filter", default="")
    parser.add_argument("--exclude-filter", default="")
    parser.add_argument("--packet", choices=("base", "compositions", "precision-holdouts", "librosa-full", "madmom-full", "remaining", "wrappers", "options", "lifecycle", "representatives"), default="base")
    parser.add_argument("--precision", choices=("portable-f32", "desktop-reference"), default="portable-f32")
    parser.add_argument("--reference-cache", type=Path)
    parser.add_argument("--cache-integrity", type=Path)
    parser.add_argument("--fresh-on-cache-miss", action="store_true")
    parser.add_argument("--only-failed", type=Path)
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    compact_paths = args.packet in ("lifecycle", "representatives") or os.environ.get("BARELIEF_DSP_BENCHMARK") == "1"
    environment = {"binary": str(args.binary.resolve()),
                   "binary_sha256": hashlib.sha256(args.binary.read_bytes()).hexdigest(),
                   "fixture_directory_policy": "case-id sha256 prefix16 with full-hash/exact-ID guard" if compact_paths else "full case ID",
                   "thread_limits": {name: os.environ.get(name) for name in
                                     ("OPENBLAS_NUM_THREADS", "OMP_NUM_THREADS", "MKL_NUM_THREADS", "NUMBA_NUM_THREADS")}}
    (args.output / "execution-environment.json").write_text(json.dumps(environment, indent=2) + "\n")
    original = getattr(original_sources, args.library)(args.source_cache)
    if args.packet == "representatives":
        import representative_cases
        cases = list(representative_cases.cases(original, args.library, args.precision))
    elif args.packet == "lifecycle":
        assert args.library == "madmom"
        import long_history_cases
        cases = list(long_history_cases.cases(original))
    elif args.packet == "options":
        if args.library == "librosa":
            import options_onset_cases
            import options_tempo_cases
            cases = [*options_onset_cases.cases(original), *options_tempo_cases.cases(original)]
        else:
            import options_gmm_cases
            cases = list(options_gmm_cases.cases(original))
    elif args.packet == "wrappers":
        assert args.library == "madmom"
        import wrapper_state_cases
        cases = list(wrapper_state_cases.cases(original))
    elif args.packet == "remaining":
        if args.library == "librosa":
            import remaining_librosa_cases
            cases = list(remaining_librosa_cases.cases(original))
        else:
            import remaining_madmom_cases
            import remaining_state_cases
            cases = [*remaining_madmom_cases.cases(original), *remaining_state_cases.cases(original)]
    elif args.packet == "madmom-full":
        assert args.library == "madmom"
        import madmom_cases
        import model_cases
        import pattern_cases
        cases = [*madmom_cases.cases(original), *model_cases.cases(original), *pattern_cases.cases(original)]
    elif args.packet == "librosa-full":
        assert args.library == "librosa"
        import librosa_cases
        import composition_cases
        cases = [*librosa_cases.cases(original), *composition_cases.cases(original)]
    elif args.packet == "precision-holdouts":
        assert args.library == "librosa", "precision holdouts use pinned librosa"
        import precision_cases
        cases = list(precision_cases.cases(original))
    elif args.packet == "compositions":
        if args.library == "librosa":
            import composition_cases
            cases = list(composition_cases.cases(original))
        else:
            import pattern_cases
            cases = list(pattern_cases.cases(original))
    elif args.library == "librosa":
        import librosa_cases
        cases = list(librosa_cases.cases(original))
    else:
        import madmom_cases
        import model_cases
        import comb_extension_cases
        cases = [*madmom_cases.cases(original), *model_cases.cases(original),
                 *comb_extension_cases.cases(original)]
    source = original_sources.source_record(args.library)
    integrity = json.loads(args.cache_integrity.read_text()) if args.cache_integrity else None
    selected = {value["case_id"] for value in json.loads(args.only_failed.read_text())["cases"]
                if value["status"] == "failed"} if args.only_failed else None
    results = []
    for case in cases:
        if args.filter and args.filter not in case.name:
            continue
        if args.exclude_filter and args.exclude_filter in case.name:
            continue
        if selected is not None and case.name not in selected:
            continue
        base_name = case.name
        if args.precision == "desktop-reference":
            configured = (case.family == "spectrum" or
                          (case.family == "librosa" and case.method in ("tempogram", "pcm_strength")) or
                          (case.family == "composition" and case.method in
                           ("envelope_tempo", "envelope_ratios", "envelope_beats", "pcm_tempo",
                            "pcm_tempogram", "pcm_ratios", "pcm_beats", "pcm_pulse")) or
                          (case.family == "remaining" and case.method == "pcm_novelty"))
            if configured:
                case.arguments = [*case.arguments, 1]
            case.settings = {**case.settings, "precision": "DesktopReference" if configured else "SourceFloat32",
                             "public_dtype": "Float32"}
            case.name += ".desktop_reference"
        directory = case_directory(args.output, case.name, compact_paths)
        host_recursion_limit = sys.getrecursionlimit()
        try:
            cache = args.reference_cache / base_name if args.reference_cache is not None else None
            if cache is not None and args.packet in ("librosa-full", "madmom-full"):
                traced = args.reference_cache.parent / (args.library + "-recheck1") / base_name
                if traced.exists():
                    cache = traced
            if cache is not None and not cache.exists() and args.packet == "librosa-full":
                cache = args.reference_cache.parent / "librosa-compositions" / base_name
            if cache is not None and not cache.exists() and args.packet == "madmom-full":
                cache = args.reference_cache.parent / "madmom-compositions" / base_name
            result = run_case(case, args.binary.resolve(), directory, source, cache, integrity, args.fresh_on_cache_miss)
            print(result["status"].upper(), case.name, flush=True)
        except Exception as error:
            result = {"case_id": case.name, "status": "failed", "evidence_category": case.evidence_category,
                      "error": str(error), "traceback": traceback.format_exc()}
            if base_name == "python.madmom.histogram.method1.channels3":
                result["known_reference_issue"] = "literal pinned 2D comb loops range(2); separate per-channel-original-1D variant validated; failure retained"
            directory.mkdir(parents=True, exist_ok=True)
            (directory / "failure.txt").write_text(result["traceback"])
            print("FAIL", case.name, str(error).splitlines()[0], flush=True)
        finally:
            if args.packet == "representatives":
                original_recursion_limit = sys.getrecursionlimit()
                sys.setrecursionlimit(host_recursion_limit)
                (directory / "host-state.json").write_text(json.dumps({"recursion_limit_before_case": host_recursion_limit,
                    "recursion_limit_after_case": original_recursion_limit, "recursion_limit_restored": sys.getrecursionlimit()}, indent=2) + "\n")
        result["fixture_directory"] = str(directory.resolve())
        results.append(result)
    dependencies = ("numpy", "scipy", "librosa", "numba") if args.library == "librosa" else ("numpy", "scipy", "Cython", "mido")
    report = {"format": "barelief-original-results-v1", "library": args.library, "packet": args.packet,
              "precision": args.precision, "source": source,
              "platform": platform.platform(), "python": platform.python_version(),
              "dependencies": {name: importlib.metadata.version(name) for name in dependencies},
              "passed": sum(value["status"] == "passed" for value in results),
              "failed": sum(value["status"] == "failed" for value in results), "cases": results,
              "pending": sum(value["status"].startswith("pending") for value in results),
              "scope": "finite cases named in this manifest; no claim for unlisted algorithms, pretrained NN, or whole-library equivalence"}
    (args.output / "results.json").write_text(json.dumps(report, indent=2) + "\n")
    print(json.dumps({key: report[key] for key in ("library", "passed", "failed", "pending")}), flush=True)
    return bool(report["failed"])


if __name__ == "__main__":
    raise SystemExit(main())
