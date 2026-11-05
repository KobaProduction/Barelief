"""Actual original PCM onset and complete tempo/interval tracking processors."""
import numpy as np

from common import Case, envelope, tones
from madmom_cases import integer_index_correlation


class Capture:
    def __init__(self, target):
        self.target, self.value = target, None

    def __call__(self, data, **kwargs):
        self.value = self.target(data, **kwargs)
        return self.value

    def __getattr__(self, name):
        return getattr(self.target, name)


def pcm_novelty_cases(original):
    names = ("high_frequency_content", "spectral_diff", "spectral_flux", "superflux", "complex_flux",
             "modified_kullback_leibler", "phase_deviation", "weighted_phase_deviation",
             "normalized_weighted_phase_deviation", "complex_domain", "rectified_complex_domain", "correlation_diff")
    values = tones(1024, 8000)
    for index, name in enumerate(names):
        def reference(x, name=name, index=index):
            processor = original.onsets.SpectralOnsetProcessor(onset_method=name, sample_rate=8000,
                                                               frame_size=64, fft_size=64, hop_size=17,
                                                               window=np.hanning, origin=0, end="normal",
                                                               include_nyquist=False, norm=False, gain=0)
            if index == 11:
                processor.processors[-1] = integer_index_correlation(original)
            stft, magnitude = Capture(processor.processors[2]), Capture(processor.processors[3])
            processor.processors[2], processor.processors[3] = stft, magnitude
            output = np.asarray(processor(x))
            z, mag = stft.value, np.asarray(magnitude.value)
            return {"values": output.reshape(mag.shape[0], -1), "complex": np.stack((z.real, z.imag), axis=-1),
                    "magnitude": mag, "phase": np.asarray(z.phase()), "features": mag}
        yield Case("python.madmom.remaining.pcm_novelty." + name, "remaining", "pcm_novelty", values,
                   [index], reference,
                   dict(fs=8000, frame_size=64, n_fft=64, hop=17, window="symmetric Hann", origin=0,
                        end="normal", include_nyquist=False, filtering=None, logarithm=None,
                        onset_method=name, automatic_source_overlap_lag=True, diff_ratio=.5),
                   policy="unchanged original SpectralOnsetProcessor; correlation centre index //2 Python3 repair" if index == 11 else "unchanged original SpectralOnsetProcessor with nonmutating stage taps")


def analysis_outputs(original, processor, capture, values, online, suffix, reset):
    output = processor.process_online(values, reset=reset) if online else processor.process_offline(values)
    raw, coordinates = capture.value
    smoothed, _ = original.tempo.smooth_histogram((raw, coordinates), 3)
    return {"values" + suffix: np.asarray(output), "raw" + suffix: np.asarray(raw).copy(),
            "smoothed" + suffix: np.asarray(smoothed), "coordinates" + suffix: np.asarray(coordinates)}


def analysis_cases(original):
    values = envelope(192)
    for index, method in enumerate(("acf", "comb", "dbn")):
        for online in (False, True):
            def reference(x, method=method, online=online):
                processor = original.tempo.TempoEstimationProcessor(method=method, min_bpm=100, max_bpm=150,
                                                                    fps=50, online=online, act_smooth=.04,
                                                                    hist_smooth=3, hist_buffer=2, alpha=.79,
                                                                    num_tempi=None, interpolate=False)
                capture = Capture(processor.histogram_processor)
                processor.histogram_processor = capture
                if not online:
                    return analysis_outputs(original, processor, capture, x, False, "", True)
                first = analysis_outputs(original, processor, capture, x[:64], True, "0", True)
                return {**first, **analysis_outputs(original, processor, capture, x[64:], True, "1", False)}
            yield Case(f"python.madmom.remaining.analysis.{method}.online{int(online)}", "remaining", "analysis", values,
                       [index, int(online), 64], reference,
                       dict(method=method, fps=50, min_bpm=100, max_bpm=150, online=online,
                            activation_smoothing_seconds=.04, histogram_smoothing=3, history_seconds=2,
                            alpha=.79, num_tempi=None, interpolate=False,
                            chunks=[64, 128] if online else [192], reset=[True, False] if online else None,
                            neural_network=False, supplied_activations=True))


def tracking_cases(original):
    values = envelope(192)
    for variable in (False, True):
        def reference(x, variable=variable):
            tempo = original.tempo.TempoEstimationProcessor(method="comb", min_bpm=100, max_bpm=150,
                                                           fps=50, act_smooth=.04, hist_smooth=3, hist_buffer=2, alpha=.79)
            processor = original.beats.BeatTrackingProcessor(fps=50, look_aside=.2,
                                                           look_ahead=.5 if variable else None, tempo_estimator=tempo)
            times = processor.process(x)
            return {"times": times, "positions": np.rint(times * 50).astype(np.uint64)}
        yield Case(f"python.madmom.remaining.interval_tracking.variable{int(variable)}", "remaining", "tracking",
                   values, [int(variable)], reference,
                   dict(variable_tempo=variable, fps=50, min_bpm=100, max_bpm=150, method="comb", alpha=.79,
                        activation_smoothing_seconds=.04, histogram_smoothing=3, history_seconds=2,
                        look_aside=.2, look_ahead_seconds=.5 if variable else None, supplied_activations=True, neural_network=False))


def cases(original):
    yield from pcm_novelty_cases(original)
    yield from analysis_cases(original)
    yield from tracking_cases(original)
