"""Predeclared held-out same-input cases for both explicit precision profiles."""
import numpy as np

from common import Case, SEED, tones
from librosa_cases import split_complex


def cases(lib):
    random = np.random.default_rng(SEED + 17)
    envelope = (.02 + random.random(173)**3).astype(np.float32)
    for center in (False, True):
        def reference(x, center=center):
            kwargs = dict(onset_envelope=x, sr=125, hop_length=1, win_length=96, center=center, window="hann")
            output = lib.feature.hybrid_tempogram(**kwargs).T
            return {"values": output,
                    "acf_values": lib.feature.tempogram(**kwargs, norm=np.inf).T[:output.shape[0]],
                    "fourier_values": split_complex(lib.feature.fourier_tempogram(**kwargs).T).reshape(-1, 98)[:output.shape[0]],
                    "coordinates": lib.fourier_tempo_frequencies(sr=125, hop_length=1, win_length=96)}
        yield Case(f"python.librosa.precision_holdout.hybrid.n96.center{int(center)}", "librosa", "tempogram",
                   envelope, [2, 125, 96, 1, int(center), 1], reference,
                   dict(method=2, frame_rate=125, length=96, hop=1, periodic=True, center=center,
                        normalization="maximum", seed=SEED + 17))
    pcm = tones(10321, 22050)
    for mode, aggregate in enumerate((np.mean, np.median)):
        def reference(x, aggregate=aggregate):
            z = lib.stft(x, n_fft=512, hop_length=128, window="hann", center=True).T
            magnitude = np.abs(z)
            mel = lib.feature.melspectrogram(S=(magnitude**2).T, sr=22050, n_fft=512, n_mels=128)
            return {"complex": split_complex(z), "magnitude": magnitude, "powered": magnitude**2,
                    "features": mel.T, "db": lib.power_to_db(mel).T,
                    "values": lib.onset.onset_strength(y=x, sr=22050, n_fft=512, hop_length=128,
                                                       aggregate=aggregate, max_size=3).reshape(-1, 1)}
        yield Case(f"python.librosa.precision_holdout.pcm.n512.aggregate{mode}", "librosa", "pcm_strength",
                   pcm, [22050, 512, 128, mode, 3, 0], reference,
                   dict(fs=22050, n_fft=512, hop=128, power=2, aggregate=aggregate.__name__,
                        max_size=3, center=True, periodic=True, nyquist=True, seed=SEED))
