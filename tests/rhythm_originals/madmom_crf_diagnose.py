"""Build/run an isolated, source-derived CRF diagnostic; integration owner only.

Never changes the pin, its installed extension, or the existing raw oracle.
Commands create exclusive output directories and retain native crash evidence.
"""
import argparse
import hashlib
import importlib
import json
import os
from pathlib import Path
import subprocess
import sys

REPO = Path(__file__).resolve().parents[2]
PIN_ROOT = REPO / ".agent-workspace/rhythm-pc-max/upstream-full/madmom-27f032e8"
PINNED_REVISION = "27f032e8947204902c675e5e341a3faf5dc86dae"
SOURCE_NAME = "madmom/features/beats_crf.pyx"


def pinned_source(cache):
    pin = json.loads((PIN_ROOT / "PIN.json").read_text(encoding="utf8"))
    if pin["revision"] != PINNED_REVISION:
        raise ValueError("Unexpected CRF authority revision")
    expected = pin["sha256"][SOURCE_NAME]
    for name, digest in pin["sha256"].items():
        for root in (PIN_ROOT, cache / "madmom-27f032e8"):
            path = root / name
            if hashlib.sha256(path.read_bytes()).hexdigest() != digest:
                raise ValueError(f"Changed immutable source: {path}")
    return (PIN_ROOT / SOURCE_NAME).read_text(encoding="utf8"), expected


def replace_once(source, old, new):
    if source.count(old) != 1:
        raise ValueError(f"Source patch does not match pin: {old!r}")
    return source.replace(old, new, 1)


OBSERVER = '''
# Offline diagnostics added to a COPY, never the source authority.
import json as _json
from pathlib import Path as _Path
TRACE_ROOT = None
TRIAL_COUNTER = 0
CURRENT_TRIAL = None

def _record(event, **fields):
    if CURRENT_TRIAL is None:
        raise RuntimeError("Diagnostic trace directory not configured")
    with (CURRENT_TRIAL / "trace.jsonl").open("a", encoding="utf8") as output:
        output.write(_json.dumps(dict(event=event, **fields)) + "\\n")
        output.flush()

def _fault(reason, **fields):
    _record("FIRST_ILLEGAL_OPERATION", reason=reason, **fields)
    raise RuntimeError("CRF diagnostic: " + reason)

def _begin(pi, transition, norm_factor, activations, tau):
    global CURRENT_TRIAL, TRIAL_COUNTER
    if TRACE_ROOT is None:
        raise RuntimeError("Set TRACE_ROOT before calling diagnostic CRF")
    CURRENT_TRIAL = _Path(TRACE_ROOT) / ("trial-%04d" % TRIAL_COUNTER)
    TRIAL_COUNTER += 1
    CURRENT_TRIAL.mkdir()
    np.savez(CURRENT_TRIAL / "viterbi-inputs.npz", pi=np.asarray(pi),
             transition=np.asarray(transition), norm_factor=np.asarray(norm_factor),
             activations=np.asarray(activations), tau=np.array(tau))
    _record("BEGIN", tau=int(tau), num_states=int(len(activations)),
            finite_initial=int(np.isfinite(np.asarray(pi)).sum()),
            finite_log_activation=int(np.isfinite(np.asarray(activations)).sum()),
            nan_log_activation=int(np.isnan(np.asarray(activations)).sum()),
            finite_normalizer=int(np.isfinite(np.asarray(norm_factor)).sum()))

def _row(k, values):
    values = np.asarray(values)
    finite = np.flatnonzero(np.isfinite(values))
    _record("DP_ROW", k=int(k), finite=int(np.isfinite(values).sum()),
            first_finite=int(finite[0]) if finite.size else None,
            nan=int(np.isnan(values).sum()), positive_infinity=int(np.isposinf(values).sum()))
'''


def diagnostic_source(source):
    source += OBSERVER
    source = replace_once(source, "@cython.boundscheck(False)", "@cython.boundscheck(True)")
    source = replace_once(source, "@cython.wraparound(False)", "@cython.wraparound(True)")
    source = replace_once(source, "    cdef int num_st = activations.shape[0]",
                          "    _begin(pi, transition, norm_factor, activations, tau)\n"
                          "    cdef int num_st = activations.shape[0]")
    source = replace_once(source, "    cdef int num_x = num_st / tau",
                          "    if tau <= 0:\n        _fault('invalid_tau', tau=int(tau))\n"
                          "    cdef int num_x = num_st / tau\n"
                          "    if num_x < 1:\n        _fault('no_beat_variables', num_x=int(num_x))")
    source = replace_once(source, "bps = np.empty((num_x - 1, num_st), dtype=int)",
                          "bps = np.full((num_x - 1, num_st), -1, dtype=int)")
    source = replace_once(source, "path = np.empty(num_x, dtype=int)",
                          "path = np.full(num_x, -1, dtype=int)")
    source = replace_once(source, "cdef int k, i, j, next_state",
                          "cdef int k, i, j, next_state = -1")
    source = replace_once(source, "    # iterate over all beats; the 1st beat is given by prior",
                          "    _row(-1, v_p)\n\n"
                          "    # iterate over all beats; the 1st beat is given by prior")
    source = replace_once(source, "        v_p, v_c = v_c, v_p",
                          "        v_p, v_c = v_c, v_p\n        _row(k, v_p)")
    source = replace_once(source, "    path[num_x - 1] = next_state",
                          "    np.savez(CURRENT_TRIAL / 'terminal.npz', scores=np.asarray(v_p), bps=np.asarray(bps))\n"
                          "    _record('TERMINAL', num_x=int(num_x), state=int(next_state), score=float(path_prob), nan=int(np.isnan(np.asarray(v_p)).sum()), finite=int(np.isfinite(np.asarray(v_p)).sum()))\n"
                          "    if next_state < 0:\n        _fault('uninitialized_terminal', num_x=int(num_x))\n"
                          "    if not np.isfinite(path_prob):\n        _fault('nonfinite_terminal', state=int(next_state))\n"
                          "    path[num_x - 1] = next_state")
    source = replace_once(source, "        next_state = bps[i, next_state]",
                          "        if next_state < 0 or next_state >= num_st:\n"
                          "            _fault('out_of_range_backlink_read', row=int(i), state=int(next_state))\n"
                          "        if bps[i, next_state] < 0:\n"
                          "            _fault('unwritten_backlink_read', row=int(i), state=int(next_state))\n"
                          "        _record('BACKTRACK', row=int(i), state=int(next_state), predecessor=int(bps[i, next_state]))\n"
                          "        next_state = bps[i, next_state]")
    return source


def build(cache, output):
    source, digest = pinned_source(cache)
    output.mkdir(parents=True, exist_ok=False)
    (output / "pinned-beats_crf.pyx").write_text(source, encoding="utf8")
    module = output / "madmom_crf_diagnostic.pyx"
    module.write_text(diagnostic_source(source), encoding="utf8")
    (output / "manifest.json").write_text(json.dumps({
        "pin": PINNED_REVISION, "original_sha256": digest,
        "diagnostic_sha256": hashlib.sha256(module.read_bytes()).hexdigest(),
        "script_sha256": hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),
        "variant": "bounds + initialized sentinels + fail before undefined read; arithmetic unchanged",
    }, indent=2), encoding="utf8")
    setup = output / "setup_diagnostic.py"
    setup.write_text('''from pathlib import Path
from setuptools import Extension, setup
from Cython.Build import cythonize
import numpy
root = Path(__file__).resolve().parent
setup(name="madmom-crf-diagnostic", ext_modules=cythonize(
    [Extension("madmom_crf_diagnostic", [str(root / "madmom_crf_diagnostic.pyx")],
               include_dirs=[numpy.get_include()])],
    build_dir=str(root / "generated"), compiler_directives={"language_level": 3}),
    script_args=["build_ext", "--build-lib", str(root), "--build-temp", str(root / "objects")])
''', encoding="utf8")
    with (output / "build.log").open("xb") as log:
        completed = subprocess.run([sys.executable, str(setup)], stdout=log, stderr=subprocess.STDOUT)
    if completed.returncode:
        raise RuntimeError(f"Diagnostic build failed: {output / 'build.log'}")
    binaries = list(output.glob("madmom_crf_diagnostic*.pyd"))
    if len(binaries) != 1:
        raise RuntimeError("Expected exactly one Windows diagnostic binary")
    (output / "binary.json").write_text(json.dumps({"path": str(binaries[0]),
        "sha256": hashlib.sha256(binaries[0].read_bytes()).hexdigest()}, indent=2), encoding="utf8")


def load_original(cache):
    if os.name == "nt":
        import ctypes
        ctypes.windll.kernel32.SetErrorMode(0x0001 | 0x0002)
    sys.path.insert(0, str(REPO / "tests/tools/rhythm_originals"))
    import original_sources
    return original_sources.madmom(cache)


def load_variant(module_root, output):
    manifest = json.loads((module_root / "manifest.json").read_text(encoding="utf8"))
    if hashlib.sha256((module_root / "madmom_crf_diagnostic.pyx").read_bytes()).hexdigest() != manifest["diagnostic_sha256"]:
        raise ValueError("Changed diagnostic source")
    binary = json.loads((module_root / "binary.json").read_text(encoding="utf8"))
    binary_path = Path(binary["path"])
    if hashlib.sha256(binary_path.read_bytes()).hexdigest() != binary["sha256"]:
        raise ValueError("Changed diagnostic binary")
    sys.path.insert(0, str(module_root.resolve()))
    variant = importlib.import_module("madmom_crf_diagnostic")
    if Path(variant.__file__).resolve() != binary_path.resolve():
        raise ValueError("Imported diagnostic module does not match build manifest")
    variant.TRACE_ROOT = str(output.resolve())
    return variant


def worker(args):
    import numpy as np
    import scipy
    pinned_source(args.cache)
    original = load_original(args.cache)
    variant = load_variant(args.module_root, args.output)
    envelope = np.load(args.envelope, allow_pickle=False)
    summary = {"input": str(args.envelope), "input_sha256": hashlib.sha256(args.envelope.read_bytes()).hexdigest(),
               "shape": list(envelope.shape), "dtype": str(envelope.dtype),
               "finite": bool(np.isfinite(envelope).all()), "negative_count": int((envelope < 0).sum()),
               "above_one_count": int((envelope > 1).sum()),
               "maximum": float(envelope.max()) if envelope.size else None,
               "python": sys.version, "numpy": np.__version__, "scipy": scipy.__version__,
               "raw_crf_binary": str(original.crf.__file__),
               "raw_crf_sha256": hashlib.sha256(Path(original.crf.__file__).read_bytes()).hexdigest()}
    with (args.output / "input.json").open("x", encoding="utf8") as record:
        json.dump(summary, record, indent=2)
    if args.interval:
        result = variant.best_sequence(np.ascontiguousarray(envelope, dtype=np.float32), args.interval, .18)
    else:
        sys.path.insert(0, str(Path(__file__).parent))
        from madmom_crf_safe_oracle import source_processor
        processor = source_processor(original)
        # Pinned processor chooses/sorts/smooths each trial unchanged; only its
        # Cython call is delegated to the diagnostic source copy.
        original.beats._process_crf = lambda trial: variant.best_sequence(*trial)
        result = processor.process(envelope)
    with (args.output / "returned.json").open("x", encoding="utf8") as record:
        json.dump({"result": [np.asarray(v).tolist() for v in result] if args.interval else result.tolist()}, record)


def run(args):
    args.output.mkdir(parents=True, exist_ok=False)
    command = [sys.executable, str(Path(__file__).resolve()), "worker", str(args.cache),
               str(args.module_root), str(args.envelope), str(args.output), "--interval", str(args.interval)]
    with (args.output / "stdout.log").open("xb") as out, (args.output / "stderr.log").open("xb") as err:
        try:
            completed = subprocess.run(command, stdout=out, stderr=err, timeout=60,
                creationflags=subprocess.CREATE_NO_WINDOW if os.name == "nt" else 0)
            record = {"returncode": completed.returncode, "returncode_hex": hex(completed.returncode & 0xffffffff)}
        except subprocess.TimeoutExpired:
            record = {"timeout_seconds": 60}
    (args.output / "process.json").write_text(json.dumps(record, indent=2), encoding="utf8")
    return 0 if (args.output / "returned.json").exists() else 1


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest="command", required=True)
    build_parser = commands.add_parser("build")
    build_parser.add_argument("cache", type=Path)
    build_parser.add_argument("output", type=Path)
    for command in ("run", "worker"):
        sub = commands.add_parser(command)
        for name in ("cache", "module_root", "envelope", "output"):
            sub.add_argument(name, type=Path)
        sub.add_argument("--interval", type=int, default=0)
    args = parser.parse_args()
    if args.command == "build":
        build(args.cache, args.output)
    elif args.command == "worker":
        worker(args)
    else:
        return run(args)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
