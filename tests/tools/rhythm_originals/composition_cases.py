"""Next finite packet: actual original full envelope/PCM recipes and metrogram."""
import itertools
import numpy as np

from common import Case, SEED, envelope, tones

FACTORS = np.array([.5, 1, 2, 3, 4], np.float32)
AGGREGATES = (None, np.mean, np.median)


def arguments(fs, hop, width, mode=0, supplied=False, trim=True):
    return [fs, hop, 2048, width, mode, int(supplied), 120, int(trim)]


def settings(fs, hop, width, **extra):
    return dict(fs=fs, hop=hop, n_fft=2048, temporal_length=width, center=True,
                window="periodic Hann", padding="constant PCM/Fourier; linear-ramp ACF",
                power=2, nyquist=True, normalization="raw FFT; Slaney Mel; dB ref1/top80",
                start_bpm=120, std_bpm=1, max_tempo=320, **extra)


def pcm_input():
    time = np.arange(32768, dtype=np.float64) / 8000
    amplitude = .04 + .96 * np.exp(-.5 * ((np.remainder(time, .5) - .07) / .025)**2)
    return (tones(time.size, 8000) * amplitude).astype(np.float32)


def onset(lib, x, median=False):
    return lib.onset.onset_strength(y=x, sr=8000, hop_length=64, n_fft=2048,
                                   aggregate=np.median if median else np.mean)


def metrical_cases(lib):
    frequencies = np.linspace(0, 400, 41, dtype=np.float32)
    values = np.random.default_rng(SEED).random((19, frequencies.size)).astype(np.float32)
    factors = np.array([1 / 3, .25, .2, 1 / 7], np.float32)
    for reverse, mode in itertools.product((False, True), range(4)):
        grid = frequencies[::-1].copy() if reverse else frequencies
        rows = values[:, ::-1].copy() if reverse else values
        aggregate = (None, np.mean, np.median, np.sum)[mode]
        def reference(x, grid=grid, aggregate=aggregate):
            result = lib.feature.metrogram(tg=x.T, freqs=grid, factors=factors,
                                           aggregate=aggregate, kind="linear", fill_value=0)
            return {"values": np.moveaxis(result, -1, 0).reshape(x.shape[0], -1)}
        yield Case(f"python.librosa.composition.metrogram.reverse{int(reverse)}.aggregate{mode}",
                   "composition", "metrical", rows, [mode, factors.size], reference,
                   dict(factors=factors.tolist(), interpolation="linear", fill=0,
                        aggregate=mode, grid_order="descending" if reverse else "ascending",
                        output_order="time,factor,original-bin; aggregate collapses bin only"),
                   {".frequencies": grid, ".factors": factors})


def envelope_cases(lib):
    values = envelope(384)
    for mode, aggregate in enumerate(AGGREGATES):
        def reference(x, aggregate=aggregate):
            return {"values": lib.feature.tempo(onset_envelope=x, sr=100, hop_length=1,
                                                ac_size=1.28, aggregate=aggregate).reshape(-1)}
        yield Case(f"python.librosa.composition.envelope_tempo.aggregate{mode}", "composition", "envelope_tempo",
                   values, arguments(100, 1, 128, mode), reference, settings(100, 1, 128, aggregate=mode))
    for supplied in (False, True):
        def reference(x, supplied=supplied):
            result = lib.feature.tempogram_ratio(onset_envelope=x, sr=100, hop_length=1, win_length=128,
                                                 factors=FACTORS, bpm=np.array([120], np.float32) if supplied else None,
                                                 aggregate=np.sum)
            return {"values": result.reshape(1, -1)}
        yield Case(f"python.librosa.composition.envelope_ratios.supplied{int(supplied)}", "composition", "envelope_ratios",
                   values, arguments(100, 1, 128, 3, supplied), reference,
                   settings(100, 1, 128, supplied_bpm=120 if supplied else None, factors=FACTORS.tolist(), aggregate="sum"))
    for supplied, trim in itertools.product((False, True), (False, True)):
        def reference(x, supplied=supplied, trim=trim):
            bpm, events = lib.beat.beat_track(onset_envelope=x, sr=100, hop_length=1,
                                              bpm=np.array([120], np.float32) if supplied else None,
                                              tightness=100, trim=trim, sparse=True, units="frames")
            return {"bpm": np.atleast_1d(bpm), "events": events}
        yield Case(f"python.librosa.composition.envelope_beats.supplied{int(supplied)}.trim{int(trim)}",
                   "composition", "envelope_beats", values, arguments(100, 1, 800, 1, supplied, trim), reference,
                   settings(100, 1, 800, supplied_bpm=120 if supplied else None, trim=trim, sparse=True, units="frames", tightness=100))


def pcm_tempo_cases(lib, values):
    for mode, aggregate in enumerate(AGGREGATES):
        def reference(x, aggregate=aggregate):
            return {"onset": onset(lib, x),
                    "values": lib.feature.tempo(y=x, sr=8000, hop_length=64, ac_size=1.024,
                                                aggregate=aggregate).reshape(-1)}
        yield Case(f"python.librosa.composition.pcm_tempo.aggregate{mode}", "composition", "pcm_tempo", values,
                   arguments(8000, 64, 128, mode), reference, settings(8000, 64, 128, aggregate=mode, onset_aggregate="mean"))
    for mode, name in enumerate(("tempogram", "fourier_tempogram", "hybrid_tempogram")):
        def reference(x, mode=mode, name=name):
            matrix = getattr(lib.feature, name)(y=x, sr=8000, hop_length=64, win_length=128)
            if mode == 1:
                matrix = np.stack((matrix.real, matrix.imag), axis=-1).transpose(1, 0, 2).reshape(matrix.shape[-1], -1)
            else:
                matrix = matrix.T
            grid = lib.tempo_frequencies(128, sr=8000, hop_length=64) if mode == 0 else lib.fourier_tempo_frequencies(sr=8000, hop_length=64, win_length=128)
            return {"onset": onset(lib, x), "values": matrix, "coordinates": grid}
        yield Case(f"python.librosa.composition.pcm_tempogram.method{mode}", "composition", "pcm_tempogram", values,
                   arguments(8000, 64, 128, mode), reference, settings(8000, 64, 128, method=name, onset_aggregate="mean"))
    for supplied in (False, True):
        def reference(x, supplied=supplied):
            result = lib.feature.tempogram_ratio(y=x, sr=8000, hop_length=64, win_length=128, factors=FACTORS,
                                                 bpm=np.array([120], np.float32) if supplied else None, aggregate=np.sum)
            return {"onset": onset(lib, x), "values": result.reshape(1, -1)}
        yield Case(f"python.librosa.composition.pcm_ratios.supplied{int(supplied)}", "composition", "pcm_ratios", values,
                   arguments(8000, 64, 128, 3, supplied), reference,
                   settings(8000, 64, 128, supplied_bpm=120 if supplied else None, factors=FACTORS.tolist(), aggregate="sum", onset_aggregate="mean"))


def pcm_beat_cases(lib, values):
    for supplied in (False, True):
        def reference(x, supplied=supplied):
            bpm, events = lib.beat.beat_track(y=x, sr=8000, hop_length=64, trim=True, tightness=100,
                                              bpm=np.array([120], np.float32) if supplied else None,
                                              sparse=True, units="frames")
            return {"onset": onset(lib, x, True), "bpm": np.atleast_1d(bpm), "events": events}
        yield Case(f"python.librosa.composition.pcm_beats.supplied{int(supplied)}", "composition", "pcm_beats", values,
                   arguments(8000, 64, 1000, 1, supplied), reference,
                   settings(8000, 64, 1000, onset_aggregate="median", supplied_bpm=120 if supplied else None, trim=True, tightness=100, units="frames"))
    def reference(x):
        return {"onset": onset(lib, x, True),
                "values": lib.beat.plp(y=x, sr=8000, hop_length=64, win_length=128, tempo_min=30, tempo_max=300)}
    yield Case("python.librosa.composition.pcm_pulse", "composition", "pcm_pulse", values,
               arguments(8000, 64, 128), reference,
               settings(8000, 64, 128, onset_aggregate="median", tempo_min=30, tempo_max=300))


def cases(lib):
    yield from metrical_cases(lib)
    yield from envelope_cases(lib)
    values = pcm_input()
    yield from pcm_tempo_cases(lib, values)
    yield from pcm_beat_cases(lib, values)
