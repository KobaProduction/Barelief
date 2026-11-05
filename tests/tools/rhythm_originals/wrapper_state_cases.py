"""Actual complete classical CRF/DBN wrappers on supplied activations."""
import itertools
import sys

import numpy as np

from common import Case, envelope


def capture_return(function, values, **kwargs):
    """Observe original return locals without rewriting its numerical body."""
    code, captured, previous = function.__func__.__code__, {}, sys.getprofile()

    def probe(frame, event, argument):
        if event == "return" and frame.f_code is code:
            captured.update(frame.f_locals)

    sys.setprofile(probe)
    try:
        return function(values, **kwargs), captured
    finally:
        sys.setprofile(previous)


def crf_cases(original):
    values = envelope(384)
    for factors, count, sigma in itertools.product((False, True), (1, 3), (.1, .18)):
        if factors and count != 3:
            continue

        def reference(x, factors=factors, count=count, sigma=sigma):
            tempo = original.tempo.TempoEstimationProcessor(method="comb", min_bpm=60, max_bpm=240,
                                                           fps=50, act_smooth=.04, hist_smooth=3,
                                                           hist_buffer=2, alpha=.79, interpolate=False)
            processor = original.beats.CRFBeatDetectionProcessor(interval_sigma=sigma, use_factors=factors,
                                                               num_intervals=count, fps=50,
                                                               tempo_estimator=tempo, num_threads=1)
            times, observed = capture_return(processor.process, x)
            return {"times": times, "positions": observed["best_seq"],
                    "intervals": np.asarray(observed["possible_intervals"], np.uint64),
                    "scores": observed["normalized_seq_probabilities"], "tempo_candidates": observed["tempi"]}

        yield Case(f"python.madmom.wrapper.crf.factors{int(factors)}.top{count}.sigma{sigma}",
                   "remaining", "crf_wrapper", values, [int(factors), count, sigma], reference,
                   dict(fps=50, min_bpm=60, max_bpm=240, method="comb", alpha=.79,
                        activation_smoothing_seconds=.04, histogram_smoothing=3, history_seconds=2,
                        interval_sigma=sigma, use_factors=factors, num_intervals=count,
                        factors=[.5, .67, 1, 1.5, 2], supplied_activations=True, neural_network=False,
                        observed_outputs="unchanged original wrapper return locals; trial ordering/ranking included"))


def state_outputs(processor, values, suffix, online, reset=True):
    function = processor.process_online if online else processor.process_offline
    times, observed = capture_return(function, values, **({"reset": reset} if online else {}))
    return {"times" + suffix: times,
            "path" + suffix: observed["states" if online else "path"],
            "first_frame" + suffix: np.asarray([0 if online else observed["first"]], np.uint64),
            "counter" + suffix: np.asarray([getattr(processor, "counter", 0)], np.uint64),
            "bpm" + suffix: np.asarray([getattr(processor, "tempo", 0)])}


def state_cases(original):
    values = envelope(192)
    for correct, threshold in itertools.product((False, True), (0, .05)):
        def reference(x, correct=correct, threshold=threshold):
            processor = original.beats.DBNBeatTrackingProcessor(min_bpm=100, max_bpm=150, num_tempi=None,
                                                               fps=50, threshold=threshold, correct=correct)
            return state_outputs(processor, x, "", False)

        yield Case(f"python.madmom.wrapper.beat_state.offline.correct{int(correct)}.threshold{threshold}",
                   "remaining", "state_wrapper", values, [0, int(correct), threshold, 0, 0], reference,
                   dict(fps=50, min_bpm=100, max_bpm=150, num_tempi=None, correct=correct,
                        threshold=threshold, online=False, supplied_activations=True, neural_network=False,
                        observed_outputs="unchanged original wrapper events/path/threshold crop"))
    modes = (("two_chunks", [64, 128], [True, False], False),
             ("small_chunks", [1, 31, 64, 96], [True, False, False, False], False),
             ("reset_midstream", [48, 64, 80], [True, False, True], False),
             ("offline_interlude", [64, 128], [True, False], True))
    for name, chunks, resets, interlude in modes:
        def reference(x, chunks=chunks, resets=resets, interlude=interlude):
            processor = original.beats.DBNBeatTrackingProcessor(min_bpm=100, max_bpm=150, num_tempi=None,
                                                               fps=50, threshold=0, correct=True, online=True)
            output, first = {}, 0
            for index, (count, reset) in enumerate(zip(chunks, resets)):
                output.update(state_outputs(processor, x[first:first + count], str(index), True, reset))
                first += count
                if interlude and index == 0:
                    output.update(state_outputs(processor, x, "offline", False))
            return output

        yield Case("python.madmom.wrapper.beat_state.online." + name, "remaining", "state_wrapper", values,
                   [1, 1, 0, len(chunks), int(interlude)], reference,
                   dict(fps=50, min_bpm=100, max_bpm=150, num_tempi=None, correct=True,
                        threshold=0, online=True, chunks=chunks, resets=resets,
                        offline_interlude=interlude, supplied_activations=True, neural_network=False,
                        observed_outputs="unchanged original wrapper events/forward states/counter/BPM"),
                   {".chunks": np.asarray(chunks, np.float32), ".resets": np.asarray(resets, np.float32)})


def cases(original):
    yield from crf_cases(original)
    yield from state_cases(original)
