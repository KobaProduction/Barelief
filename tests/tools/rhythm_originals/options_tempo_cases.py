"""Explicit priors and custom reduction for the shared tempo estimator."""
import numpy as np

from common import Case, SEED


class SuppliedPrior:
    def __init__(self, values):
        self.values = np.asarray(values, np.float32)

    def logpdf(self, coordinates):
        if self.values.ndim == 2:
            return self.values.T.copy()
        return self.values[:, None].copy() if coordinates.ndim == 2 else self.values.copy()


def temporal_inputs():
    values = (.01 + np.random.default_rng(SEED + 52).random((32, 48)) * .03).astype(np.float32)
    for row in range(values.shape[0]):
        values[row, 21 + row % 8] += .4
    prior = np.full(48, -3, np.float32)
    prior[20:32] = 0
    prior[24] = .6
    prior[0] = -np.inf
    return values, prior


def tempo_cases(lib):
    values, prior = temporal_inputs()
    for custom in (False, True):
        def reference(x, custom=custom):
            output = lib.feature.tempo(tg=x.T, sr=50, hop_length=1, max_tempo=240,
                                       aggregate=np.sum if custom else None, prior=SuppliedPrior(prior))
            return {"values": output}

        yield Case(f"python.librosa.options.tempo.prior.custom_sum{int(custom)}", "composition", "tempo_options",
                   values, [int(custom)], reference,
                   dict(frame_rate=50, max_tempo=240, prior="explicit Float32 log-probability per lag",
                        aggregate="custom_sum" if custom else None, seed=SEED + 52), {".prior": prior})


def cases(lib):
    yield from tempo_cases(lib)
