"""Original interval decoders with explicitly supplied activations and periods."""
import numpy as np
from common import Case, envelope


def cases(original):
    values = envelope(192)
    yield Case("python.madmom.interval_search.fixed24", "remaining", "interval_search", values, [24],
               lambda x: {"positions": original.beats.detect_beats(x, 24, .2)},
               dict(interval=24, look_aside=.2, preprocessing="none", supplied_activations=True))
    intervals = np.asarray([12, 24, 48], np.float32)

    def reference(x):
        ordered = np.sort(intervals.astype(np.uint64))[::-1]
        paths, scores = zip(*(original.crf.best_sequence(x, int(interval), .18) for interval in ordered))
        normalized = np.asarray(scores) / np.asarray([len(path) for path in paths])
        return {"positions": paths[np.argmax(normalized)], "intervals": ordered, "scores": normalized}

    yield Case("python.madmom.crf_search.explicit", "remaining", "crf_search", values, [len(intervals)],
               reference, dict(intervals=intervals.tolist(), interval_sigma=.18, preprocessing="none",
                               supplied_activations=True), {".intervals": intervals})
