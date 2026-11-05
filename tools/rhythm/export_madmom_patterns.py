"""Convert classical madmom GMM pattern files into explicit Float32 model data.

No original Python class/module is executed. Only numerical NumPy reconstruction
and an inert GMM state container are admitted while decoding legacy pickle data.
This is an offline data adapter, not a parity runner or an inference runtime.
"""
from __future__ import annotations

import argparse
import hashlib
import io
import json
import pickle
from pathlib import Path

import numpy as np


class GmmState:
    """Inert storage for original GMM instance attributes."""


def reconstruct_object(cls, base, state):
    if cls is not GmmState or base is not object or state is not None:
        raise pickle.UnpicklingError("unsupported legacy object reconstruction")
    return object.__new__(GmmState)


class NumericalPatternUnpickler(pickle.Unpickler):
    def find_class(self, module, name):
        if (module, name) in {
            ("madmom.ml.gmm", "GMM"), ("sklearn.mixture.gmm", "GMM")
        }:
            return GmmState
        if module in {"numpy", "numpy.core", "numpy._core"}:
            if name == "ndarray":
                return np.ndarray
            if name == "dtype":
                return np.dtype
        if module in {"numpy.core.multiarray", "numpy._core.multiarray"}:
            if name in {"_reconstruct", "scalar"}:
                return getattr(np.core.multiarray, name)
        if module in {"numpy.core.numeric", "numpy._core.numeric"} and name == "_frombuffer":
            return np.core.numeric._frombuffer
        if module in {"copy_reg", "copyreg"} and name == "_reconstructor":
            return reconstruct_object
        if module in {"__builtin__", "builtins"} and name == "object":
            return object
        raise pickle.UnpicklingError(f"unsupported pattern global {module}.{name}")


def attribute(model, name):
    values = vars(model)
    return values[name] if name in values else values[name + "_"]


def vector(values):
    data = np.asarray(values, dtype=np.float32)
    if not np.isfinite(data).all():
        raise ValueError("nonfinite or Float32-overflowing parameter")
    return data.ravel(order="C").tolist()


def encode_gmm(model, identifier):
    if not isinstance(model, GmmState):
        raise ValueError("pattern contains a non-GMM object")
    means = np.asarray(attribute(model, "means"), dtype=np.float32)
    weights = np.asarray(attribute(model, "weights"), dtype=np.float32)
    covariances = np.asarray(attribute(model, "covars"), dtype=np.float32)
    kind = model.covariance_type
    if isinstance(kind, bytes):
        kind = kind.decode("ascii")
    if means.ndim != 2 or weights.shape != (means.shape[0],):
        raise ValueError("invalid means/weights shape")
    components, features = means.shape
    expected = {
        "diag": {(components, features)},
        "spherical": {(components,), (components, 1), (components, features)},
        "tied": {(features, features)},
        "full": {(components, features, features)},
    }
    if kind not in expected or covariances.shape not in expected[kind]:
        raise ValueError("invalid covariance shape/type")
    return {
        "id": identifier, "features": features, "components": components,
        "covariance_type": kind, "weights": vector(weights),
        "means": vector(means), "covariances": vector(covariances),
    }


def encode_pattern(path):
    raw = path.read_bytes()
    if len(raw) > 64 * 1024 * 1024:
        raise ValueError("pattern exceeds 64 MiB")
    pattern = NumericalPatternUnpickler(io.BytesIO(raw), encoding="latin1").load()
    if not isinstance(pattern, dict) or "gmms" not in pattern or "num_beats" not in pattern:
        raise ValueError("missing pattern data")
    digest = hashlib.sha256(raw).hexdigest()
    identifier = "sha256:" + digest
    beats = int(pattern["num_beats"])
    if beats < 1 or beats != pattern["num_beats"]:
        raise ValueError("invalid num_beats")
    gmms = [encode_gmm(gmm, f"{identifier}:gmm:{i}") for i, gmm in enumerate(pattern["gmms"])]
    if not gmms:
        raise ValueError("empty pattern")
    return {
        "id": identifier, "num_beats": beats, "gmms": gmms,
        "source": {"file": path.name, "sha256": digest},
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("patterns", nargs="+", type=Path)
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args()
    result = {
        "format": "barelief.rhythm-patterns", "version": 1, "dtype": "float32",
        "source_revision": "27f032e8947204902c675e5e341a3faf5dc86dae",
        "patterns": [encode_pattern(path) for path in args.patterns],
    }
    if len({item["id"] for item in result["patterns"]}) != len(result["patterns"]):
        raise ValueError("duplicate pattern inputs")
    with args.output.open("x", encoding="utf-8") as stream:
        json.dump(result, stream, ensure_ascii=False, allow_nan=False, separators=(",", ":"))
        stream.write("\n")


if __name__ == "__main__":
    main()
