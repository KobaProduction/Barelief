"""Explicit priors, BPM/coordinate rows and custom reduction source paths."""
import numpy as np

from common import Case, SEED, envelope
from remaining_librosa_cases import supplied_fourier_plp


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


def ratio_cases(lib):
    values, prior = temporal_inputs()
    frequencies = lib.tempo_frequencies(48, sr=50, hop_length=1).astype(np.float32)
    bpm = np.linspace(100, 140, values.shape[0], dtype=np.float32)
    factors = np.asarray([.5, 1, 1.5, 2], np.float32)
    dynamic = (frequencies[None, :] * (1 + .025 * np.sin(np.arange(values.shape[0]))[:, None])).astype(np.float32)
    modes = (("bpm_rows", False, True, False), ("bpm_and_frequency_rows", True, True, False),
             ("estimated_bpm_supplied_prior", False, False, False), ("dynamic_rows_custom_sum", True, True, True))
    for name, moving, supplied_bpm, custom in modes:
        coords = dynamic if moving else frequencies

        def reference(x, coords=coords, supplied_bpm=supplied_bpm, custom=custom):
            output = lib.feature.tempogram_ratio(tg=x.T, sr=50, hop_length=1, freqs=coords.T,
                                                bpm=bpm if supplied_bpm else None, max_tempo=240,
                                                prior=SuppliedPrior(prior), factors=factors,
                                                aggregate=np.sum if custom else None, fill_value=0)
            return {"values": output.reshape(1, -1) if custom else output.T}

        yield Case("python.librosa.options.ratios." + name, "composition", "ratio_options", values,
                   [int(moving), int(supplied_bpm), int(custom), factors.size], reference,
                   dict(frame_rate=50, max_tempo=240, dynamic_frequencies=moving, supplied_bpm_rows=supplied_bpm,
                        factors=factors.tolist(), custom_sum=custom, supplied_log_prior=True, fill=0, seed=SEED + 52),
                   {".frequencies": coords, ".bpm": bpm, ".prior": prior, ".factors": factors})


def beat_cases(lib):
    values = envelope(256)
    bpm = np.linspace(112, 128, values.size, dtype=np.float32)
    for trim in (False, True):
        def reference(x, trim=trim):
            tempo, events = lib.beat.beat_track(onset_envelope=x, sr=50, hop_length=1,
                                              bpm=bpm, tightness=100, trim=trim)
            return {"bpm": tempo, "events": events}

        yield Case(f"python.librosa.options.beats.bpm_rows.trim{int(trim)}", "composition", "beat_options",
                   values, [int(trim)], reference,
                   dict(frame_rate=50, supplied_bpm="one Float32 value per frame", tightness=100,
                        trim=trim, sparse=True), {".bpm": bpm})


def pulse_cases(lib):
    samples = 128
    random = np.random.default_rng(SEED + 53)
    complex_rows = (random.normal(size=(samples + 1, 33)) + 1j * random.normal(size=(samples + 1, 33))).astype(np.complex64)
    complex_rows[:, (0, -1)] = complex_rows[:, (0, -1)].real
    values = np.stack((complex_rows.real, complex_rows.imag), axis=-1).reshape(samples + 1, 66)
    static = np.linspace(-.4, .4, 33, dtype=np.float32)
    moving = np.broadcast_to(static, (samples + 1, 33)).copy()
    moving[np.arange(samples + 1), 1 + np.arange(samples + 1) % 3] += .7
    for rows in (False, True):
        prior = moving if rows else static

        def reference(x, prior=prior):
            return {"values": supplied_fourier_plp(lib, x, samples, SuppliedPrior(prior))}

        yield Case(f"python.librosa.options.pulse.prior_rows{int(rows)}", "composition", "pulse_options",
                   values, [samples, int(rows)], reference,
                   dict(samples=samples, frame_rate=100, window_length=64, min_bpm=30, max_bpm=300,
                        input="explicit Complex64 Fourier rows", supplied_log_prior_rows=rows, seed=SEED + 53),
                   {".prior": prior},
                   policy="unchanged pinned plp function body; its upstream provider supplies explicit identical Complex64 rows; explicit prior object returns supplied log probabilities")


def cases(lib):
    yield from tempo_cases(lib)
    yield from ratio_cases(lib)
    yield from beat_cases(lib)
    yield from pulse_cases(lib)
