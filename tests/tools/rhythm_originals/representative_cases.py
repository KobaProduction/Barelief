"""Finite whole-operation cost representatives, reusing canonical source cases."""
import importlib

import numpy as np

from common import Case, SEED, tones
from librosa_cases import split_complex


# Stable operation kind -> existing exact same-input/source/settings case.
REPRESENTATIVES = {
    "SpectralNovelty": "python.madmom.novelty.spectral_flux.lag1",
    "PcmSpectralNovelty": "python.madmom.remaining.pcm_novelty.phase_deviation",
    "ActivationPeakMatrix": "python.madmom.peaks.channels3.avg2.max3",
    "OnsetStrengthSequence": "python.librosa.options.strength.boundary_mean",
    "OnsetDetection": "python.librosa.options.detection.dense_default",
    "OnsetBacktrack": "python.librosa.remaining.backtrack.unsorted_repeated",
    "PcmOnsetEnvelope": "python.librosa.pcm_strength.a0.max1",
    "TempoEstimate": "python.librosa.options.tempo.prior.custom_sum0",
    "TempoRatios": "python.librosa.options.ratios.bpm_and_frequency_rows",
    "MetricalTempogram": "python.librosa.composition.metrogram.reverse0.aggregate1",
    "EnvelopeTempo": "python.librosa.composition.envelope_tempo.aggregate0",
    "EnvelopeTempoRatios": "python.librosa.composition.envelope_ratios.supplied0",
    "PcmTempo": "python.librosa.composition.pcm_tempo.aggregate0",
    "PcmTempogram": "python.librosa.composition.pcm_tempogram.method2",
    "PcmTempoRatios": "python.librosa.composition.pcm_ratios.supplied0",
    "DynamicBeatPath": "python.librosa.beat.bpm120.trim1",
    "EnvelopeBeatPath": "python.librosa.composition.envelope_beats.supplied0.trim1",
    "PcmBeatPath": "python.librosa.composition.pcm_beats.supplied0",
    "LocalPulse": "python.librosa.options.pulse.prior_rows1",
    "EnvelopeLocalPulse": "python.librosa.plp",
    "PcmLocalPulse": "python.librosa.composition.pcm_pulse",
    "TempoHistogram": "python.madmom.histogram.method1.channels1",
    "TempoAnalysis": "python.madmom.lifecycle.analysis.comb.history4096",
    "BeatTrackingSequence": "python.madmom.remaining.interval_tracking.variable1",
    "CrfBeatSequence": "python.madmom.wrapper.crf.factors0.top3.sigma0.18",
    "BeatStateSequence": "python.madmom.wrapper.beat_state.online.two_chunks",
    "DownbeatStateSequence": "python.madmom.remaining.downbeat.correct1.threshold0.05",
    "BarStateSequence": "python.madmom.remaining.bar.switch0.1",
    "GaussianMixtureScore": "python.madmom.options.gmm.components.full",
    "PatternStateSequence": "python.madmom.pattern.state.patterns2",
    "PatternFeatures": "python.madmom.pattern.features.tones",
    "PcmPatternSequence": "python.madmom.pattern.pcm.patterns2",
    "BeatFeatureSynchronization": "python.madmom.remaining.synchronization.subdivisions4",
    "ModelPredictionSelection": "python.madmom.remaining.model_selection.references1",
    "TempoProbabilityHistogram": "python.madmom.remaining.tempo_probabilities.n300.range100-150.tuple1",
}
REFERENCE_KINDS = {"PcmSpectralNovelty", "PcmOnsetEnvelope", "EnvelopeTempo", "EnvelopeTempoRatios",
                   "PcmTempo", "PcmTempogram", "PcmTempoRatios", "EnvelopeBeatPath", "PcmBeatPath", "PcmLocalPulse"}
MODULES = {
    "librosa": ("librosa_cases", "composition_cases", "remaining_librosa_cases", "options_onset_cases", "options_tempo_cases"),
    "madmom": ("madmom_cases", "model_cases", "pattern_cases", "remaining_madmom_cases", "remaining_state_cases",
               "wrapper_state_cases", "options_gmm_cases", "long_history_cases"),
}


def boundaries(lib):
    for length in (512, 4096):
        pcm = tones(length * 5 + 17, 22050)

        def reference(x, length=length):
            hop = length // 4
            z = lib.stft(x, n_fft=length, hop_length=hop, window="hann", center=True).T
            magnitude = np.abs(z)
            mel = lib.feature.melspectrogram(S=(magnitude ** 2).T, sr=22050, n_fft=length, n_mels=128)
            return {"complex": split_complex(z), "magnitude": magnitude, "powered": magnitude ** 2,
                    "features": mel.T, "db": lib.power_to_db(mel).T,
                    "values": lib.onset.onset_strength(y=x, sr=22050, n_fft=length, hop_length=hop,
                                                       aggregate=np.mean, max_size=3).reshape(-1, 1)}

        yield Case(f"python.librosa.cost_boundary.pcm_envelope.n{length}", "librosa", "pcm_strength",
                   pcm, [22050, length, length // 4, 0, 3, 0], reference,
                   dict(fs=22050, n_fft=length, hop=length // 4, power=2, aggregate="mean", max_size=3,
                        center=True, periodic=True, nyquist=True, samples=length * 5 + 17, seed=SEED))


def cases(original, library, precision):
    selected = {case_id for kind, case_id in REPRESENTATIVES.items()
                if precision == "portable-f32" or kind in REFERENCE_KINDS}
    expected = {name for name in selected if name.startswith(f"python.{library}.")}
    found = set()
    for module in MODULES[library]:
        for case in importlib.import_module(module).cases(original):
            if case.name in expected:
                assert case.name not in found, case.name
                found.add(case.name)
                yield case
    assert found == expected, ("representatives missing", expected - found)
    if library == "librosa":
        yield from boundaries(original)
