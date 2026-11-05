"""Defined-memory COPY of pinned CRF, preserving all selected valid scores.

Integration owner builds/runs this separately from raw and diagnostic binaries.
No epsilon, normalization, fallback decoder, or paper-equation rewrite is used.
"""
import argparse
import ast
import hashlib
import importlib
import json
from pathlib import Path
import subprocess
import sys
import textwrap
import types

import madmom_crf_diagnose as diagnosis


def safe_source(source):
    replace = diagnosis.replace_once
    source = replace(source, "@cython.boundscheck(False)", "@cython.boundscheck(True)")
    source = replace(source, "    init = initial_distribution(activations.shape[0],",
        "    if activations.ndim != 1 or not np.isfinite(activations).all() or (activations < 0).any():\n"
        "        raise ValueError('CRF needs finite nonnegative one-dimensional activations')\n"
        "    if interval < 1 or int(interval) != interval or not np.isfinite(interval_sigma) or interval_sigma <= 0:\n"
        "        raise ValueError('CRF needs positive integer interval and finite positive sigma')\n"
        "    if activations.shape[0] < interval:\n"
        "        return np.empty(0, dtype=int), -np.inf\n"
        "    init = initial_distribution(activations.shape[0],")
    source = replace(source, "    cdef int num_x = num_st / tau",
        "    if tau < 1:\n        raise ValueError('CRF interval must be positive')\n"
        "    cdef int num_x = num_st / tau\n"
        "    if num_x < 1:\n        return np.empty(0, dtype=int), -np.inf")
    source = replace(source, "bps = np.empty((num_x - 1, num_st), dtype=int)",
                     "bps = np.full((num_x - 1, num_st), -1, dtype=int)")
    source = replace(source, "cdef int k, i, j, next_state", "cdef int k, i, j, next_state = -1")
    source = replace(source, "    path[num_x - 1] = next_state",
        "    if next_state < 0 or not np.isfinite(path_prob):\n"
        "        return np.empty(0, dtype=int), -np.inf\n"
        "    path[num_x - 1] = next_state")
    source = replace(source, "        next_state = bps[i, next_state]",
        "        if next_state < 0 or next_state >= num_st:\n"
        "            return np.empty(0, dtype=int), -np.inf\n"
        "        next_state = bps[i, next_state]\n"
        "        if next_state < 0 or next_state >= num_st:\n"
        "            return np.empty(0, dtype=int), -np.inf")
    return source


def process_source(cache):
    path = cache / "madmom-27f032e8/madmom/features/beats.py"
    source = path.read_text(encoding="utf8")
    tree = ast.parse(source)
    processor = next(node for node in tree.body if isinstance(node, ast.ClassDef)
                     and node.name == "CRFBeatDetectionProcessor")
    process = next(node for node in processor.body if isinstance(node, ast.FunctionDef) and node.name == "process")
    function = textwrap.dedent("\n".join(source.splitlines()[process.lineno - 1:process.end_lineno]))
    function = diagnosis.replace_once(function, "    import itertools as it",
        "    _validate_input(activations)\n"
        "    if not len(activations):\n        return np.empty(0, dtype=float)\n"
        "    import itertools as it")
    function = diagnosis.replace_once(function, "    tempi = self.tempo_estimator.process(activations)",
        "    tempi = self.tempo_estimator.process(activations)\n"
        "    if not len(tempi) or not np.isfinite(tempi[:, 0]).any():\n"
        "        return np.empty(0, dtype=float)")
    function = diagnosis.replace_once(function,
        "    normalized_seq_probabilities = np.array([r[1] / r[0].shape[0]\n"
        "                                             for r in results])",
        "    normalized_seq_probabilities = _defined_scores(results)\n"
        "    if not np.isfinite(normalized_seq_probabilities).any():\n"
        "        return np.empty(0, dtype=float)")
    return function


def build(cache, output):
    source, digest = diagnosis.pinned_source(cache)
    output.mkdir(parents=True, exist_ok=False)
    module = output / "madmom_crf_defined.pyx"
    module.write_text(safe_source(source), encoding="utf8")
    wrapper = output / "defined_process.py"
    wrapper.write_text(process_source(cache), encoding="utf8")
    (output / "manifest.json").write_text(json.dumps({
        "pin": diagnosis.PINNED_REVISION, "pinned_crf_sha256": digest,
        "safe_source_sha256": hashlib.sha256(module.read_bytes()).hexdigest(),
        "wrapper_sha256": hashlib.sha256(wrapper.read_bytes()).hexdigest(),
        "generator_sha256": hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),
        "policy": "unreachable/short trial: empty/-Inf; ordered finite nonempty trials only; valid arithmetic unchanged",
    }, indent=2), encoding="utf8")
    setup = output / "setup_defined.py"
    setup.write_text('''from pathlib import Path
from setuptools import Extension, setup
from Cython.Build import cythonize
import numpy
root = Path(__file__).resolve().parent
setup(name="madmom-crf-defined", ext_modules=cythonize(
    [Extension("madmom_crf_defined", [str(root / "madmom_crf_defined.pyx")],
               include_dirs=[numpy.get_include()])],
    build_dir=str(root / "generated"), compiler_directives={"language_level": 3}),
    script_args=["build_ext", "--build-lib", str(root), "--build-temp", str(root / "objects")])
''', encoding="utf8")
    with (output / "build.log").open("xb") as log:
        completed = subprocess.run([sys.executable, str(setup)], stdout=log, stderr=subprocess.STDOUT)
    if completed.returncode:
        raise RuntimeError(f"Defined-source build failed: {output / 'build.log'}")
    binaries = list(output.glob("madmom_crf_defined*.pyd"))
    if len(binaries) != 1:
        raise RuntimeError("Expected exactly one Windows defined-source binary")
    (output / "binary.json").write_text(json.dumps({"path": str(binaries[0].resolve()),
        "sha256": hashlib.sha256(binaries[0].read_bytes()).hexdigest()}, indent=2), encoding="utf8")


def load_variant(root):
    manifest = json.loads((root / "manifest.json").read_text(encoding="utf8"))
    for path, key in ((root / "madmom_crf_defined.pyx", "safe_source_sha256"),
                      (root / "defined_process.py", "wrapper_sha256")):
        if hashlib.sha256(path.read_bytes()).hexdigest() != manifest[key]:
            raise ValueError(f"Changed defined-source artifact: {path}")
    binary = json.loads((root / "binary.json").read_text(encoding="utf8"))
    path = Path(binary["path"])
    if hashlib.sha256(path.read_bytes()).hexdigest() != binary["sha256"]:
        raise ValueError("Changed defined-source binary")
    sys.path.insert(0, str(root.resolve()))
    module = importlib.import_module("madmom_crf_defined")
    if Path(module.__file__).resolve() != path.resolve():
        raise ValueError("Wrong defined-source module imported")
    return module


def validate_input(activations):
    import numpy as np
    values = np.asarray(activations)
    if values.ndim != 1 or not np.isfinite(values).all() or (values < 0).any():
        raise ValueError("CRF needs finite nonnegative one-dimensional activations")


def defined_scores(results):
    import numpy as np
    # Python division is intentionally Float64 as in pinned beats.py. Other
    # unreachable DP states may contain NaN; only the SELECTED result matters.
    return np.asarray([score / path.shape[0] if path.shape[0] and np.isfinite(score)
                       else -np.inf for path, score in results], dtype=np.float64)


def processor(original, module, module_root, report=None):
    import numpy as np
    import madmom_pipeline_oracle as oracle
    value = oracle.tracker(original, 2, oracle.tempo_processor(original, 2))
    trials = [] if report is None else report

    def trial_call(trial):
        path, score = module.best_sequence(*trial)
        trials.append({"interval": int(trial[1]), "count": int(path.size),
                       "score": float(score), "normalized_score": float(score / path.size) if path.size else -np.inf,
                       "status": "VALID" if path.size and np.isfinite(score) else "NO_RESULT",
                       "positions": path.tolist()})
        return path, score

    namespace = dict(vars(original.beats))
    namespace.update(_validate_input=validate_input, _defined_scores=defined_scores, _process_crf=trial_call)
    path = module_root / "defined_process.py"
    exec(compile(path.read_text(encoding="utf8"), str(path), "exec"), namespace)
    value.process = types.MethodType(namespace["process"], value)
    return value


def run(args):
    import numpy as np
    diagnosis.pinned_source(args.cache)
    args.output.mkdir(parents=True, exist_ok=False)
    original = diagnosis.load_original(args.cache)
    module = load_variant(args.module_root)
    values = np.load(args.envelope, allow_pickle=False)
    trials = []
    result = processor(original, module, args.module_root, trials).process(values)
    with (args.output / "events.npy").open("xb") as output:
        np.save(output, result, allow_pickle=False)
    record = {"variant": "DEFINED_SOURCE_COPY", "pin": diagnosis.PINNED_REVISION,
              "input_sha256": hashlib.sha256(args.envelope.read_bytes()).hexdigest(),
              "raw_binary_sha256": hashlib.sha256(Path(original.crf.__file__).read_bytes()).hexdigest(),
              "status": "VALID" if result.size else "NO_RESULT", "events": result.tolist(), "trials": trials,
              "manifest": json.loads((args.module_root / "manifest.json").read_text(encoding="utf8"))}
    (args.output / "result.json").write_text(json.dumps(record, indent=2), encoding="utf8")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest="command", required=True)
    sub = commands.add_parser("build")
    sub.add_argument("cache", type=Path)
    sub.add_argument("output", type=Path)
    sub = commands.add_parser("run")
    for name in ("cache", "module_root", "envelope", "output"):
        sub.add_argument(name, type=Path)
    args = parser.parse_args()
    if args.command == "build":
        build(args.cache, args.output)
    else:
        run(args)


if __name__ == "__main__":
    main()
