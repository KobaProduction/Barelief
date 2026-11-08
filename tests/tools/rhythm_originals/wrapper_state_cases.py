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
    yield from state_cases(original)
