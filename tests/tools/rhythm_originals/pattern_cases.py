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

from common import Case, SEED


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


def pattern_settings(count):
    return dict(pattern_count=count, profiles_per_pattern=2, components_per_gmm=2, features=2,
                covariance="diag", supplied_models=True, fitting=False, pretrained=False, neural_network=False,
                beats=[3, 4][:count], minimum_bpm=100, maximum_bpm=150, num_tempi=None,
                transition_lambda=100, fps=50, pattern_switches=False,
                frontend="supplied Float32 feature rows")


def cases(original):
    values = np.random.default_rng(SEED).random((192, 2)).astype(np.float32)
    for count in (1, 2):
        data = parameters(count)
        def feature_reference(x, data=data):
            return original_outputs(original, x, data)
        yield Case(f"python.madmom.pattern.state.patterns{count}", "patterns", "state", values, [count, 2, 2],
                   feature_reference, pattern_settings(count), data)
