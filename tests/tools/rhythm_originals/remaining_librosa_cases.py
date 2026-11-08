"""Actual source backtracking and downstream PLP from explicit complex rows."""
import numpy as np

from common import Case, SEED, envelope




def cases(lib):
    energy = envelope(96)
    for name, events in (("ordered", [3, 12, 27, 46, 95]), ("unsorted_repeated", [46, 3, 46, 95, 7])):
        indices = np.asarray(events, np.float32)
        def reference(x, indices=indices):
            return {"events": lib.onset.onset_backtrack(indices.astype(np.int64), x)}
        yield Case("python.librosa.remaining.backtrack." + name, "remaining", "backtrack", energy,
                   [indices.size], reference, dict(indices=events, sparse=True, repeated_indices_preserved=True), {".events": indices})
