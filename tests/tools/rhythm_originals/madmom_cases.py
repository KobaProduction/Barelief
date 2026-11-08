"""Original madmom ODFs/frames/peaks/histograms on explicit supplied data."""
import ast
import itertools
from pathlib import Path
import types
import numpy as np

from common import Case, SEED, envelope, tones


def spectral_cases(original):
    for size, n, shift, nyquist, kind in itertools.product((63, 64), (64, 128), (False, True), (False, True), ("tones", "silence")):
        values = tones(384, 8000) if kind == "tones" else np.zeros(384, np.float32)
        def reference(x, size=size, n=n, shift=shift, nyquist=nyquist):
            signal = original.signal.Signal(x, sample_rate=8000)
            frames = original.signal.FramedSignal(signal, frame_size=size, hop_size=17, origin=0, end="normal")
            z = original.stft.ShortTimeFourierTransform(frames, window=np.hanning, fft_size=n,
                                                       circular_shift=shift, include_nyquist=nyquist)
            magnitude = np.abs(z)
            return {"complex": np.stack((z.real, z.imag), axis=-1), "magnitude": magnitude,
                    "phase": np.angle(z), "powered": magnitude, "features": magnitude,
                    "window": np.hanning(size), "frequencies": np.fft.rfftfreq(n, 1 / 8000)[:z.shape[1]],
                    "source_filter_frequencies": z.bin_frequencies}
        yield Case(f"python.madmom.stft.m{size}.n{n}.shift{int(shift)}.nyquist{int(nyquist)}.{kind}", "spectrum", "stft", values,
                   [size, 17, n, size, 0, 1, 0, int(shift), int(nyquist), 0, 1, 8000], reference,
                   dict(fs=8000, length=size, hop=17, fft_size=n, window="symmetric Hann", circular_shift=shift, include_nyquist=nyquist, end="normal",
                        original_unsupported="odd frame circular shift uses unequal half slices" if size % 2 and shift else None))


def supplied_spectrogram(original, magnitude, phases):
    phase = np.asarray(phases).view(original.stft.Phase)
    phase.stft = types.SimpleNamespace(circular_shift=True)
    spectrum = np.asarray(magnitude).view(original.spectrogram.Spectrogram)
    spectrum.stft = types.SimpleNamespace(phase=lambda: phase)
    return spectrum


def integer_index_correlation(original):
    # Pinned function still has Python-2 integer-index syntax len(c)/2. The
    # only compatibility repair is that index division; all DSP statements stay.
    path = Path(original.onsets.__file__)
    node = next(value for value in ast.parse(path.read_text(encoding="utf8")).body
                if isinstance(value, ast.FunctionDef) and value.name == "correlation_diff")
    for statement in ast.walk(node):
        if isinstance(statement, ast.Assign) and any(isinstance(target, ast.Name) and target.id == "centre" for target in statement.targets):
            assert isinstance(statement.value, ast.BinOp) and isinstance(statement.value.op, ast.Div)
            statement.value.op = ast.FloorDiv()
    namespace = dict(original.onsets.__dict__)
    exec(compile(ast.Module(body=[node], type_ignores=[]), str(path), "exec"), namespace)
    return namespace["correlation_diff"]


def novelty_cases(original):
    random = np.random.default_rng(SEED)
    magnitude = random.random((48, 17)).astype(np.float32)
    magnitude[::7, :3] = 0
    phase = random.uniform(-np.pi, np.pi, magnitude.shape).astype(np.float32)
    phase[magnitude == 0] = 0
    names = ("high_frequency_content", "spectral_diff", "spectral_flux", "superflux", "complex_flux",
             "modified_kullback_leibler", "phase_deviation", "weighted_phase_deviation",
             "normalized_weighted_phase_deviation", "complex_domain", "rectified_complex_domain", "correlation_diff")
    correlation = integer_index_correlation(original)
    complex_input = (magnitude * np.exp(1j * phase)).astype(np.complex64).view(original.stft.ShortTimeFourierTransform)
    complex_input.circular_shift = True
    actual_spectrum = original.spectrogram.Spectrogram(complex_input)
    actual_phase = np.asarray(complex_input.phase())
    for index, name in enumerate(names):
        function = correlation if index == 11 else getattr(original.onsets, name)
        for lag in ((1, 3) if index in (1, 2, 3, 4, 5, 10, 11) else (1,)):
            def reference(x, index=index, function=function, lag=lag):
                spectrum = actual_spectrum if index == 4 else supplied_spectrogram(original, x, phase)
                np.testing.assert_array_equal(np.asarray(spectrum), x)
                kwargs = {"diff_frames": lag} if index in (1, 2, 3, 4, 5, 10, 11) else {}
                if index == 4:
                    kwargs.update(diff_max_bins=3, temporal_filter=3, temporal_origin=0)
                output = function(spectrum, **kwargs)
                return {"values": np.asarray(output).reshape(x.shape[0], -1)}
            yield Case(f"python.madmom.novelty.{name}.lag{lag}", "madmom", "novelty", np.asarray(actual_spectrum) if index == 4 else magnitude,
                       [index, lag, 3, 3, 0], reference, dict(method=name, lag=lag, frequency_maximum=3, temporal_maximum=3, temporal_origin=0),
                       {".phase": actual_phase if index == 4 else phase}, "original function; centre index //2 Python3 repair" if index == 11 else "unchanged original function with supplied Spectrogram/Phase views")


def histogram_cases(original):
    for method, channels in ((0, 1), (1, 1), (1, 3)):
        values = np.tile(envelope(192)[:, None], (1, channels))
        def reference(x, method=method):
            values = x[:, 0] if x.shape[1] == 1 else x
            bins, intervals = (original.tempo.interval_histogram_acf(values, min_tau=12, max_tau=60) if method == 0 else
                               original.tempo.interval_histogram_comb(values, alpha=.79, min_tau=12, max_tau=60))
            return {"values": np.asarray(bins).reshape(x.shape[1], -1), "intervals": intervals}
        yield Case(f"python.madmom.histogram.method{method}.channels{channels}", "madmom", "histogram", values,
                   [method, 12, 60, .79], reference, dict(method=method, minimum_interval=12, maximum_interval=60, alpha=.79, channels=channels, online=False))
    values = np.random.default_rng(SEED).random((16, 12)).astype(np.float32)
    for multiply, add in ((1, 1), (10, .1)):
        def reference(x, multiply=multiply, add=add):
            spectrum = supplied_spectrogram(original, x, np.zeros_like(x))
            return {"values": np.asarray(original.spectrogram.LogarithmicSpectrogram(spectrum, mul=multiply, add=add))}
        yield Case(f"python.madmom.logarithm.mul{multiply}.add{add}", "filters", "logarithm", values,
                   [multiply, add, 0], reference, dict(multiply=multiply, add=add, logarithm="base10"))


def cases(original):
    yield from spectral_cases(original)
    yield from novelty_cases(original)
    yield from histogram_cases(original)
