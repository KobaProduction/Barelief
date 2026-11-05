"""Import pinned originals while keeping neural-network aggregation out of scope."""
import ast
import hashlib
import importlib
import importlib.metadata as metadata
from pathlib import Path
import sys
import types

PINS = {"librosa": "3e4ff7bf5898ac5b326e7dee56463b4b325b91cb",
        "madmom": "27f032e8947204902c675e5e341a3faf5dc86dae"}


def librosa(cache):
    module = importlib.import_module("librosa")
    root = Path(cache) / "librosa-3e4ff7bf"
    distribution = metadata.distribution("librosa")
    for source in (root / "librosa").rglob("*.py"):
        installed = Path(distribution.locate_file(str(source.relative_to(root))))
        assert hashlib.sha256(source.read_bytes()).digest() == hashlib.sha256(installed.read_bytes()).digest(), str(source)
    return module


def madmom(cache):
    root = Path(cache)
    source, built = root / "madmom-27f032e8", root / "madmom-build"
    for name in ("madmom", "madmom.audio", "madmom.features", "madmom.ml"):
        module = types.ModuleType(name)
        module.__path__ = [str(built.joinpath(*name.split("."))), str(source.joinpath(*name.split(".")))]
        sys.modules[name] = module
        if "." in name:
            parent, leaf = name.rsplit(".", 1)
            setattr(sys.modules[parent], leaf, module)
    # beats.py imports this pure helper at module scope. Load its unchanged AST;
    # importing the aggregate NN package would load excluded neural layers/models.
    nn = types.ModuleType("madmom.ml.nn")
    nn.np = importlib.import_module("numpy")
    path = source / "madmom/ml/nn/__init__.py"
    nodes = [node for node in ast.parse(path.read_text(encoding="utf8")).body
             if isinstance(node, ast.FunctionDef) and node.name == "average_predictions"]
    exec(compile(ast.Module(body=nodes, type_ignores=[]), str(path), "exec"), nn.__dict__)
    sys.modules[nn.__name__] = nn
    sys.modules["madmom.ml"].nn = nn
    names = {"signal": "audio.signal", "stft": "audio.stft", "spectrogram": "audio.spectrogram",
             "onsets": "features.onsets", "tempo": "features.tempo", "beats": "features.beats",
             "beats_hmm": "features.beats_hmm", "hmm": "ml.hmm", "gmm": "ml.gmm",
             "crf": "features.beats_crf"}
    return types.SimpleNamespace(**{name: importlib.import_module("madmom." + path) for name, path in names.items()})


def source_record(library):
    return {"library": library, "revision": PINS[library],
            "route": "actual pinned Python modules" if library == "librosa" else
            "actual pinned Python modules and source-built original Cython extensions; NN initializers bypassed"}
