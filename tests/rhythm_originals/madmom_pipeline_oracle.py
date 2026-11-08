"""Pinned-source numerical qualification of the actual combined Madmom preset.

The C++ executable exports every rolling snapshot and explicit decoder input.
This oracle compares source primitives, supplied-input source decoders, and
independent original PCM -> frontend -> tracker results. Defined-memory CRF is
mandatory; raw native CRF is an optional, isolated diagnostic, never a fallback.
Run only in the integration owner's final verification phase.
"""
import argparse
import hashlib
import importlib
import inspect
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import numpy as np

REPO = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO / "tests/tools/rhythm_originals"))
import original_sources
import madmom_crf_safe_oracle as safe_crf

NUMERICAL_FAILURES = []
TRACKER_CACHE = TRACKER_ROOT = None
TRACKER_COUNTER = 0


def verify_sources(cache):
    authority = REPO / ".agent-workspace/rhythm-pc-max/upstream-full/madmom-27f032e8"
    pin = json.loads((authority / "PIN.json").read_text())
    assert pin["revision"] == original_sources.PINS["madmom"]
    for name, expected in pin["sha256"].items():
        for root in (authority, cache / "madmom-27f032e8"):
            assert hashlib.sha256((root / name).read_bytes()).hexdigest() == expected, str(root / name)
    return pin


def compare(name, actual, expected, atol=2e-4, rtol=3e-5):
    a, e = np.asarray(actual), np.asarray(expected)
    result = {"shape": list(a.shape), "expected_shape": list(e.shape), "status": "PASS"}
    try:
        assert a.shape == e.shape, (name, a.shape, e.shape)
        finite = np.isfinite(a) & np.isfinite(e)
        result["max_abs"] = float(np.max(np.abs(a[finite].astype(float) - e[finite]))) if finite.any() else 0.
        np.testing.assert_allclose(a, e, atol=atol, rtol=rtol, equal_nan=True, err_msg=name)
    except AssertionError as error:
        result.update(status="STRICT_FAIL", message=str(error))
        if a.size <= 64 and e.size <= 64:
            result.update(actual_values=a.tolist(), expected_values=e.tolist())
        NUMERICAL_FAILURES.append({"name": name, **result})
    print(name, json.dumps(result), flush=True)
    return result


def front_end(original, pcm, count):
    # Explicit source alignment: frame starts at k*240, with no initial/final
    # partial frames. origin='future' is source's left-edge reference convention.
    signal = original.signal.Signal(pcm, sample_rate=48000, num_channels=1)
    frames = original.signal.FramedSignal(signal, frame_size=2048, hop_size=240,
                                         origin="future", num_frames=count)
    stft = original.stft.ShortTimeFourierTransform(frames, window=np.hanning,
                                                  fft_size=2048, include_nyquist=False,
                                                  circular_shift=False)
    magnitude = original.spectrogram.Spectrogram(stft)
    filters = importlib.import_module("madmom.audio.filters")
    filtered = original.spectrogram.FilteredSpectrogram(
        magnitude, filterbank=filters.LogarithmicFilterbank, num_bands=24,
        fmin=30, fmax=17000, fref=440, norm_filters=True, unique_filters=True)
    logarithmic = original.spectrogram.LogarithmicSpectrogram(filtered, log=np.log10, mul=1, add=1)
    onset = original.onsets.superflux(logarithmic, diff_frames=None, diff_max_bins=3)
    return magnitude, filtered, logarithmic, onset


def tempo_processor(original, mode):
    crf = mode == 2
    return original.tempo.TempoEstimationProcessor(
        method="comb", min_bpm=20 if crf else 40, max_bpm=240 if crf else 250,
        act_smooth=.09 if crf else .14, hist_smooth=7 if crf else 9,
        fps=200, online=False, hist_buffer=10, alpha=.79, interpolate=False)


class IntervalEstimatorBridge:
    """Explicitly delegate the pinned histogram and dominant-interval steps."""
    def __init__(self, original, processor):
        self.original, self.processor = original, processor
        self.last_histogram = None
        self.intervals = []

    def __getattr__(self, name):
        return getattr(self.processor, name)

    def interval_histogram(self, activations):
        histogram = self.processor.histogram_processor.process(activations)
        self.last_histogram = np.asarray(histogram[0]).copy()
        return histogram

    def dominant_interval(self, histogram):
        bins, coordinates = self.original.tempo.smooth_histogram(histogram, self.processor.hist_smooth)
        interval = coordinates[np.argmax(bins)]
        self.intervals.append(int(interval))
        return interval


def tracker(original, mode, tempo):
    if mode == 2:
        return original.beats.CRFBeatDetectionProcessor(
            fps=200, tempo_estimator=tempo, interval_sigma=.18,
            num_intervals=5, use_factors=False, num_threads=1)
    return original.beats.BeatTrackingProcessor(
        fps=200, tempo_estimator=tempo, look_aside=.2,
        look_ahead=10. if mode == 1 else None)


def tracker_worker(arguments):
    parser = argparse.ArgumentParser()
    parser.add_argument("cache", type=Path)
    parser.add_argument("mode", type=int)
    parser.add_argument("directory", type=Path)
    parser.add_argument("bridge", type=int, choices=(0, 1))
    args = parser.parse_args(arguments)
    if os.name == "nt":
        import ctypes
        ctypes.windll.kernel32.SetErrorMode(0x0001 | 0x0002)
    verify_sources(args.cache)
    original = original_sources.madmom(args.cache)
    tempo = tempo_processor(original, args.mode)
    if args.bridge:
        tempo = IntervalEstimatorBridge(original, tempo)
    processor = tracker(original, args.mode, tempo)
    envelope = np.load(args.directory / "envelope.npy", allow_pickle=False)
    recursion_limit = sys.getrecursionlimit()
    (args.directory / "started").open("x").close()
    try:
        result = processor.process(envelope)
    except Exception as error:
        record = {"status": "ORIGINAL_REJECT", "kind": "PYTHON_EXCEPTION",
                  "exception": type(error).__name__, "message": str(error)}
    else:
        (args.directory / "returned").open("x").close()
        record = {"status": "RETURNED", "raw_binary_sha256": hashlib.sha256(Path(original.crf.__file__).read_bytes()).hexdigest()}
    finally:
        # detect_beats changes the process recursion limit; restore it only
        # AFTER the actual call, before serializing its unchanged result/error.
        sys.setrecursionlimit(recursion_limit)
    if record["status"] == "RETURNED":
        values = np.asarray(result)
        with (args.directory / "events.npy").open("xb") as output:
            np.save(output, values, allow_pickle=False)
        record["count"] = int(values.size)
        if args.bridge and args.mode != 2:
            record["estimated_intervals"] = tempo.intervals
            assert tempo.last_histogram is not None
            path = args.directory / "decoder_histogram.npy"
            with path.open("xb") as output:
                np.save(output, tempo.last_histogram, allow_pickle=False)
            record["decoder_histogram_sha256"] = hashlib.sha256(path.read_bytes()).hexdigest()
    with (args.directory / "result.json").open("x", encoding="utf8") as output:
        json.dump(record, output)


def worker_failure_status(directory):
    if (directory / "returned").exists():
        return "ORACLE_OUTPUT_REJECT"
    return "ORIGINAL_REJECT" if (directory / "started").exists() else "ORACLE_SETUP_REJECT"


def raw_result(mode, envelope, bridge=False):
    global TRACKER_COUNTER
    directory = TRACKER_ROOT / f"{TRACKER_COUNTER:04d}"
    TRACKER_COUNTER += 1
    directory.mkdir()
    with (directory / "envelope.npy").open("xb") as output:
        np.save(output, envelope, allow_pickle=False)
    command = [sys.executable, str(Path(__file__).resolve()), "--tracker-worker",
               str(TRACKER_CACHE), str(mode), str(directory), str(int(bridge))]
    detail = {"worker": str(directory), "mode": mode, "bridge": bridge,
              "input_sha256": hashlib.sha256((directory / "envelope.npy").read_bytes()).hexdigest()}
    try:
        completed = subprocess.run(command, capture_output=True, timeout=30,
                                   creationflags=subprocess.CREATE_NO_WINDOW if os.name == "nt" else 0)
    except subprocess.TimeoutExpired as error:
        (directory / "stdout.log").write_bytes(error.stdout or b"")
        (directory / "stderr.log").write_bytes(error.stderr or b"")
        status = worker_failure_status(directory)
        return {**detail, "status": status, "kind": "TIMEOUT", "seconds": 30}, None
    (directory / "stdout.log").write_bytes(completed.stdout)
    (directory / "stderr.log").write_bytes(completed.stderr)
    detail["returncode"] = completed.returncode
    if completed.returncode:
        status = worker_failure_status(directory)
        return {**detail, "status": status, "kind": "NATIVE_EXIT" if status == "ORIGINAL_REJECT" else "WORKER_FAILURE",
                "returncode_hex": hex(completed.returncode & 0xffffffff)}, None
    try:
        record = json.loads((directory / "result.json").read_text(encoding="utf8"))
        assert record["status"] in ("RETURNED", "ORIGINAL_REJECT")
        events = np.load(directory / "events.npy", allow_pickle=False) if record["status"] == "RETURNED" else None
        if events is not None:
            assert int(events.size) == record["count"]
    except Exception as error:
        return {**detail, "status": "ORACLE_OUTPUT_REJECT", "exception": type(error).__name__, "message": str(error)}, None
    return {**detail, **record}, events


def defined_result(envelope, module_root):
    global TRACKER_COUNTER
    directory = TRACKER_ROOT / f"defined-{TRACKER_COUNTER:04d}"
    input_path = TRACKER_ROOT / f"defined-{TRACKER_COUNTER:04d}-input.npy"
    TRACKER_COUNTER += 1
    with input_path.open("xb") as output:
        np.save(output, envelope, allow_pickle=False)
    command = [sys.executable, str(Path(__file__).with_name("madmom_crf_safe_oracle.py")), "run",
               str(TRACKER_CACHE), str(module_root), str(input_path), str(directory)]
    stdout = TRACKER_ROOT / (directory.name + "-stdout.log")
    stderr = TRACKER_ROOT / (directory.name + "-stderr.log")
    with stdout.open("xb") as out, stderr.open("xb") as err:
        completed = subprocess.run(command, stdout=out, stderr=err, timeout=120,
            creationflags=subprocess.CREATE_NO_WINDOW if os.name == "nt" else 0)
    assert completed.returncode == 0, {"source": "DEFINED_SOURCE_COPY", "returncode": completed.returncode,
                                       "stderr": str(stderr), "directory": str(directory)}
    result_path = directory / "result.json"
    record = json.loads(result_path.read_text(encoding="utf8"))
    assert record["variant"] == "DEFINED_SOURCE_COPY" and record["pin"] == original_sources.PINS["madmom"]
    assert record["input_sha256"] == hashlib.sha256(input_path.read_bytes()).hexdigest()
    assert record["status"] in ("VALID", "NO_RESULT")
    events = np.load(directory / "events.npy", allow_pickle=False)
    assert events.ndim == 1 and np.isfinite(events).all() and np.all(np.diff(events) > 0)
    assert (record["status"] == "VALID") == bool(events.size)
    record.update(report_sha256=hashlib.sha256(result_path.read_bytes()).hexdigest(),
                  events_sha256=hashlib.sha256((directory / "events.npy").read_bytes()).hexdigest())
    return record, events


def read(root, name, dtype="<f4"):
    return np.fromfile(root / name, dtype=dtype)


def matrix(root, name, columns, dtype="<f4"):
    values = read(root, name, dtype)
    assert values.size % columns == 0, (name, values.size, columns)
    return values.reshape(-1, columns)


def graph_contract(root):
    graph = json.loads((root / "graph.json").read_text(encoding="utf8"))
    assert graph["graph_id"] == "madmom-superflux-reference"
    nodes = {entry["id"]: entry for entry in graph["nodes"]}
    required = {4: "CrfBeatSequence", 80: "BeatTrackingSequence", 81: "BeatTrackingSequence",
                93: "CombFilterBank", 121: "AxisMaximum", 122: "AxisSum",
                100: "CombFilterBank", 101: "AxisMaximum", 102: "AxisSum",
                112: "CombFilterBank", 123: "AxisMaximum", 124: "AxisSum",
                96: "SameConvolution", 104: "SameConvolution", 115: "SameConvolution",
                98: "ArrayGather", 106: "ArrayGather", 116: "OrderedPeaks", 119: "NumericMap",
                99: "IndexExtract", 103: "IndexExtract"}
    assert all(nodes[key]["kind"] == kind for key, kind in required.items())
    assert not {"TempoHistogram", "TempoAnalysis", "PcmSpectralNovelty", "ActivationPeakMatrix", "ArrayReindex"} & {n["kind"] for n in nodes.values()}
    assert all(nodes[key]["settings"]["mode"] == 1 for key in (99, 103))
    assert all(nodes[key]["settings"]["accumulation"] == 1 for key in (122, 124))
    assert nodes[102]["settings"].get("accumulation", 0) == 0
    edges = {tuple(edge) for edge in graph["edges"]}
    expected = {(93, 0, 121, 0), (121, 2, 122, 0), (122, 0, 96, 0),
                (112, 0, 123, 0), (123, 2, 124, 0), (124, 0, 115, 0),
                (98, 0, 80, 1), (106, 0, 81, 1), (119, 0, 4, 1),
                (92, 0, 80, 0), (92, 0, 81, 0), (120, 0, 4, 0)}
    assert expected <= edges

    def dtype(node, direction, port):
        return next(entry["dtype"] for entry in nodes[node][direction] if entry["id"] == port)

    for node in (22, 23, 24, 25):
        assert dtype(node, "outputs", 0) == "Float32"
    for node in (111, 112, 123, 124, 115):
        port = 2 if node == 123 else 0
        assert dtype(node, "outputs", port) == "Float64"
    assert dtype(119, "outputs", 0) == "UInt64"
    for node in (80, 81, 4):
        assert dtype(node, "outputs", 0) == "Float32" and dtype(node, "outputs", 1) == "UInt64"
    return {"graph_sha256": hashlib.sha256((root / "graph.json").read_bytes()).hexdigest(),
            "atomic_chains": required, "beat_outputs": [[80, 0], [81, 0], [4, 0]]}


def validate_defined_build(cache, root):
    source, digest = safe_crf.diagnosis.pinned_source(cache)
    manifest = json.loads((root / "manifest.json").read_text(encoding="utf8"))
    assert manifest["pin"] == original_sources.PINS["madmom"] and manifest["pinned_crf_sha256"] == digest
    # load_variant verifies raw artifact bytes and the compiled binary against
    # the manifest; compare generated text with universal newlines on Windows.
    assert (root / "madmom_crf_defined.pyx").read_text(encoding="utf8") == safe_crf.safe_source(source)
    assert (root / "defined_process.py").read_text(encoding="utf8") == safe_crf.process_source(cache)
    return safe_crf.load_variant(root)


class SuppliedEstimator:
    """Explicit interval input to the unchanged source BeatTrackingProcessor.

    Reading its prediction local only selects a supplied array entry. All beat
    alignment, context extraction, admissibility and iteration remain in the
    pinned original process method. This is decoder isolation, not an end-to-end
    reference; the independent original frontend/tracker gate is also required.
    """
    act_smooth, min_interval = 0, 48

    def __init__(self, intervals):
        self.intervals = intervals

    def interval_histogram(self, activations):
        return None

    def dominant_interval(self, unused):
        caller = inspect.currentframe().f_back
        prediction = int(caller.f_locals.get("pos", 0)) if len(self.intervals) != 1 else 0
        assert caller.f_code.co_name == "process" and 0 <= prediction < len(self.intervals)
        return int(self.intervals[prediction])


def supplied_worker(arguments):
    parser = argparse.ArgumentParser()
    parser.add_argument("cache", type=Path)
    parser.add_argument("module_root", type=Path)
    parser.add_argument("mode", type=int, choices=(0, 1, 2))
    parser.add_argument("directory", type=Path)
    args = parser.parse_args(arguments)
    verify_sources(args.cache)
    original = original_sources.madmom(args.cache)
    activation = np.load(args.directory / "activation.npy", allow_pickle=False)
    intervals = np.load(args.directory / "intervals.npy", allow_pickle=False)
    assert activation.dtype == np.float32 and activation.ndim == 1 and 0 < len(activation) <= 2000 and np.isfinite(activation).all() and (activation >= 0).all()
    assert np.isfinite(intervals).all() and (intervals >= 1).all() and np.equal(intervals, np.floor(intervals)).all()
    record = {"variant": "SUPPLIED_INPUT_SOURCE_DECODER", "pin": original_sources.PINS["madmom"], "status": "RETURNED"}
    if args.mode == 2:
        module = validate_defined_build(args.cache, args.module_root)
        trials = np.sort(intervals.astype(np.uint64))[::-1]
        results = [module.best_sequence(activation, int(interval), .18) for interval in trials]
        scores = safe_crf.defined_scores(results)
        positions = results[int(np.argmax(scores))][0] if np.isfinite(scores).any() else np.empty(0, dtype=np.uint64)
        record.update(trials=trials.tolist(), scores=scores.tolist())
        record["defined_manifest_sha256"] = hashlib.sha256((args.module_root / "manifest.json").read_bytes()).hexdigest()
    else:
        processor = tracker(original, args.mode, SuppliedEstimator(intervals))
        limit = sys.getrecursionlimit()
        try:
            events = processor.process(activation)
        except Exception as error:
            record.update(status="ORIGINAL_REJECT", kind="PYTHON_EXCEPTION", exception=type(error).__name__, message=str(error))
            positions = np.empty(0, dtype=np.uint64)
        else:
            positions = np.rint(np.asarray(events) * 200).astype(np.uint64)
        finally:
            sys.setrecursionlimit(limit)
    record["positions"] = np.asarray(positions, dtype=np.uint64).tolist()
    record["activation_sha256"] = hashlib.sha256((args.directory / "activation.npy").read_bytes()).hexdigest()
    record["intervals_sha256"] = hashlib.sha256((args.directory / "intervals.npy").read_bytes()).hexdigest()
    with (args.directory / "result.json").open("x", encoding="utf8") as output:
        json.dump(record, output)


def supplied_result(mode, activation, intervals, module_root):
    global TRACKER_COUNTER
    directory = TRACKER_ROOT / f"supplied-{TRACKER_COUNTER:04d}"
    TRACKER_COUNTER += 1
    directory.mkdir()
    for name, values in (("activation", activation), ("intervals", intervals)):
        with (directory / (name + ".npy")).open("xb") as output:
            np.save(output, values, allow_pickle=False)
    command = [sys.executable, str(Path(__file__).resolve()), "--supplied-worker",
               str(TRACKER_CACHE), str(module_root), str(mode), str(directory)]
    with (directory / "stdout.log").open("xb") as out, (directory / "stderr.log").open("xb") as err:
        completed = subprocess.run(command, stdout=out, stderr=err, timeout=120,
            creationflags=subprocess.CREATE_NO_WINDOW if os.name == "nt" else 0)
    assert completed.returncode == 0, {"decoder_worker": str(directory), "returncode": completed.returncode}
    record = json.loads((directory / "result.json").read_text(encoding="utf8"))
    assert record["pin"] == original_sources.PINS["madmom"]
    for name in ("activation", "intervals"):
        assert record[name + "_sha256"] == hashlib.sha256((directory / (name + ".npy")).read_bytes()).hexdigest()
    if mode == 2:
        assert record["defined_manifest_sha256"] == hashlib.sha256((module_root / "manifest.json").read_bytes()).hexdigest()
    return record


def occupied_snapshot(onset, end):
    return np.asarray(onset[max(0, end - 2000):end], dtype=np.float32)


def segments(root, file, offset_file, dtype="<f4"):
    values, offsets = read(root, file, dtype), read(root, offset_file, "<u8")
    assert offsets.size and offsets[0] == 0 and offsets[-1] == len(values) and np.all(offsets[1:] >= offsets[:-1])
    return [values[int(first):int(last)] for first, last in zip(offsets[:-1], offsets[1:])]


def graph_beats(root, mode, index, snapshots):
    directory = root / ("fixed", "local", "crf")[mode]
    offsets = read(directory, "offsets.u64", "<u8")
    outcomes = read(directory, "outcomes.u8", "u1")
    times, positions = read(directory, "beats.f32"), read(directory, "positions.u64", "<u8")
    assert outcomes.shape == (snapshots,) and np.isin(outcomes, [0, 1]).all()
    assert offsets.shape == (snapshots + 1,) and offsets[0] == 0 and offsets[-1] == len(times) == len(positions)
    assert np.all(offsets[1:] >= offsets[:-1]) and np.isfinite(times).all()
    first, last = map(int, offsets[index:index + 2])
    assert np.all(np.diff(positions[first:last].astype(np.int64)) > 0)
    compare("exported beat seconds match UInt64 positions", times[first:last], positions[first:last].astype(np.float32) / np.float32(200), atol=0, rtol=0)
    if outcomes[index]:
        assert first == last, "rejected graph snapshot cannot publish a path"
    return times[first:last], positions[first:last], outcomes[index] == 0


def crf_diagnostics(root, index, count):
    directory = root / "crf"
    offsets = read(directory, "trial_offsets.u64", "<u8")
    intervals, scores = read(directory, "trials.u64", "<u8"), read(directory, "scores.f32")
    assert offsets.shape == (count + 1,) and offsets[0] == 0 and offsets[-1] == len(intervals) == len(scores)
    assert np.all(offsets[1:] >= offsets[:-1]) and (np.isfinite(scores) | np.isneginf(scores)).all()
    first, last = map(int, offsets[index:index + 2])
    return intervals[first:last], scores[first:last]


def source_tracker(mode, envelope, module_root):
    if mode == 2:
        return defined_result(envelope, module_root)
    source, events = raw_result(mode, envelope, bridge=True)
    assert source["status"] == "RETURNED", source
    assert np.asarray(events).ndim == 1 and np.isfinite(events).all()
    return source, events


def expected_trials(original, envelope):
    candidates = np.asarray(tempo_processor(original, 2).process(envelope))
    if not candidates.size or not np.isfinite(candidates[:, 0]).any():
        return np.empty(0, dtype=np.uint64)
    # The pinned caller performs the two Double divisions before integer cast.
    return (12000. / candidates[:5, 0]).astype(np.uint64)


def primitive_gates(original, root, index, envelope, stages, supplied):
    report = {}
    expected_fixed = original.signal.smooth(envelope, 28)
    expected_crf = original.signal.smooth(envelope, 18)
    report["fixed_activation"] = compare("explicit Fixed activation smoothing", stages["fixed_activation"][index], expected_fixed)
    report["crf_activation"] = compare("explicit Double CRF activation smoothing", stages["crf_activation"][index], expected_crf)
    compare("explicit CRF decoder narrowing", stages["decoder_activation"][index], stages["crf_activation"][index].astype(np.float32), atol=0, rtol=0)
    for name, low, high, smooth in (("fixed", 48, 300, 9), ("crf", 50, 600, 7)):
        raw = original.tempo.interval_histogram_comb(stages[name + "_activation"][index], alpha=.79, min_tau=low, max_tau=high)
        smoothed = original.tempo.smooth_histogram(raw, smooth)[0]
        report[name + "_histogram"] = compare("explicit " + name + " comb/tied-max/sum", stages[name + "_histogram"][index], raw[0], atol=2e-3)
        report[name + "_smoothed"] = compare("explicit " + name + " histogram smoothing", stages[name + "_smoothed"][index], smoothed, atol=1e-2)
        if name == "fixed":
            compare("supplied Fixed interval", stages["fixed_intervals"][index:index + 1], [raw[1][np.argmax(smoothed)]], atol=0, rtol=0)
    # Keep the source's Double response/history and sequential Double sum.
    # Do not round its authority toward the graph's portable Float32 policy.
    comb = importlib.import_module("madmom.audio.comb_filters")
    activation = stages["fixed_activation"][index]
    response = comb.CombFilterbankProcessor("backward", np.arange(48, 301), .79).process(np.pad(activation, (0, 1999)))
    weighted = response * (response == np.max(response, axis=1, keepdims=True))
    prefixes = np.cumsum(weighted, axis=0)[1999:]
    local = np.asarray([original.tempo.dominant_interval((row, np.arange(48, 301)), smooth=9) for row in prefixes])
    report["local_interval_map"] = compare("supplied Local interval at every prediction", stages["local_intervals"][index], local, atol=0, rtol=0)
    report["crf_trials"] = compare("supplied CRF source candidate intervals", supplied, expected_trials(original, envelope), atol=0, rtol=0)
    return report


def verify_short_rejection(mode, envelope, direct, isolated):
    assert mode == 0
    report = {"graph_status": "REJECT", "supplied_input_source": isolated}
    if isolated["status"] == "RETURNED":
        NUMERICAL_FAILURES.append({"name": "successful supplied source decoder versus rejected graph", "source": isolated})
        report["status"] = "STRICT_FAIL"
    else:
        assert isolated["status"] == "ORIGINAL_REJECT" and isolated["kind"] == "PYTHON_EXCEPTION" and isolated["exception"] == "IndexError", isolated
    for label, values in (("exported_odf", envelope), ("original_pcm", direct)):
        source, events = raw_result(mode, values, bridge=True)
        assert source["status"] in ("RETURNED", "ORIGINAL_REJECT"), source
        report[label] = source
        if source["status"] == "RETURNED":
            NUMERICAL_FAILURES.append({"name": "successful source return versus rejected live graph", "source": source,
                                       "events": np.asarray(events).tolist(), "route": label})
            report["status"] = "STRICT_FAIL"
        else:
            assert source["kind"] == "PYTHON_EXCEPTION" and source["exception"] == "IndexError", source
    report.setdefault("status", "MATCHED_ORIGINAL_REJECT")
    return report


def snapshot_gates(original, root, index, envelope, direct, stages, supplied, module_root, count, raw_crf):
    report = {"index": index, "primitives": primitive_gates(original, root, index, envelope, stages, supplied), "decoders": []}
    for mode in range(3):
        times, positions, accepted = graph_beats(root, mode, index, count)
        activation = stages["decoder_activation" if mode == 2 else "fixed_activation"][index]
        intervals = (stages["fixed_intervals"][index:index + 1] if mode == 0 else stages["local_intervals"][index] if mode == 1 else supplied)
        isolated = supplied_result(mode, activation, intervals, module_root)
        if not accepted:
            report["decoders"].append({"mode": mode, **verify_short_rejection(mode, envelope, direct, isolated)})
            continue
        assert isolated["status"] == "RETURNED", isolated
        result = {"mode": mode, "graph_status": "AVAILABLE", "supplied_input_source": isolated}
        result["decoder_positions"] = compare("source decoder on explicit inputs", positions, np.asarray(isolated["positions"], dtype=np.uint64), atol=0, rtol=0)
        source, events = source_tracker(mode, envelope, module_root)
        result["source_on_exported_odf"] = source
        result["snapshot_beats"] = compare("actual source tracker on exported ODF", times, events, atol=2e-6, rtol=2e-6)
        compare("source tracker exact frame positions", positions, np.rint(np.asarray(events) * 200).astype(np.uint64), atol=0, rtol=0)
        source_pcm, direct_events = source_tracker(mode, direct, module_root)
        result["source_on_original_pcm"] = source_pcm
        result["end_to_end_beats"] = compare("independent original PCM frontend and source tracker", times, direct_events, atol=2e-6, rtol=2e-6)
        compare("end-to-end exact frame positions", positions, np.rint(np.asarray(direct_events) * 200).astype(np.uint64), atol=0, rtol=0)
        if mode == 2:
            trials, scores = crf_diagnostics(root, index, count)
            compare("source decoder sorted trials", trials, np.asarray(isolated["trials"], dtype=np.uint64), atol=0, rtol=0)
            compare("source decoder normalized scores", scores, isolated["scores"], atol=2e-4)
            compare("full source attempted intervals", trials, np.asarray([entry["interval"] for entry in source["trials"]], dtype=np.uint64), atol=0, rtol=0)
            compare("full source normalized scores", scores, [entry["normalized_score"] for entry in source["trials"]], atol=2e-4)
            if raw_crf:
                raw, _ = raw_result(mode, envelope)
                result["raw_original_crf_diagnostic"] = raw
                assert raw["status"] in ("RETURNED", "ORIGINAL_REJECT"), raw
        elif index == 0:
            raw, _ = raw_result(mode, envelope)
            result["raw_original_interface_diagnostic"] = raw
            assert raw["status"] in ("RETURNED", "ORIGINAL_REJECT"), raw
        report["decoders"].append(result)
    return report


def capture_case(original, root, module_root, raw_crf):
    before = len(NUMERICAL_FAILURES)
    meta = json.loads((root / "capture.json").read_text(encoding="utf8"))
    assert (meta["version"], meta["sample_rate"], meta["frame"], meta["hop"], meta["history"], meta["stride"]) == (1, 48000, 2048, 240, 2000, 40)
    assert meta["graph_id"] == "madmom-superflux-reference" and meta["byte_order"] == "little" and meta["beat_nodes"] == [80, 81, 4]
    pcm = read(root, "pcm.f32")
    count, snapshots = meta["frames"], meta["snapshots"]
    assert len(pcm) == meta["samples"] and np.isfinite(pcm).all() and count == 1 + (len(pcm) - 2048) // 240
    magnitude, filtered, logarithmic, onset = front_end(original, pcm, count)
    report = {"case": root.name, "capture": meta, "graph": graph_contract(root), "snapshots": []}
    report["magnitude"] = compare("source raw magnitude bins 1..1023", matrix(root, "magnitude.f32", 1024)[:, :-1], np.asarray(magnitude)[:, 1:], atol=5e-5)
    report["filtered"] = compare("source logarithmic filterbank", matrix(root, "filtered.f32", 137), filtered)
    report["log"] = compare("source logarithmic compression", matrix(root, "log.f32", 137), logarithmic)
    graph_onset = read(root, "onset.f32")
    report["onset"] = compare("source complete SuperFlux frontend", graph_onset, onset)
    contexts = segments(root, "snapshots.f32", "snapshot_offsets.u64")
    ends = read(root, "snapshot_ends.u64", "<u8")
    assert len(contexts) == len(ends) == snapshots == 1 + (count - 1) // 40
    assert np.array_equal(ends, 1 + 40 * np.arange(snapshots))
    columns = {"fixed_histogram": 253, "fixed_smoothed": 253, "crf_histogram": 551, "crf_smoothed": 551}
    stages = {name: matrix(root, name + (".f64" if name.startswith("crf_") else ".f32"), width,
                          "<f8" if name.startswith("crf_") else "<f4") for name, width in columns.items()}
    stages.update(fixed_activation=segments(root, "fixed_activation.f32", "fixed_offsets.u64"),
                  local_intervals=segments(root, "local_intervals.f32", "fixed_offsets.u64"),
                  crf_activation=segments(root, "crf_activation.f64", "crf_offsets.u64", "<f8"),
                  decoder_activation=segments(root, "decoder_activation.f32", "crf_offsets.u64"))
    stages["fixed_intervals"] = read(root, "fixed_intervals.f32")
    assert all(len(values) == snapshots and all(np.isfinite(row).all() for row in values) for values in stages.values())
    trials, offsets = read(root, "supplied_trials.u64", "<u8"), read(root, "supplied_offsets.u64", "<u8")
    assert offsets.shape == (snapshots + 1,) and offsets[0] == 0 and offsets[-1] == len(trials) and np.all(offsets[1:] >= offsets[:-1])
    for index, end in enumerate(ends):
        compare("bounded chronological occupied snapshot", contexts[index], occupied_snapshot(graph_onset, int(end)), atol=0, rtol=0)
        supplied = trials[int(offsets[index]):int(offsets[index + 1])]
        direct = occupied_snapshot(onset, int(end))
        report["snapshots"].append(snapshot_gates(original, root, index, contexts[index], direct, stages, supplied, module_root, snapshots, raw_crf))
    report["capture_sha256"] = {str(path.relative_to(root)): hashlib.sha256(path.read_bytes()).hexdigest()
                                for path in root.rglob("*") if path.is_file()}
    report["numerical_status"] = "STRICT_FAIL" if len(NUMERICAL_FAILURES) != before else "PASS_ON_EXPORTED_CAPTURE"
    return report


def main():
    global TRACKER_CACHE, TRACKER_ROOT
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("evidence", type=Path)
    parser.add_argument("--source-cache", type=Path, required=True)
    parser.add_argument("--defined-crf-root", type=Path, required=True)
    parser.add_argument("--report", type=Path)
    parser.add_argument("--raw-crf", action="store_true", help="Additional isolated unsafe-original diagnostic; never changes qualification gates")
    args = parser.parse_args()
    report_path = args.report or args.evidence / "madmom-parity.json"
    if report_path.exists():
        raise FileExistsError(f"Preserve prior numerical report: {report_path}")
    pin = verify_sources(args.source_cache)
    validate_defined_build(args.source_cache, args.defined_crf_root)
    regression = json.loads((args.evidence / "regression.json").read_text(encoding="utf8"))
    assert regression["cases"] == ["combined", "rollover", "silence", "provider"]
    assert all(regression[name] is True for name in ("packet_reset_clone", "no_hot_cpp_allocation", "provider_clone", "coordinate_validation"))
    original = original_sources.madmom(args.source_cache)
    TRACKER_CACHE = args.source_cache.resolve()
    TRACKER_ROOT = Path(tempfile.mkdtemp(prefix=report_path.stem + "-workers-", dir=report_path.parent)).resolve()
    reports, failure = [], None
    try:
        for name in regression["cases"]:
            reports.append(capture_case(original, args.evidence / name, args.defined_crf_root.resolve(), args.raw_crf))
        if NUMERICAL_FAILURES:
            raise AssertionError(f"{len(NUMERICAL_FAILURES)} strict numerical comparisons failed")
    except Exception as error:
        failure = {"exception": type(error).__name__, "message": str(error)}
        raise
    finally:
        result = {"pinned_revision": pin["revision"], "results": reports, "runtime_regression": regression,
                  "oracle_sha256": hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),
                  "source_modules_sha256": {name: hashlib.sha256(Path(module.__file__).read_bytes()).hexdigest()
                                            for name, module in vars(original).items()},
                  "defined_build": {"source": json.loads((args.defined_crf_root / "manifest.json").read_text(encoding="utf8")),
                                    "binary": json.loads((args.defined_crf_root / "binary.json").read_text(encoding="utf8"))},
                  "complete": len(reports) == len(regression["cases"]), "failure": failure,
                  "numerical_failures": NUMERICAL_FAILURES, "tracker_workers": str(TRACKER_ROOT),
                  "numerical_status": "STRICT_FAIL" if failure else "PASS_ON_EXPORTED_CAPTURES",
                  "scope": "actual combined graph; all exported rolling snapshots; eight changing synthetic tones and silence; no musical-quality claim"}
        with report_path.open("x", encoding="utf8") as output:
            json.dump(result, output, indent=2)
            output.write("\n")


if __name__ == "__main__":
    if len(sys.argv) > 1 and sys.argv[1] == "--tracker-worker":
        tracker_worker(sys.argv[2:])
    elif len(sys.argv) > 1 and sys.argv[1] == "--supplied-worker":
        supplied_worker(sys.argv[2:])
    else:
        main()
