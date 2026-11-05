"""Unchanged supplied-probability/classical postprocessors; no neural model."""
import importlib
import itertools
import numpy as np

from common import Case, SEED


def downbeat_cases(original):
    downbeats = importlib.import_module("madmom.features.downbeats")
    values = np.full((192, 2), .01, np.float32)
    for index, frame in enumerate(range(15, 185, 25)):
        values[frame - 1:frame + 2, int(index % 3 == 0)] = (.25, .8, .3)
    for correct, threshold in itertools.product((False, True), (0, .05)):
        def reference(x, correct=correct, threshold=threshold):
            processor = downbeats.DBNDownBeatTrackingProcessor([3, 4], min_bpm=100, max_bpm=150,
                                                              num_tempi=None, transition_lambda=100,
                                                              fps=50, threshold=threshold, correct=correct, num_threads=1)
            events = processor.process(x)
            cropped, first = original.beats.threshold_activations(x, threshold) if threshold else (x, 0)
            decoded = [hmm.viterbi(cropped) for hmm in processor.hmms]
            likelihoods = np.asarray([item[1] for item in decoded])
            selected = int(np.argmax(likelihoods))
            return {"times": events[:, 0], "numbers": events[:, 1].astype(np.uint32),
                    "path": decoded[selected][0], "likelihoods": likelihoods,
                    "selected": np.asarray([selected], np.uint64), "first_frame": np.asarray([first], np.uint64)}
        yield Case(f"python.madmom.remaining.downbeat.correct{int(correct)}.threshold{threshold}", "remaining", "downbeat",
                   values, [int(correct), threshold], reference,
                   dict(fps=50, meters=[3, 4], min_bpm=100, max_bpm=150, num_tempi=None,
                        transition_lambda=100, observation_lambda=16, threshold=threshold, correct=correct,
                        supplied_activations=True, neural_network=False, threads=1))


def bar_cases(original):
    downbeats = importlib.import_module("madmom.features.downbeats")
    beats = (.15 + .5 * np.arange(32)).astype(np.float32)
    probabilities = np.asarray([.9 if i % 4 == 0 else .05 for i in range(32)], np.float32)
    probabilities[-1] = np.nan
    for switching in (0, .1):
        def reference(x, switching=switching):
            processor = downbeats.DBNBarTrackingProcessor(beats_per_bar=[3, 4], observation_weight=100,
                                                         meter_change_prob=switching)
            events = processor.process(np.column_stack((beats, x)))
            path, _ = processor.hmm.viterbi(x[:-1])
            return {"numbers": events[:, 1].astype(np.uint32), "path": path}
        yield Case(f"python.madmom.remaining.bar.switch{switching}", "remaining", "bar", probabilities,
                   [switching], reference, dict(meters=[3, 4], observation_weight=100,
                                                meter_change_probability=switching, last_probability="NaN ignored by original", supplied_activations=True),
                   {".beats": beats})


def synchronization_cases(original):
    downbeats = importlib.import_module("madmom.features.downbeats")
    values = np.random.default_rng(SEED).random((128, 3)).astype(np.float32)
    beats = np.array([0, .2, .4, 1.4], np.float32)
    for subdivisions in (4, 64):
        def reference(x, subdivisions=subdivisions):
            output = downbeats.SyncronizeFeaturesProcessor(subdivisions, fps=100).process((x, beats))
            return {"values": output.reshape(-1, x.shape[1])}
        yield Case(f"python.madmom.remaining.synchronization.subdivisions{subdivisions}", "remaining", "synchronization",
                   values, [subdivisions, beats.size, 100], reference,
                   dict(fps=100, subdivisions=subdivisions, beats=beats.tolist(), empty_subdivision="source mean(empty) NaN", trim_beyond_duration=True),
                   {".beats": beats})


def selection_cases(original):
    values = np.random.default_rng(SEED).random((4, 64)).astype(np.float32)
    values[2] = values[1]
    for references in (0, 1, 4):
        def reference(x, references=references):
            output = original.beats.MultiModelSelectionProcessor(references).process(list(x))
            selected = np.flatnonzero([np.shares_memory(output, row) for row in x]).astype(np.uint64)
            return {"values": output, "selected": selected}
        yield Case(f"python.madmom.remaining.model_selection.references{references}", "remaining", "selection", values,
                   [references], reference, dict(reference_count=references, models=4, frames=64,
                                                identical_candidates=[1, 2], selected_index="observed alias of returned original prediction", neural_network=False))


def probability_cases(original):
    for count, low, high, multitask in ((300, 40, 250, False), (100, 40, 250, False), (300, 100, 150, True)):
        values = np.linspace(1, 2, count, dtype=np.float32)
        values /= values.sum()
        def reference(x, low=low, high=high, multitask=multitask):
            data = (np.zeros(17, np.float32), x) if multitask else x
            bins, coordinates = original.tempo.TCNTempoHistogramProcessor(min_bpm=low, max_bpm=high).process(data)
            return {"values": bins, "coordinates": coordinates}
        yield Case(f"python.madmom.remaining.tempo_probabilities.n{count}.range{low}-{high}.tuple{int(multitask)}",
                   "remaining", "probabilities", values, [low, high], reference,
                   dict(min_bpm=low, max_bpm=high, classes=count, input="explicit tempo task probabilities",
                        original_tuple_route=multitask, neural_network=False, source_argmin_endpoint_preserved=True))


def cases(original):
    yield from downbeat_cases(original)
    yield from bar_cases(original)
    yield from synchronization_cases(original)
    yield from selection_cases(original)
    yield from probability_cases(original)
