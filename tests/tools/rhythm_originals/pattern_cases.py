"""Pinned classical PatternTrackingProcessor with explicit supplied GMMs.

Original constructor/process/feature classes execute unchanged. Temporary pickle
files only adapt original model-file I/O to the same Float32 sidecar parameters;
no pretrained pattern, fitting, neural layer, or replacement HMM is involved.
"""
import importlib
from pathlib import Path
import pickle
import tempfile
import numpy as np

from common import Case, SEED, tones


def parameters(count):
    weights = np.tile(np.array([.4, .6], np.float32), (count, 2, 1))
    means = np.zeros((count, 2, 2, 2), np.float32)
    for p in range(count):
        for profile in range(2):
            means[p, profile] = [[.1 + .7 * profile + .2 * p, .4 - .2 * profile + .1 * p],
                                 [.3 + .7 * profile + .2 * p, .6 - .2 * profile + .1 * p]]
    covariances = np.tile(np.array([1.1, .8], np.float32), (count, 2, 2, 1))
    beats = np.array([3, 4][:count], np.float32)
    tempo = np.tile(np.array([100, 150, 0, 100], np.float32), (count, 1))
    return {".weights": weights, ".means": means, ".covariances": covariances,
            ".beats": beats, ".tempo": tempo}


def original_features(original, values):
    signal = original.signal.Signal(values, sample_rate=44100, num_channels=1, norm=False, gain=0)
    frames = original.signal.FramedSignal(signal, frame_size=2048, hop_size=882, origin=0, end="normal")
    stft = original.stft.ShortTimeFourierTransform(frames, window=np.hanning, fft_size=2048,
                                                  circular_shift=False, include_nyquist=False)
    filtered = original.spectrogram.FilteredSpectrogram(stft, num_bands=12, fmin=30, fmax=17000,
                                                        norm_filters=False, unique_filters=True)
    logarithm = original.spectrogram.LogarithmicSpectrogram(filtered, log=np.log10, mul=1, add=1)
    difference = original.spectrogram.SpectrogramDifference(logarithm, diff_ratio=.5,
                                                           diff_frames=None, diff_max_bins=None,
                                                           positive_diffs=True, keep_dims=True)
    features = original.spectrogram.MultiBandSpectrogram(difference, crossover_frequencies=[270],
                                                         fmin=30, fmax=17000, norm_filters=False,
                                                         unique_filters=True)
    return np.asarray(features)


def original_model(original, directory, data):
    downbeats = importlib.import_module("madmom.features.downbeats")
    paths = []
    for p, beats in enumerate(data[".beats"]):
        gmms = []
        for profile in range(2):
            gmm = original.gmm.GMM(n_components=2, covariance_type="diag")
            gmm.weights = data[".weights"][p, profile]
            gmm.means = data[".means"][p, profile]
            gmm.covars = data[".covariances"][p, profile]
            gmms.append(gmm)
        path = directory / f"pattern{p}.pkl"
        with path.open("wb") as stream:
            pickle.dump({"gmms": gmms, "num_beats": int(beats)}, stream, protocol=4)
        paths.append(str(path))
    return downbeats.PatternTrackingProcessor(paths, min_bpm=data[".tempo"][:, 0].astype(float),
                                               max_bpm=data[".tempo"][:, 1].astype(float),
                                               num_tempi=None, transition_lambda=100, fps=50)


def original_outputs(original, features, data):
    with tempfile.TemporaryDirectory(prefix="barelief-supplied-pattern-") as temporary:
        model = original_model(original, Path(temporary), data)
        events = model.process(features)
        path, score = model.hmm.viterbi(features)
    return {"times": events[:, 0], "numbers": events[:, 1].astype(np.uint32),
            "path": path, "log_probability": np.array([score])}


def pattern_settings(count, pcm):
    return dict(pattern_count=count, profiles_per_pattern=2, components_per_gmm=2, features=2,
                covariance="diag", supplied_models=True, fitting=False, pretrained=False, neural_network=False,
                beats=[3, 4][:count], minimum_bpm=100, maximum_bpm=150, num_tempi=None,
                transition_lambda=100, fps=50, pattern_switches=False,
                frontend="actual GMMPatternTracker fixed pipeline" if pcm else "supplied Float32 feature rows",
                fs=44100 if pcm else None, frame_size=2048 if pcm else None, hop=882 if pcm else None)


def cases(original):
    pcm = tones(88200, 44100)
    for kind, values in (("tones", pcm), ("silence", np.zeros_like(pcm))):
        def reference(x):
            return {"features": original_features(original, x)}
        yield Case("python.madmom.pattern.features." + kind, "patterns", "features", values, [], reference,
                   dict(fs=44100, fps=50, n_fft=2048, hop=882, center=True, end="normal", window="symmetric Hann",
                        include_nyquist=False, circular_shift=False, bands_per_octave=12, fmin=30, fmax=17000,
                        normalized_filters=False, logarithm="log10(1+x)", diff_ratio=.5,
                        positive_diffs=True, crossover_hz=[270]))
    values = np.random.default_rng(SEED).random((192, 2)).astype(np.float32)
    for count in (1, 2):
        data = parameters(count)
        def feature_reference(x, data=data):
            return original_outputs(original, x, data)
        yield Case(f"python.madmom.pattern.state.patterns{count}", "patterns", "state", values, [count, 2, 2],
                   feature_reference, pattern_settings(count, False), data)
        def pcm_reference(x, data=data):
            features = original_features(original, x)
            return {"features": features, **original_outputs(original, features, data)}
        yield Case(f"python.madmom.pattern.pcm.patterns{count}", "patterns", "pcm", pcm, [count, 2, 2],
                   pcm_reference, pattern_settings(count, True), data)
