"""Actual source backtracking and downstream PLP from explicit complex rows."""
import types
import numpy as np

from common import Case, SEED, envelope


def supplied_fourier_plp(lib, values, samples, prior=None):
    spectrum = (values[:, 0::2] + 1j * values[:, 1::2]).astype(np.complex64).T
    function = lib.beat.plp
    namespace = dict(function.__globals__)
    namespace["fourier_tempogram"] = lambda **kwargs: spectrum.copy()
    actual_body = types.FunctionType(function.__code__, namespace, function.__name__, function.__defaults__, function.__closure__)
    actual_body.__kwdefaults__ = function.__kwdefaults__
    return actual_body(onset_envelope=np.zeros(samples, np.float32), sr=100, hop_length=1,
                       win_length=64, tempo_min=30, tempo_max=300, prior=prior)


def cases(lib):
    energy = envelope(96)
    for name, events in (("ordered", [3, 12, 27, 46, 95]), ("unsorted_repeated", [46, 3, 46, 95, 7])):
        indices = np.asarray(events, np.float32)
        def reference(x, indices=indices):
            return {"events": lib.onset.onset_backtrack(indices.astype(np.int64), x)}
        yield Case("python.librosa.remaining.backtrack." + name, "remaining", "backtrack", energy,
                   [indices.size], reference, dict(indices=events, sparse=True, repeated_indices_preserved=True), {".events": indices})
    random = np.random.default_rng(SEED + 31)
    for samples in (128, 173):
        complex_rows = (random.normal(size=(samples + 1, 33)) + 1j * random.normal(size=(samples + 1, 33))).astype(np.complex64)
        complex_rows[:, (0, -1)] = complex_rows[:, (0, -1)].real
        values = np.stack((complex_rows.real, complex_rows.imag), axis=-1).reshape(samples + 1, 66)
        def reference(x, samples=samples):
            return {"values": supplied_fourier_plp(lib, x, samples)}
        yield Case(f"python.librosa.remaining.local_pulse.samples{samples}", "remaining", "local_pulse", values,
                   [samples, 64, 100], reference,
                   dict(samples=samples, frame_rate=100, window_length=64, hop=1, center=True,
                        window="periodic Hann", input="explicit Complex64 one-sided Fourier rows incl DC/Nyquist",
                        tempo_min=30, tempo_max=300, seed=SEED + 31),
                   policy="unchanged pinned plp function body; only its upstream fourier_tempogram provider supplies the explicit identical Complex64 input")
