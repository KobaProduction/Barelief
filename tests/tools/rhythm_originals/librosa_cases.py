"""Actual librosa calls against source-port stages with explicit settings."""
import itertools
import numpy as np

from common import Case, SEED, envelope, tones


def split_complex(values):
    return np.stack((values.real, values.imag), axis=-1)


def spectral_cases(lib):
    for n, m, center, kind in itertools.product((64, 128), (32, 63), (False, True), ("tones", "impulse", "silence")):
        values = tones(512, 8000) if kind == "tones" else np.zeros(512, np.float32)
        if kind == "impulse":
            values[[0, 127, -1]] = (.25, -.5, .75)
        hop = 17
        def reference(x, n=n, m=m, center=center, hop=hop):
            z = lib.stft(x, n_fft=n, hop_length=hop, win_length=m, window="hann", center=center, pad_mode="constant").T
            magnitude = np.abs(z)
            window = lib.util.pad_center(lib.filters.get_window("hann", m, fftbins=True), size=n)
            return {"complex": split_complex(z), "magnitude": magnitude, "phase": np.angle(z),
                    "powered": magnitude, "linear": magnitude, "features": magnitude,
                    "window": window, "frequencies": lib.fft_frequencies(sr=8000, n_fft=n)}
        settings = dict(fs=8000, n=n, m=m, hop=hop, window="hann", periodic=True, center=center, tail="complete", power=1, dc=True, nyquist=True)
        yield Case(f"python.librosa.stft.n{n}.m{m}.center{int(center)}.{kind}", "spectrum", "stft", values,
                   [n, hop, n, m, 1, int(center), 2, 0, 1, 0, 1, 8000], reference, settings)


def strength_cases(lib):
    random = np.random.default_rng(SEED)
    values = (10 * random.random((96, 17)) + np.sin(np.arange(96)[:, None] * .2)).astype(np.float32)
    for aggregate, maximum, center, detrend in itertools.product(range(3), (1, 3), (False, True), (False, True)):
        function = (np.mean, np.median, np.sum)[aggregate]
        def reference(x, function=function, maximum=maximum, center=center, detrend=detrend):
            output = lib.onset.onset_strength(S=x.T, sr=16000, hop_length=64, n_fft=256,
                                             lag=2, max_size=maximum, center=center, detrend=detrend, aggregate=function)
            return {"values": output}
        yield Case(f"python.librosa.strength.a{aggregate}.max{maximum}.c{int(center)}.d{int(detrend)}", "librosa", "strength", values,
                   [2, maximum, int(center), int(detrend), aggregate, 256, 64], reference,
                   dict(lag=2, max_size=maximum, center=center, detrend=detrend, aggregate=function.__name__, n_fft=256, hop_length=64))




def envelope_cases(lib):
    values = envelope(384)
    for bpm, trim in itertools.product((90, 120), (False, True)):
        def reference(x, bpm=bpm, trim=trim):
            _, mask = lib.beat.beat_track(onset_envelope=x, sr=100, hop_length=1, bpm=np.array([bpm], np.float32), tightness=100, trim=trim, sparse=False)
            return {"mask": mask}
        yield Case(f"python.librosa.beat.bpm{bpm}.trim{int(trim)}", "librosa", "beat", values,
                   [100, bpm, 100, int(trim)], reference, dict(frame_rate=100, supplied_bpm=bpm, tightness=100, trim=trim, sparse=False))


def tempo_cases(lib):
    random = np.random.default_rng(SEED)
    lag = np.arange(128)
    values = np.asarray([np.exp(-.5 * ((lag - 50 - .8 * np.sin(time / 7)) / 2)**2) +
                         .3 * np.exp(-.5 * ((lag - 25) / 2)**2) + .005 * random.random(128) for time in range(32)], np.float32)
    for aggregate in range(3):
        function = (None, np.mean, np.median)[aggregate]
        def reference(x, function=function):
            return {"values": lib.feature.tempo(tg=x.T, sr=100, hop_length=1, start_bpm=120, std_bpm=1, max_tempo=320, aggregate=function).reshape(-1)}
        yield Case(f"python.librosa.tempo.aggregate{aggregate}", "librosa", "tempo", values,
                   [100, 120, 1, 320, aggregate], reference, dict(frame_rate=100, start_bpm=120, std_bpm=1, max_tempo=320, aggregate=aggregate))



def filter_cases(lib):
    random = np.random.default_rng(SEED)
    values = random.random((12, 129)).astype(np.float32)
    for htk, norm in itertools.product((False, True), (None, "slaney")):
        def reference(x, htk=htk, norm=norm):
            weights = lib.filters.mel(sr=16000, n_fft=256, n_mels=16, fmin=30, fmax=8000, htk=htk, norm=norm, dtype=np.float32)
            return {"weights": weights, "values": x @ weights.T}
        yield Case(f"python.librosa.mel.htk{int(htk)}.norm{norm}", "filters", "mel", values,
                   [16000, 256, 16, 30, 8000, int(not htk), 1 if norm is None else 0], reference,
                   dict(fs=16000, n_fft=256, bands=16, fmin=30, fmax=8000, htk=htk, norm=norm))
    for ref in (1, np.max):
        def reference(x, ref=ref):
            return {"values": lib.power_to_db(x, ref=ref, amin=1e-10, top_db=80)}
        yield Case(f"python.librosa.power_to_db.ref{int(ref is np.max)}", "filters", "decibels", values,
                   [1, 1e-10, 80, int(ref is np.max), 1], reference, dict(ref="maximum" if ref is np.max else 1, amin=1e-10, top_db=80))




def cases(lib):
    yield from spectral_cases(lib)
    yield from strength_cases(lib)
    yield from envelope_cases(lib)
    yield from tempo_cases(lib)
    yield from filter_cases(lib)
