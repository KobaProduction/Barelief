"""Finite original ACF/comb history4096, induction and explicit reset cases."""
import numpy as np

from common import Case, envelope
from remaining_madmom_cases import Capture, analysis_outputs


def cases(original):
    values = np.repeat(envelope(2304), 2)
    modes = (("history4096", [512, 1024, 2048, 1024], [True, False, False, False]),
             ("induction", [8, 24, 64, 128, 4384], [True, False, False, False, False]),
             ("buffer_reset", [4096, 256, 256], [True, False, True]))
    for index, method in enumerate(("acf", "comb")):
        for name, chunks, resets in modes:
            def reference(x, method=method, chunks=chunks, resets=resets):
                processor = original.tempo.TempoEstimationProcessor(method=method, min_bpm=100, max_bpm=150,
                                                                    fps=100, online=True, act_smooth=.02,
                                                                    hist_smooth=3, hist_buffer=40.96, alpha=.79,
                                                                    interpolate=False, num_tempi=None)
                capture = Capture(processor.histogram_processor)
                processor.histogram_processor = capture
                output, first = {}, 0
                for step, (count, reset) in enumerate(zip(chunks, resets)):
                    output.update(analysis_outputs(original, processor, capture, x[first:first + count],
                                                   True, str(step), reset))
                    first += count
                return output

            yield Case(f"python.madmom.lifecycle.analysis.{method}.{name}", "remaining", "analysis_lifecycle",
                       values, [index, len(chunks)], reference,
                       dict(method=method, fps=100, min_bpm=100, max_bpm=150, online=True,
                            history_seconds=40.96, history_frames=4096, input_frames=4608,
                            activation_smoothing_seconds=.02, histogram_smoothing=3, alpha=.79,
                            chunks=chunks, resets=resets, supplied_activations=True, neural_network=False),
                       {".chunks": np.asarray(chunks, np.float32), ".resets": np.asarray(resets, np.float32)})
