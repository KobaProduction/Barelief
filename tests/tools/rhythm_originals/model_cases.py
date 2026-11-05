"""Classical supplied-feature GMM/HMM/CRF: no pretrained network or weights."""
import itertools
import numpy as np

from common import Case, SEED, envelope


def gmm_cases(original):
    values = np.random.default_rng(SEED).normal(size=(24, 2)).astype(np.float32)
    means = np.array([[-.4, .2], [.7, -.3]], np.float32)
    weights = np.array([.4, .6], np.float32)
    covariances = (np.array([[.8, 1.2], [1.1, .7]], np.float32), np.array([[.8], [1.1]], np.float32),
                   np.array([[.8, .1], [.1, 1.2]], np.float32),
                   np.array([[[.8, .1], [.1, 1.2]], [[1.1, -.1], [-.1, .7]]], np.float32))
    for kind, name in enumerate(("diag", "spherical", "tied", "full")):
        covariance = covariances[kind]
        def reference(x, name=name, covariance=covariance):
            model = original.gmm.GMM(n_components=2, covariance_type=name)
            model.weights, model.means, model.covars = weights, means, covariance
            scores, responsibilities = model.score_samples(x)
            return {"scores": scores, "responsibilities": responsibilities}
        yield Case(f"python.madmom.gmm.{name}", "models", "gmm", values, [kind, 2], reference,
                   dict(covariance=name, components=2, features=2, fitting=False, spherical_input="explicit column"),
                   {".weights": weights, ".means": means, ".covariances": covariance})


def hmm_cases(original):
    random = np.random.default_rng(SEED)
    transitions = np.array([[.75, .25, 0], [0, .7, .3], [.2, 0, .8]], np.float32)
    initial = np.array([.5, .25, .25], np.float32)
    for mode in ("viterbi", "forward"):
        densities = (.05 + random.random((96, 3))).astype(np.float32)
        values = np.log(densities).astype(np.float32) if mode == "viterbi" else densities
        def reference(x, mode=mode):
            previous, states = np.nonzero(transitions)
            model = original.hmm.TransitionModel.from_dense(states.astype(np.uint32), previous.astype(np.uint32),
                                                             transitions[previous, states].astype(float))
            class Observations(original.hmm.ObservationModel):
                def log_densities(self, data):
                    return data.astype(float) if mode == "viterbi" else np.log(data.astype(float))
                def densities(self, data):
                    return np.exp(data.astype(float)) if mode == "viterbi" else data.astype(float)
            hmm = original.hmm.HiddenMarkovModel(model, Observations(np.arange(3, dtype=np.uint32)), initial.astype(float))
            if mode == "forward":
                return {"posteriors": hmm.forward(x, reset=True)}
            path, probability = hmm.viterbi(x)
            return {"path": path, "log_probability": np.array([probability])}
        yield Case(f"python.madmom.hmm.{mode}", "models", "hmm_" + mode, values, [], reference,
                   dict(states=3, density_count=3, initial=initial.tolist(), mode=mode, observation_mapping="identity", reset=True),
                   {".transitions": transitions, ".initial": initial})


def crf_cases(original):
    values = envelope(384)
    for interval, sigma in itertools.product((24, 25, 40), (.1, .18)):
        def reference(x, interval=interval, sigma=sigma):
            positions, probability = original.crf.best_sequence(x, interval, sigma)
            return {"positions": positions, "log_probability": np.array([probability]),
                    "transition": original.crf.transition_distribution(interval, sigma)}
        yield Case(f"python.madmom.crf.interval{interval}.sigma{sigma}", "models", "crf", values,
                   [interval, sigma], reference, dict(interval=interval, interval_sigma=sigma, supplied_activations=True, neural_network=False))


def beat_state_cases(original):
    values = envelope(384)
    for correct in (False, True):
        def reference(x, correct=correct):
            processor = original.beats.DBNBeatTrackingProcessor(min_bpm=60, max_bpm=180, num_tempi=8,
                                                               fps=100, threshold=0, correct=correct)
            times = processor.process_offline(x)
            path, _ = processor.hmm.viterbi(x)
            mask = np.zeros(x.size, np.uint8)
            mask[np.rint(times * 100).astype(int)] = 1
            return {"mask": mask, "path": path}
        yield Case(f"python.madmom.dbn.correct{int(correct)}", "models", "beat_states", values,
                   [100, 60, 180, 8, int(correct)], reference,
                   dict(fps=100, min_bpm=60, max_bpm=180, num_tempi=8, threshold=0, correct=correct, supplied_activations=True))


def cases(original):
    yield from gmm_cases(original)
    yield from hmm_cases(original)
    yield from crf_cases(original)
    yield from beat_state_cases(original)
