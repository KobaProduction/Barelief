"""Live source catalogue mapped to named finite cases, without transitive PASS.

This is a coverage API used by the reference runner. Stage-only matches do not
certify a composition, graph binding, streaming mode, or its parameter domain.
"""
import re
import hashlib


# Prefixes name actual executed cases. Empty tuples mean no same-input case.
# Remaining modes are explicit even where a default offline route was executed.
ROUTES = {
    "SpectralNovelty": (("python.madmom.novelty.",), "twelve default recipes; custom filters/epsilon/silence/batch/reset pending"),
    "OnsetBacktrack": (("python.librosa.remaining.backtrack.",), "standalone ordered/unsorted repeated sparse indices; other configurations/lifecycle pending"),
    "TempoEstimate": (("python.librosa.tempo.", "python.librosa.options.tempo."), "finite default/supplied prior and custom-aggregation cases; other priors/grids/batch pending"),
    "DynamicBeatPath": (("python.librosa.beat.",), "supplied scalar BPM/trim; per-frame BPM/batch/score intermediates pending"),
    "BeatTrackingSequence": (("python.madmom.interval_search.",), "explicit global interval decoder; local graph integration pending"),
    "CrfBeatSequence": (("python.madmom.crf.", "python.madmom.crf_search."), "explicit CRF trials/ranking; graph preprocessing acceptance separate"),
    "BeatStateSequence": (("python.madmom.dbn.", "python.madmom.wrapper.beat_state."), "decoder stage and finite offline/online wrapper chunks/reset/interlude cases; broader settings/induction pending"),
    "DownbeatStateSequence": (("python.madmom.remaining.downbeat.",), "finite supplied two-channel DBN cases; online/chunks/reset/settings pending"),
    "BarStateSequence": (("python.madmom.remaining.bar.",), "finite beat-timed multi-pattern DBN cases; chunks/reset/settings pending"),
    "GaussianMixtureScore": (("python.madmom.gmm.", "python.madmom.options.gmm."), "finite four-covariance scores/responsibilities/components and full/tied retry cases; other sizes/failure/settings pending"),
    "PatternStateSequence": (("python.madmom.pattern.state.",), "one/two supplied diagonal-GMM pattern packet; other covariance/beat-specific transitions pending"),
    "BeatFeatureSynchronization": (("python.madmom.remaining.synchronization.",), "finite subdivision/empty-mean/trim cases; other timing/settings pending"),
    "ModelPredictionSelection": (("python.madmom.remaining.model_selection.",), "finite supplied prediction/reference-count cases; other settings pending"),
    "TempoProbabilityHistogram": (("python.madmom.remaining.tempo_probabilities.",), "finite supplied prediction/range/tuple cases; other settings pending"),
}

# Source presence and numerical coverage are independent columns. These routes
# name the existing prepared kernel and current neutral prepare/assess binding;
# they do not turn an unexecuted source path into numerical acceptance.
SOURCE_ROUTES = {
    "SpectralNovelty": ("spectral_novelty", "PreparedSpectralNovelty", "spectral_sequence_bindings"),
    "OnsetBacktrack": ("onset_peaks", "PreparedOnsetBacktracker", "onset_bindings"),
    "TempoEstimate": ("tempo_estimator", "PreparedTempoEstimator", "tempo_bindings"),
    "DynamicBeatPath": ("beat_tracker", "PreparedBeatTracker", "beat_pulse_bindings"),
    "BeatTrackingSequence": ("local_interval_beat_search", "PreparedLocalIntervalBeatSearch", "histogram_bindings"),
    "CrfBeatSequence": ("interval_sequence_search", "PreparedIntervalSequenceSearch", "histogram_bindings"),
    "BeatStateSequence": ("state_beat_sequence", "PreparedStateBeatSequence", "state_bindings"),
    "DownbeatStateSequence": ("downbeat_state_decoder", "PreparedDownbeatStateDecoder", "state_bindings"),
    "BarStateSequence": ("bar_state_decoder", "PreparedBarStateDecoder", "state_bindings"),
    "GaussianMixtureScore": ("gaussian_mixture", "PreparedGaussianMixture", "model_bindings"),
    "PatternStateSequence": ("pattern_state_decoder", "PreparedPatternStateDecoder", "model_bindings"),
    "BeatFeatureSynchronization": ("beat_feature_synchronization", "PreparedBeatFeatureSynchronization", "model_bindings"),
    "ModelPredictionSelection": ("model_prediction_selection", "PreparedModelPredictionSelection", "model_bindings"),
    "TempoProbabilityHistogram": ("tempo_probability_histogram", "tempo_probability_range", "model_bindings"),
}


def source_route(repository, kind):
    kernel, symbol, binding = SOURCE_ROUTES[kind]
    paths = {"kernel_header": repository / f"libs/dsp_kernels/include/dsp_kernels/{kernel}.h",
             "kernel_source": repository / f"libs/dsp_kernels/src/{kernel}.cpp",
             "adapter_source": repository / f"libs/dsp_graph_adapters/src/{binding}.cpp"}
    assert symbol in paths["kernel_header"].read_text(), symbol
    assert kind in paths["adapter_source"].read_text(), kind
    return {"status": "source_present_numerical_acceptance_separate", "symbol": symbol,
            "files": {name: {"path": str(path), "sha256": hashlib.sha256(path.read_bytes()).hexdigest()}
                      for name, path in paths.items()}}


def catalogue(repository, results):
    path = repository / "domain/src/numeric/sequence_operation_contracts.cpp"
    source = path.read_text(encoding="utf8").split("using C=", 1)[0]
    kinds = re.findall(r'\{"([A-Za-z]+)",F::', source)
    assert set(kinds) == set(ROUTES), "source catalogue changed; classify new/removed routes explicitly"
    by_id = {case["case_id"]: case for case in results}
    rows = []
    for kind in kinds:
        prefixes, remaining = ROUTES[kind]
        matched = [name for name in by_id if any(name.startswith(prefix) for prefix in prefixes)]
        rows.append({"kind": kind, "case_ids": matched,
                     "source_route": source_route(repository, kind),
                     "status": "partial_named_cases" if matched else "pending",
                     "case_statuses": {name: by_id[name]["status"] for name in matched},
                     "remaining": remaining})
    return {"source": str(path), "source_sha256": hashlib.sha256(path.read_bytes()).hexdigest(),
            "catalogue_size": len(kinds), "operations": rows,
            "policy": "named stage/subset coverage only; no composition, all-mode, or graph-binding equivalence inferred"}


def main():
    import argparse
    import json
    from pathlib import Path
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repository", type=Path, default=Path(__file__).resolve().parents[3])
    parser.add_argument("--results", type=Path, nargs="+", required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    cases = [case for path in args.results for case in json.loads(path.read_text())["cases"]]
    report = catalogue(args.repository, cases)
    report["evidence_manifests"] = list(map(str, args.results))
    args.output.write_text(json.dumps(report, indent=2) + "\n")
    print(json.dumps({"catalogue_size": report["catalogue_size"],
                      "pending": sum(row["status"] == "pending" for row in report["operations"])}))


if __name__ == "__main__":
    main()
