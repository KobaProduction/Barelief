"""Finite whole-operation cost representatives, reusing canonical source cases."""
import importlib

import numpy as np

from common import Case, SEED, tones
from librosa_cases import split_complex


# Stable operation kind -> existing exact same-input/source/settings case.
REPRESENTATIVES = {
    "SpectralNovelty": "python.madmom.novelty.spectral_flux.lag1",
    "OnsetBacktrack": "python.librosa.remaining.backtrack.unsorted_repeated",
    "TempoEstimate": "python.librosa.options.tempo.prior.custom_sum0",
    "DynamicBeatPath": "python.librosa.beat.bpm120.trim1",
    "BeatTrackingSequence": "python.madmom.interval_search.fixed24",
    "CrfBeatSequence": "python.madmom.crf_search.explicit",
    "BeatStateSequence": "python.madmom.wrapper.beat_state.online.two_chunks",
    "DownbeatStateSequence": "python.madmom.remaining.downbeat.correct1.threshold0.05",
    "BarStateSequence": "python.madmom.remaining.bar.switch0.1",
    "GaussianMixtureScore": "python.madmom.options.gmm.components.full",
    "PatternStateSequence": "python.madmom.pattern.state.patterns2",
    "BeatFeatureSynchronization": "python.madmom.remaining.synchronization.subdivisions4",
    "ModelPredictionSelection": "python.madmom.remaining.model_selection.references1",
    "TempoProbabilityHistogram": "python.madmom.remaining.tempo_probabilities.n300.range100-150.tuple1",
}
MODULES = {
    "librosa": ("librosa_cases", "remaining_librosa_cases", "options_tempo_cases"),
    "madmom": ("madmom_cases", "model_cases", "pattern_cases", "remaining_madmom_cases", "remaining_state_cases",
               "wrapper_state_cases", "options_gmm_cases"),
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
