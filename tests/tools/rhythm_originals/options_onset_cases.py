"""Finite original onset grouping/reference and detection option contracts."""
import numpy as np

from common import Case, SEED, envelope


def strength_cases(lib):
    values = np.random.default_rng(SEED + 51).uniform(-30, 0, (96, 8)).astype(np.float32)
    supplied = (values + np.linspace(-.5, .5, 8, dtype=np.float32)).astype(np.float32)
    modes = (("boundary_mean", False, 0, False, False),
             ("boundary_median_reference", False, 1, True, False),
             ("slice_sum_reference_detrend", True, 2, True, True),
             ("groups_ignored_reference_detrend", True, 3, True, True),
             ("custom_sum_reference", True, 4, True, False))
    for name, slices, mode, reference_input, detrend in modes:
        channels = [slice(0, 4), slice(2, 8)] if slices else [0, 3, 8]
        aggregate = (np.mean, np.median, np.sum, False, np.sum)[mode]

        def reference(x, channels=channels, aggregate=aggregate, reference_input=reference_input, detrend=detrend):
            output = lib.onset.onset_strength_multi(S=x.T, ref=supplied.T if reference_input else None,
                                                  lag=2, max_size=3, n_fft=64, hop_length=16,
                                                  center=True, detrend=detrend, channels=channels,
                                                  aggregate=aggregate)
            return {"values": output.T}

        yield Case("python.librosa.options.strength." + name, "librosa", "strength_options", values,
                   [int(slices), mode, int(reference_input), int(detrend)], reference,
                   dict(groups=[[0, 1, 2, 3], [2, 3, 4, 5, 6, 7]] if slices else [[0, 1, 2], [3, 4, 5, 6, 7]],
                        source_channels="slices" if slices else "boundaries", aggregate="custom_sum" if mode == 4 else str(mode),
                        supplied_reference=reference_input, detrend=detrend, lag=2, max_size=3,
                        n_fft=64, hop_length=16, center=True, seed=SEED + 51),
                   {".reference": supplied} if reference_input else {})


def detection_cases(lib):
    values = envelope(128)
    dense = np.column_stack((values, np.zeros_like(values), np.roll(values, 7)))
    modes = (("dense_default", False, 0, False), ("dense_custom_zero_delta", False, 0, True),
             ("frames_custom", True, 0, True), ("samples_custom", True, 1, True),
             ("seconds_custom", True, 2, True), ("samples_default", True, 1, False),
             ("seconds_default", True, 2, False))
    for name, sparse, units, custom in modes:
        def reference(x, sparse=sparse, units=units, custom=custom):
            kwargs = dict(pre_max=2, post_max=2, pre_avg=3, post_avg=4, wait=2 if sparse else 0,
                          delta=.05 if sparse else 0) if custom else {}
            output = lib.onset.onset_detect(onset_envelope=x if sparse else x.T, sr=8000,
                                           hop_length=80, sparse=sparse, normalize=True,
                                           units=("frames", "samples", "time")[units], **kwargs)
            return {"events": output} if sparse else {"mask": output.T.astype(np.uint8).ravel()}

        yield Case("python.librosa.options.detection." + name, "librosa", "detect_options",
                   values if sparse else dense, [int(sparse), units, int(custom)], reference,
                   dict(fs=8000, hop=80, sparse=sparse, units=("frames", "samples", "time")[units],
                        custom_peaks=custom, normalize=True, backtrack=False,
                        dense_layout="flattened time/channel", channels=1 if sparse else 3,
                        zero_channel_with_active_batch=not sparse))


def cases(lib):
    yield from strength_cases(lib)
    yield from detection_cases(lib)
