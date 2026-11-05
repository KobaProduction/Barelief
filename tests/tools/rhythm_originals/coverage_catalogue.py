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
    "PcmSpectralNovelty": (("python.madmom.remaining.pcm_novelty.",), "finite PCM source frontend/novelty cases; custom filters/log/chunks/reset pending"),
    "ActivationPeakMatrix": (("python.madmom.peaks.",), "offline average/max cases; smoothing kernels/online/batch pending"),
    "OnsetStrengthSequence": (("python.librosa.strength.", "python.librosa.options.strength."), "finite scalar/groups/reference/custom-reducer cases; other groups/batch/chunks pending"),
    "OnsetDetection": (("python.librosa.detect.", "python.librosa.options.detection."), "finite sparse/dense/units/custom-picker cases; broader peaks/batch/lifecycle pending"),
    "OnsetBacktrack": (("python.librosa.remaining.backtrack.",), "standalone ordered/unsorted repeated sparse indices; other configurations/lifecycle pending"),
    "PcmOnsetEnvelope": (("python.librosa.pcm_strength.",), "mean/median; grouped features/supplied window/batch pending"),
    "TempoEstimate": (("python.librosa.tempo.", "python.librosa.options.tempo."), "finite default/supplied prior and custom-aggregation cases; other priors/grids/batch pending"),
    "TempoRatios": (("python.librosa.ratios.", "python.librosa.options.ratios."), "finite scalar/per-frame/estimated BPM, dynamic frequencies, prior/custom reducer; other grids/batch/lifecycle pending"),
    "MetricalTempogram": (("python.librosa.composition.metrogram.",), "fixed grids/reducers packet; dynamic frequencies/custom reducer pending"),
    "EnvelopeTempo": (("python.librosa.composition.envelope_tempo.",), "default prior packet; supplied prior/batch pending"),
    "EnvelopeTempoRatios": (("python.librosa.composition.envelope_ratios.",), "scalar/estimated BPM packet; prior/dynamic frequencies/batch pending"),
    "PcmTempo": (("python.librosa.composition.pcm_tempo.",), "default PCM/tempo packet; supplied prior/batch pending"),
    "PcmTempogram": (("python.librosa.composition.pcm_tempogram.",), "default PCM/ACF/Fourier/hybrid packet; custom windows/batch pending"),
    "PcmTempoRatios": (("python.librosa.composition.pcm_ratios.",), "default PCM/scalar/estimated BPM packet; dynamic frequencies/prior/batch pending"),
    "DynamicBeatPath": (("python.librosa.beat.",), "supplied scalar BPM/trim; per-frame BPM/batch/score intermediates pending"),
    "EnvelopeBeatPath": (("python.librosa.composition.envelope_beats.", "python.librosa.options.beats."), "finite estimated/scalar/per-frame BPM cases; supplied prior/batch/chunks pending"),
    "PcmBeatPath": (("python.librosa.composition.pcm_beats.",), "estimated/scalar BPM packet; per-frame BPM/prior/batch pending"),
    "LocalPulse": (("python.librosa.remaining.local_pulse.", "python.librosa.options.pulse."), "finite explicit Complex64 rows and static/per-row prior cases; other windows/batch/chunks pending"),
    "EnvelopeLocalPulse": (("python.librosa.plp",), "default offline pulse; supplied prior/rows/batch pending"),
    "PcmLocalPulse": (("python.librosa.composition.pcm_pulse",), "default PCM PLP packet; prior/rows/batch pending"),
    "TempoHistogram": (("python.madmom.histogram.",), "offline ACF/comb; online/state-model/vector-alpha pending; literal 3-channel defect retained"),
    "TempoAnalysis": (("python.madmom.remaining.analysis.", "python.madmom.lifecycle.analysis."), "finite ACF/comb/DBN offline/online processor cases and ACF/comb history4096/chunk-induction/reset cases; broader lifecycle/settings pending"),
    "BeatTrackingSequence": (("python.madmom.remaining.interval_tracking.",), "finite global/local interval search compositions; induction/chunks/reset/settings pending"),
    "CrfBeatSequence": (("python.madmom.crf.", "python.madmom.wrapper.crf."), "best_sequence stage and finite full-wrapper trials/ranking cases; broader settings/induction pending"),
    "BeatStateSequence": (("python.madmom.dbn.", "python.madmom.wrapper.beat_state."), "decoder stage and finite offline/online wrapper chunks/reset/interlude cases; broader settings/induction pending"),
    "DownbeatStateSequence": (("python.madmom.remaining.downbeat.",), "finite supplied two-channel DBN cases; online/chunks/reset/settings pending"),
    "BarStateSequence": (("python.madmom.remaining.bar.",), "finite beat-timed multi-pattern DBN cases; chunks/reset/settings pending"),
    "GaussianMixtureScore": (("python.madmom.gmm.", "python.madmom.options.gmm."), "finite four-covariance scores/responsibilities/components and full/tied retry cases; other sizes/failure/settings pending"),
    "PatternStateSequence": (("python.madmom.pattern.state.",), "one/two supplied diagonal-GMM pattern packet; other covariance/beat-specific transitions pending"),
    "PatternFeatures": (("python.madmom.pattern.features.",), "fixed original frontend tones/silence packet; explicit lifecycle/chunks pending"),
    "PcmPatternSequence": (("python.madmom.pattern.pcm.",), "PCM + one/two supplied-GMM pattern packet; other model modes pending"),
    "BeatFeatureSynchronization": (("python.madmom.remaining.synchronization.",), "finite subdivision/empty-mean/trim cases; other timing/settings pending"),
    "ModelPredictionSelection": (("python.madmom.remaining.model_selection.",), "finite supplied prediction/reference-count cases; other settings pending"),
    "TempoProbabilityHistogram": (("python.madmom.remaining.tempo_probabilities.",), "finite supplied prediction/range/tuple cases; other settings pending"),
}

# Source presence and numerical coverage are independent columns. These routes
# name the existing prepared kernel and current neutral prepare/assess binding;
# they do not turn an unexecuted source path into numerical acceptance.
SOURCE_ROUTES = {
    "SpectralNovelty": ("spectral_novelty", "PreparedSpectralNovelty", "spectral_sequence_bindings"),
    "PcmSpectralNovelty": ("pcm_spectral_novelty", "PreparedPcmSpectralNovelty", "spectral_sequence_bindings"),
    "ActivationPeakMatrix": ("activation_peak_matrix", "PreparedActivationPeakMatrix", "spectral_sequence_bindings"),
    "OnsetStrengthSequence": ("onset_strength_sequence", "PreparedOnsetStrengthSequence", "onset_bindings"),
    "OnsetDetection": ("onset_detection_sequence", "PreparedOnsetDetectionSequence", "onset_bindings"),
    "OnsetBacktrack": ("onset_peaks", "PreparedOnsetBacktracker", "onset_bindings"),
    "PcmOnsetEnvelope": ("pcm_onset_envelope", "PreparedPcmOnsetEnvelope", "pcm_bindings"),
    "TempoEstimate": ("tempo_estimator", "PreparedTempoEstimator", "tempo_bindings"),
    "TempoRatios": ("tempo_ratio_sequence", "PreparedTempoRatioSequence", "tempo_ratio_bindings"),
    "MetricalTempogram": ("metrical_tempogram_sequence", "PreparedMetricalTempogramSequence", "tempo_ratio_bindings"),
    "EnvelopeTempo": ("envelope_tempo", "PreparedEnvelopeTempo", "tempo_bindings"),
    "EnvelopeTempoRatios": ("envelope_tempo_ratios", "PreparedEnvelopeTempoRatios", "tempo_ratio_bindings"),
    "PcmTempo": ("pcm_tempo", "PreparedPcmTempo", "pcm_bindings"),
    "PcmTempogram": ("pcm_tempo", "PreparedPcmTempogram", "pcm_bindings"),
    "PcmTempoRatios": ("envelope_tempo_ratios", "PreparedPcmTempoRatios", "pcm_bindings"),
    "DynamicBeatPath": ("beat_tracker", "PreparedBeatTracker", "beat_pulse_bindings"),
    "EnvelopeBeatPath": ("envelope_beat_tracking", "PreparedEnvelopeBeatTracking", "beat_pulse_bindings"),
    "PcmBeatPath": ("pcm_beat_tracking", "PreparedPcmBeatTracking", "pcm_bindings"),
    "LocalPulse": ("local_pulse", "PreparedLocalPulse", "beat_pulse_bindings"),
    "EnvelopeLocalPulse": ("envelope_local_pulse", "PreparedEnvelopeLocalPulse", "beat_pulse_bindings"),
    "PcmLocalPulse": ("pcm_beat_tracking", "PreparedPcmLocalPulse", "pcm_bindings"),
    "TempoHistogram": ("tempo_histogram_sequence", "PreparedTempoHistogramSequence", "histogram_bindings"),
    "TempoAnalysis": ("tempo_analysis_sequence", "PreparedTempoAnalysisSequence", "histogram_bindings"),
    "BeatTrackingSequence": ("beat_tracking_sequence", "PreparedBeatTrackingSequence", "histogram_bindings"),
    "CrfBeatSequence": ("crf_beat_sequence", "PreparedCrfBeatSequence", "histogram_bindings"),
    "BeatStateSequence": ("state_beat_sequence", "PreparedStateBeatSequence", "state_bindings"),
    "DownbeatStateSequence": ("downbeat_state_decoder", "PreparedDownbeatStateDecoder", "state_bindings"),
    "BarStateSequence": ("bar_state_decoder", "PreparedBarStateDecoder", "state_bindings"),
    "GaussianMixtureScore": ("gaussian_mixture", "PreparedGaussianMixture", "model_bindings"),
    "PatternStateSequence": ("pattern_state_decoder", "PreparedPatternStateDecoder", "model_bindings"),
    "PatternFeatures": ("pattern_feature_recipe", "PreparedPatternFeatureRecipe", "model_bindings"),
    "PcmPatternSequence": ("pcm_pattern_recipe", "PreparedPcmPatternRecipe", "model_bindings"),
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
