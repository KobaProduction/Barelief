"""Explicit intended N-channel extension, separate from the literal 2D defect.

The pinned Cython 2D backward comb loops over range(2). The existing literal
three-channel case is retained unchanged. Here every column executes the actual
unchanged original 1D histogram/comb chain; no recurrence is reimplemented.
"""
import numpy as np

from common import Case, SEED, envelope


def per_channel_original_1d(original, values):
    histograms, axis = [], None
    for channel in range(values.shape[1]):
        bins, intervals = original.tempo.interval_histogram_comb(
            values[:, channel], alpha=.79, min_tau=12, max_tau=60)
        if axis is not None:
            np.testing.assert_array_equal(intervals, axis)
        histograms.append(bins)
        axis = intervals
    return {"values": np.stack(histograms), "intervals": axis}


def cases(original):
    identical = np.tile(envelope(192)[:, None], (1, 3))
    random = np.random.default_rng(SEED)
    distinct = (.03 * random.random((192, 5))).astype(np.float32)
    for channel in range(5):
        distinct[:, channel] += np.roll(envelope(192), 3 * channel) * np.float32(.3 + .1 * channel)
    for name, values in (("channels3.identical", identical), ("channels5.distinct", distinct)):
        yield Case("python.madmom.comb.per_channel_original_1d." + name,
                   "madmom", "histogram", values, [1, 12, 60, .79],
                   lambda x: per_channel_original_1d(original, x),
                   dict(method="backward_comb", minimum_interval=12, maximum_interval=60,
                        alpha=.79, channels=values.shape[1], online=False,
                        literal_source_case="python.madmom.histogram.method1.channels3",
                        original_route="interval_histogram_comb(column) -> CombFilterbankProcessor -> _feed_backward_comb_filter_1d"),
                   policy="per_channel_original_1d: unchanged pinned original called independently on each column; literal 2D defect retained separately",
                   evidence_category="intended-generalization")
