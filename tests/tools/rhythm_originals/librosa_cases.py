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


def temporal_cases(lib):
    values = envelope(192)
    for method, width, center in itertools.product(range(3), (64, 128), (False, True)):
        def reference(x, method=method, width=width, center=center):
            kwargs = dict(onset_envelope=x, sr=100, hop_length=1, win_length=width, center=center, window="hann")
            if method == 0:
                output = lib.feature.tempogram(**kwargs, norm=np.inf).T
                coordinates = lib.tempo_frequencies(width, sr=100, hop_length=1)
            elif method == 1:
                output = split_complex(lib.feature.fourier_tempogram(**kwargs).T).reshape(-1, (width // 2 + 1) * 2)
                coordinates = lib.fourier_tempo_frequencies(sr=100, hop_length=1, win_length=width)
            else:
                output = lib.feature.hybrid_tempogram(**kwargs).T
                coordinates = lib.fourier_tempo_frequencies(sr=100, hop_length=1, win_length=width)
            stages = {"coordinates": coordinates}
            if method == 2:
                stages["acf_values"] = lib.feature.tempogram(**kwargs, norm=np.inf).T
                stages["fourier_values"] = split_complex(lib.feature.fourier_tempogram(**kwargs).T).reshape(-1, (width // 2 + 1) * 2)[:output.shape[0]]
                stages["acf_values"] = stages["acf_values"][:output.shape[0]]
            stages["values"] = output
            return stages
        yield Case(f"python.librosa.tempogram.method{method}.n{width}.center{int(center)}", "librosa", "tempogram", values,
                   [method, 100, width, 1, int(center), 1], reference,
                   dict(method=method, frame_rate=100, length=width, hop=1, periodic=True, center=center, normalization="maximum"))


def envelope_cases(lib):
    values = envelope(384)
    for rate, backtrack in itertools.product((43.06640625, 100), (False, True)):
        def reference(x, rate=rate, backtrack=backtrack):
            return {"events": lib.onset.onset_detect(onset_envelope=x, sr=rate * 512, hop_length=512,
                                                     backtrack=backtrack, energy=x, units="frames", sparse=True)}
        yield Case(f"python.librosa.detect.fps{rate}.backtrack{int(backtrack)}", "librosa", "detect", values,
                   [rate * 512, 512, int(backtrack)], reference, dict(frame_rate=rate, normalize=True, sparse=True, backtrack=backtrack))
    for bpm, trim in itertools.product((90, 120), (False, True)):
        def reference(x, bpm=bpm, trim=trim):
            _, mask = lib.beat.beat_track(onset_envelope=x, sr=100, hop_length=1, bpm=np.array([bpm], np.float32), tightness=100, trim=trim, sparse=False)
            return {"mask": mask}
        yield Case(f"python.librosa.beat.bpm{bpm}.trim{int(trim)}", "librosa", "beat", values,
                   [100, bpm, 100, int(trim)], reference, dict(frame_rate=100, supplied_bpm=bpm, tightness=100, trim=trim, sparse=False))
    def pulse_reference(x):
        return {"values": lib.beat.plp(onset_envelope=x, sr=100, hop_length=1, win_length=128, tempo_min=30, tempo_max=300)}
    yield Case("python.librosa.plp", "librosa", "pulse", values, [100, 1, 128, 30, 300], pulse_reference,
               dict(frame_rate=100, length=128, min_bpm=30, max_bpm=300))


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
    for aggregate in (None, np.mean, np.median, np.sum):
        mode = (None, np.mean, np.median, np.sum).index(aggregate)
        def reference(x, aggregate=aggregate):
            output = lib.feature.tempogram_ratio(tg=x.T, sr=100, hop_length=1, bpm=np.array([120], np.float32),
                                                factors=np.array([.5, 1, 2, 3, 4], np.float32), aggregate=aggregate)
            return {"values": np.asarray(output).reshape(5, -1).T}
        yield Case(f"python.librosa.ratios.aggregate{mode}", "librosa", "ratios", values,
                   [100, 120, mode], reference, dict(frame_rate=100, supplied_bpm=120, factors=[.5, 1, 2, 3, 4], aggregate=mode))


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


def pcm_cases(lib):
    values = tones(8192, 16000)
    for aggregate, maximum in itertools.product((0, 1), (1, 3)):
        function = (np.mean, np.median)[aggregate]
        def reference(x, function=function, maximum=maximum):
            z = lib.stft(x, n_fft=256, hop_length=64, win_length=256, window="hann", center=True, pad_mode="constant").T
            magnitude = np.abs(z)
            mel = lib.feature.melspectrogram(S=(magnitude**2).T, sr=16000, n_fft=256, n_mels=128, fmin=0, fmax=8000)
            return {"complex": split_complex(z), "magnitude": magnitude, "powered": magnitude**2,
                    "features": mel.T, "db": lib.power_to_db(mel).T,
                    "values": lib.onset.onset_strength(y=x, sr=16000, n_fft=256, hop_length=64,
                                                       aggregate=function, max_size=maximum, center=True, detrend=False).reshape(-1, 1)}
        yield Case(f"python.librosa.pcm_strength.a{aggregate}.max{maximum}", "librosa", "pcm_strength", values,
                   [16000, 256, 64, aggregate, maximum, 0], reference,
                   dict(fs=16000, n_fft=256, hop=64, periodic=True, nyquist=True, power=2, aggregate=function.__name__, max_size=maximum))


def cases(lib):
    yield from spectral_cases(lib)
    yield from strength_cases(lib)
    yield from temporal_cases(lib)
    yield from envelope_cases(lib)
    yield from tempo_cases(lib)
    yield from filter_cases(lib)
    yield from pcm_cases(lib)
