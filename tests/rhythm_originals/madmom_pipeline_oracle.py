"""Pinned madmom stage oracle for madmom_reference_pipeline_tests exports.

Run only in the shared owner's final integration phase. Direct source calls
are recorded separately from an explicit equivalent interval-estimator bridge.
No neural inference or weights are imported. A numerical PASS is bounded to the
exported signals/settings; it is not an assessment of musical tracking quality.
"""
import argparse
import base64
import hashlib
import importlib
import io
import json
import math
import os
from pathlib import Path
import subprocess
import sys
import tempfile

import numpy as np

REPO = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO / "tests/tools/rhythm_originals"))
import original_sources

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


def read(root, name):
    return np.fromfile(root / (name + ".f32"), dtype="<f4")


def whole_path(root, name):
    direct = root / name
    if direct.exists():
        return direct
    aliases = {"snapshot.f32": "envelope.f32", "onset.f32": "envelope.f32",
               "trials.u64": "trial_intervals.u64"}
    return root / aliases.get(name, name)


def whole_read(root, name):
    return np.fromfile(whole_path(root, name + ".f32"), dtype="<f4")


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

    def __getattr__(self, name):
        return getattr(self.processor, name)

    def interval_histogram(self, activations):
        histogram = self.processor.histogram_processor.process(activations)
        self.last_histogram = np.asarray(histogram[0]).copy()
        return histogram

    def dominant_interval(self, histogram):
        bins, coordinates = self.original.tempo.smooth_histogram(histogram, self.processor.hist_smooth)
        return coordinates[np.argmax(bins)]


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
        record = {"status": "RETURNED"}
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


def temporal(original, root, mode, report):
    envelope = read(root, "snapshot")
    tempo = tempo_processor(original, mode)
    candidates = tempo.process(envelope)
    smoothed_activation = original.signal.smooth(envelope, int(round(200 * tempo.act_smooth)))
    raw_histogram = tempo.histogram_processor.process(smoothed_activation)
    smooth_histogram = original.tempo.smooth_histogram(raw_histogram, tempo.hist_smooth)[0]
    report["tempo"] = compare("tempo candidates", read(root, "tempo").reshape(-1, 2), candidates)
    report["histogram"] = compare("raw comb histogram", read(root, "histogram"), raw_histogram[0], atol=2e-3)
    report["smoothed"] = compare("smoothed histogram", read(root, "smoothed"), smooth_histogram, atol=1e-2)
    if mode == 2:
        # Diagnostic only: neither rounded/graph histogram replaces the actual
        # original candidate comparison above or its unchanged strict gate.
        report["candidate_precision_diagnostic"] = {
            "source_on_exported_smoothed": original.tempo.detect_tempo(
                (read(root, "smoothed"), raw_histogram[1]), fps=200, interpolate=False).tolist(),
            "source_on_rounded_reference_smoothed": original.tempo.detect_tempo(
                (np.asarray(smooth_histogram, dtype=np.float32), raw_histogram[1]),
                fps=200, interpolate=False).tolist()}
    raw, events = raw_result(mode, envelope)
    assert raw["status"] in ("RETURNED", "ORIGINAL_REJECT"), raw
    report["raw_original_tracker"] = raw
    if mode != 2:
        safe, events = raw_result(mode, envelope, bridge=True)
        report["explicit_estimator_bridge"] = safe
    checked = safe if mode != 2 else raw
    assert checked["status"] in ("RETURNED", "ORIGINAL_REJECT"), checked
    if checked["status"] == "RETURNED":
        report["beats"] = compare("snapshot-relative beat seconds", read(root, "beats"), events, atol=2e-6, rtol=2e-6)
        compare("exact beat frame indices", np.fromfile(root / "beat_positions.u64", dtype="<u8"),
                np.rint(np.asarray(events) * 200).astype(np.uint64), atol=0, rtol=0)
    else:
        report["beats"] = {"status": "UNCOMPARABLE_ORIGINAL_REJECT", "source": checked,
                           "graph_returned_count": int(read(root, "beats").size)}
        NUMERICAL_FAILURES.append({"name": "returned graph beat path has no successful original path", **report["beats"]})
    peaks = original.onsets.peak_picking(envelope[:, None], threshold=.5,
                                       pre_avg=0, post_avg=0, pre_max=1, post_max=1)
    compare("onset peak frame indices", np.fromfile(root / "peaks.u64", dtype="<u8"),
            np.asarray(peaks[0], dtype=np.uint64), atol=0, rtol=0)
    verify_rejections(original, root, mode, report)


def whole_estimators(original, root, mode, report):
    envelope = whole_read(root, "snapshot")
    tempo = tempo_processor(original, mode)
    candidates = tempo.process(envelope)
    activation = original.signal.smooth(envelope, int(round(200 * tempo.act_smooth)))
    histogram = tempo.histogram_processor.process(activation)
    smoothed = original.tempo.smooth_histogram(histogram, tempo.hist_smooth)[0]
    report["tempo"] = compare("whole tempo candidates", read(root, "tempo").reshape(-1, 2), candidates)
    report["histogram"] = compare("whole raw histogram", read(root, "histogram"), histogram[0], atol=2e-3)
    report["smoothed"] = compare("whole smoothed histogram", read(root, "smoothed"), smoothed, atol=1e-2)
    if (root / "coordinates.f32").exists():
        report["histogram_coordinates"] = compare("whole interval coordinates", read(root, "coordinates"), histogram[1], atol=0, rtol=0)
    if mode == 2 and (root / "crf_tempo.f32").exists():
        report["crf_tempo"] = compare("whole CRF own tempo candidates", read(root, "crf_tempo").reshape(-1, 2), candidates)
    return envelope


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


def compare_beat_outputs(root, events, label):
    result = compare(label + " beat seconds", read(root, "beats"), events, atol=2e-6, rtol=2e-6)
    compare(label + " exact beat indices", np.fromfile(root / "beat_positions.u64", dtype="<u8"),
            np.rint(np.asarray(events) * 200).astype(np.uint64), atol=0, rtol=0)
    return result


def compare_decoder_histogram(root, source, label):
    path = Path(source["worker"]) / "decoder_histogram.npy"
    assert hashlib.sha256(path.read_bytes()).hexdigest() == source["decoder_histogram_sha256"]
    histogram = np.load(path, allow_pickle=False)
    assert histogram.shape == (253,) and np.isfinite(histogram).all() and (histogram >= 0).all()
    return compare(label + " last decoder interval histogram", read(root, "decoder_histogram"), histogram, atol=2e-3)


def whole_temporal(original, root, mode, report, module_root, source_onset):
    envelope = whole_estimators(original, root, mode, report)
    if mode == 2:
        source, events = defined_result(envelope, module_root)
        report["defined_source_tracker"] = source
        compare("whole CRF attempted intervals", np.fromfile(whole_path(root, "trials.u64"), dtype="<u8"),
                np.asarray([row["interval"] for row in source["trials"]], dtype=np.uint64), atol=0, rtol=0)
        compare("whole CRF normalized scores", read(root, "trial_scores"),
                np.asarray([row["normalized_score"] for row in source["trials"]]), atol=2e-4)
    else:
        raw, _ = raw_result(mode, envelope)
        source, events = raw_result(mode, envelope, bridge=True)
        report.update(raw_original_tracker=raw, explicit_estimator_bridge=source)
        assert raw["status"] == source["status"] == "RETURNED", report
        report["decoder_histogram"] = compare_decoder_histogram(root, source, "whole")
    report["beats"] = compare_beat_outputs(root, events, "whole")
    # A stage-isolated match on exported ODF is not an end-to-end reference.
    # Feed the ACTUAL original frontend's ODF into the original tracker as well.
    direct_envelope = np.asarray(source_onset)
    direct_tempo = tempo_processor(original, mode).process(direct_envelope)
    report["original_frontend_tempo"] = compare("whole actual-source frontend tempo",
        read(root, "tempo").reshape(-1, 2), direct_tempo)
    if mode == 2:
        direct, direct_events = defined_result(direct_envelope, module_root)
    else:
        direct_raw, _ = raw_result(mode, direct_envelope)
        direct, direct_events = raw_result(mode, direct_envelope, bridge=True)
        assert direct_raw["status"] == direct["status"] == "RETURNED", (direct_raw, direct)
        report["original_frontend_raw_tracker"] = direct_raw
        report["end_to_end_decoder_histogram"] = compare_decoder_histogram(root, direct, "whole actual-source frontend")
    report["original_frontend_tracker"] = direct
    report["end_to_end_beats"] = compare_beat_outputs(root, direct_events, "whole actual-source frontend")
    report["peak_branch"] = "OMITTED: no rolling peak branch is claimed as whole-input"
    if (root / "rejected_ends.u64").exists():
        assert np.fromfile(root / "rejected_ends.u64", dtype="<u8").size == 0


def whole_projection_contract(nodes, mode):
    weights = []
    expected = "sequential_fma" if mode == 2 else "separate"
    for node_id in range(60, 65):
        parameters = {item["key"]: item["value"] for item in nodes[node_id]["parameters"]["values"]}
        assert parameters.get("accumulation", "separate") == expected
        assert parameters["compensated"] is False and parameters["square_input"] is False
        encoded = parameters["weights"]["values"]
        assert encoded.startswith("f32le-base64:")
        weights.append(np.frombuffer(base64.b64decode(encoded.split(":", 1)[1]), dtype="<f4").reshape(-1, 1024))
    weights = np.concatenate(weights, axis=0)
    assert weights.shape == (137, 1024)
    if mode == 2:
        filters = importlib.import_module("madmom.audio.filters")
        frequencies = np.arange(1024) * (48000. / 2048.)
        bank = filters.LogarithmicFilterbank(frequencies, num_bands=24, fmin=30,
            fmax=17000, fref=440, norm_filters=True, unique_filters=True)
        aligned = np.concatenate((np.asarray(bank).T[:, 1:], np.zeros((137, 1), dtype=np.float32)), axis=1)
        assert np.array_equal(weights, aligned), "Whole CRF requires source-constructed filter weights"
    return {"accumulation": expected, "filter_weights": "source_reference" if mode == 2 else "portable_float32"}


def whole_graph_precision(root, mode):
    path = root / "graph.json"
    if not path.exists():
        return "Public graph dtype contracts checked by C++ whole fixture"
    graph = json.loads(path.read_text(encoding="utf8"))["scene_graph"]
    nodes = {node["id"]: node for node in graph["nodes"]}
    dtype = "Float64" if mode == 2 else "Float32"
    def port_type(node, direction, port):
        return next(item["contract"]["dtype"] for item in nodes[node][direction] if item["id"] == port)
    assert port_type(22, "outputs", 0) == port_type(23, "inputs", 1) == dtype
    assert port_type(23, "inputs", 0) == "Float32"
    assert port_type(23, "outputs", 0) == port_type(24, "inputs", 0) == dtype
    assert port_type(24, "outputs", 0) == port_type(24, "outputs", 1) == "Float32"
    parameters = {item["key"]: item["value"] for item in nodes[25]["parameters"]["values"]}
    magnitude_norm = parameters.get("magnitude_norm", "hypot")
    assert magnitude_norm == ("scaled_ratio_fma" if mode == 2 else "hypot")
    return {"window_product_fft_input": dtype, "fft_publication": "Float32",
            "magnitude_norm": magnitude_norm,
            "projection": whole_projection_contract(nodes, mode),
            "graph_sha256": hashlib.sha256(path.read_bytes()).hexdigest()}


def whole_metadata(root, mode):
    if (root / "whole.json").exists():
        return json.loads((root / "whole.json").read_text(encoding="utf8"))
    recipe = json.loads((root / "recipe.json").read_text(encoding="utf8"))
    capture = json.loads((root / "capture.json").read_text(encoding="utf8"))
    assert recipe["revision"] == original_sources.PINS["madmom"] and recipe["whole_input"]
    assert (recipe["sample_rate"], recipe["frame"], recipe["hop"], recipe["frame_rate"], recipe["mode"]) == (48000, 2048, 240, 200, mode)
    count, samples = recipe["odf_frames"], recipe["pcm_samples"]
    assert capture["execution_complete"] and capture["processing_status"] == capture["finalization_status"] == 0
    assert capture["batch_attempts"] == 1 and capture["declared_samples"] == capture["processed_samples"] == samples
    assert capture["collected_values"] == count and capture["result"] in ("available", "no_result")
    required = {(35, port) for port in range(4)} | {(4, port) for port in range(5 if mode == 2 else 3)}
    endpoints = {(entry["node"], entry["port"]): entry for entry in capture["endpoints"]}
    assert required <= endpoints.keys()
    for identity in required:
        entry = endpoints[identity]
        assert entry["complete"] and entry["available_publications"] == 1
        assert entry["streaming_publications"] == entry["errors"] == 0 and entry["byte_order"] == "little"
        assert entry["dtype"] == ("UInt64" if identity in ((4, 1), (4, 2)) and (mode == 2 or identity == (4, 1)) else "Float32")
        path = root / entry["file"]
        assert hashlib.sha256(path.read_bytes()).hexdigest() == entry["sha256"]
        width = 8 if entry["dtype"] == "UInt64" else 4
        assert path.stat().st_size == math.prod(entry["shape"]) * width
        candidate = identity in ((35, 0), (4, 4))
        assert len(entry["shape"]) == 2 and entry["shape"][1] == (2 if candidate else 1)
        assert entry["rank"] == (2 if candidate else 1)
        if entry["dtype"] == "Float32":
            values = np.fromfile(path, dtype="<f4")
            if candidate:
                rows = values.reshape(-1, 2)
                sentinel = rows.shape == (1, 2) and np.isnan(rows[0, 0]) and rows[0, 1] == 0
                assert sentinel or (np.isfinite(rows).all() and (rows[:, 0] > 0).all() and (rows[:, 1] >= 0).all())
            elif identity == (4, 3):
                assert (np.isfinite(values) | np.isneginf(values)).all()
            else:
                assert np.isfinite(values).all()
        if identity[0] == 35 and identity[1] != 0:
            assert entry["shape"] == [551 if mode == 2 else 253, 1]
        if identity == (4, 2) and mode != 2:
            assert entry["shape"] == [253, 1]
    beats = np.fromfile(root / endpoints[(4, 0)]["file"], dtype="<f4")
    positions = np.fromfile(root / endpoints[(4, 1)]["file"], dtype="<u8")
    assert beats.size == positions.size and (positions < count).all() and np.all(np.diff(positions.astype(np.int64)) > 0)
    assert (capture["result"] == "available") == bool(beats.size)
    if mode == 2:
        trials = np.fromfile(root / endpoints[(4, 2)]["file"], dtype="<u8")
        scores = np.fromfile(root / endpoints[(4, 3)]["file"], dtype="<f4")
        assert trials.size == scores.size <= 5 and ((trials >= 50) & (trials <= 600)).all()
        assert np.isfinite(scores).any() if beats.size else np.isneginf(scores).all()
    return {"whole_input": True, "pcm_samples": samples, "frames": count, "eof_batches": 1,
            "tail_samples": samples - ((count - 1) * 240 + 2048), "capture": capture,
            "frontend_contract": whole_graph_precision(root, mode)}


def whole_case(original, root, mode, module_root):
    previous_failures = len(NUMERICAL_FAILURES)
    metadata = whole_metadata(root, mode)
    pcm = read(root, "pcm")
    assert np.isfinite(pcm).all(), "PCM is finite signed audio, not nonnegative activations"
    count = 1 + (pcm.size - 2048) // 240
    assert metadata["whole_input"] and metadata["eof_batches"] == 1
    assert metadata["pcm_samples"] == pcm.size and metadata["frames"] == count
    assert metadata["tail_samples"] == pcm.size - ((count - 1) * 240 + 2048)
    assert count > 2000 and (count - 1) % 40 != 0, "whole fixture must exceed rolling context and end off stride"
    envelope = whole_read(root, "snapshot")
    assert envelope.shape == (count,) and np.isfinite(envelope).all() and (envelope >= 0).all()
    magnitude, filtered, logarithmic, onset = front_end(original, pcm, count)
    report = {"mode": mode, "whole_session": metadata, "reference": original_sources.PINS["madmom"],
              "graph_frontend_precision": ("Float32 PCM; Double Hann/product/real FFT; Float32 complex publication before magnitude"
                                           if mode == 2 else "Float32 graph frontend"),
              "graph_magnitude_norm": "scaled_ratio_fma" if mode == 2 else "hypot",
              "odf_collection_file": whole_path(root, "snapshot.f32").name,
              "frame_origin": "complete 2048/hop240 frames, left edge; no EOF flush"}
    for name, expected in (("magnitude", np.asarray(magnitude)[:, 1:]), ("filtered", filtered), ("log", logarithmic)):
        path = root / (name + ".f32")
        if path.exists():
            actual = read(root, name).reshape(count, -1)
            if name == "magnitude":
                actual = actual[:, :-1]
            report[name] = compare("whole " + name, actual, expected, atol=5e-5 if name == "magnitude" else 2e-4)
        else:
            report[name] = {"status": "NOT_EXPORTED", "gate": "full ODF and end-to-end tracker remain strict"}
    report["onset"] = compare("whole complete SuperFlux", envelope, onset)
    if (root / "onset.f32").exists():
        compare("whole snapshot contains every collected frame", envelope, read(root, "onset"), atol=0, rtol=0)
    whole_temporal(original, root, mode, report, module_root, onset)
    report["capture_sha256"] = {path.name: hashlib.sha256(path.read_bytes()).hexdigest() for path in root.iterdir()
                                if path.is_file() and path.suffix in (".f32", ".u64", ".json", ".txt")}
    report["numerical_comparison"] = "STRICT_FAIL" if len(NUMERICAL_FAILURES) != previous_failures else "PASS_ON_EXPORTED_WHOLE_CASE"
    return report


def legacy_defined_binding(root, report_path):
    record = json.loads(report_path.read_text(encoding="utf8"))
    assert record["variant"] == "DEFINED_SOURCE_COPY" and record["pin"] == original_sources.PINS["madmom"]
    serialized = io.BytesIO()
    np.save(serialized, read(root, "snapshot"), allow_pickle=False)
    actual_input_hash = hashlib.sha256(serialized.getvalue()).hexdigest()
    assert actual_input_hash == record["input_sha256"] == "4f2e2a6295f8249a744b6b83aa37334f64629d0e83d318a05b3b27671040f831"
    beats = compare_beat_outputs(root, np.asarray(record["events"]), "bound legacy defined")
    compare("bound legacy trial intervals", np.fromfile(root / "trials.u64", dtype="<u8"),
            np.asarray([row["interval"] for row in record["trials"]], dtype=np.uint64), atol=0, rtol=0)
    scores = compare("bound legacy normalized scores", read(root, "trial_scores"),
                     np.asarray([row["normalized_score"] for row in record["trials"]]), atol=2e-4)
    return {"route": "explicit hash-bound reuse of actual defined-source execution, not a fresh legacy call",
            "input_sha256": actual_input_hash, "source_report_sha256": hashlib.sha256(report_path.read_bytes()).hexdigest(),
            "source_report": str(report_path), "graph": str(root), "beats": beats, "scores": scores,
            "graph_snapshot_sha256": hashlib.sha256((root / "snapshot.f32").read_bytes()).hexdigest()}


def returned_path_evidence(events, mode):
    values = events.tolist()
    evidence = {"events": values, "shape": list(events.shape)}
    if events.ndim != 1:
        evidence["classification"] = "INVALID_SOURCE_PATH_SHAPE"
    elif any(not math.isfinite(value) for value in values):
        evidence["classification"] = "NONFINITE_SOURCE_PATH"
    elif any(left > right for left, right in zip(values, values[1:])):
        evidence["classification"] = "VIOLATES_PINNED_CRF_BACKTRACE_ORDER" if mode == 2 else "NONMONOTONIC_SOURCE_PATH"
        if mode == 2:
            evidence["source_invariant"] = ("madmom 27f032e8 beats_crf.pyx:205-217 stores predecessor i-j<=i; "
                                            "238-239 backtracks these links; beats.py:791 only scales by positive fps")
    else:
        evidence["classification"] = "MONOTONIC_SOURCE_RETURN" if values else "EMPTY_SOURCE_RETURN"
    return evidence


def verify_rejections(original, root, mode, report):
    rejected_ends = np.fromfile(root / "rejected_ends.u64", dtype="<u8")
    onset = read(root, "onset")
    results = []
    for end in rejected_ends:
        end = int(end)
        envelope = onset[max(0, end - 2000):end]
        raw, events = raw_result(mode, envelope)
        assert raw["status"] in ("RETURNED", "ORIGINAL_REJECT"), raw
        record = {"snapshot_end": end, "raw_original": raw}
        results.append(record)
        checked = raw
        if mode != 2:
            checked, events = raw_result(mode, envelope, bridge=True)
            record["explicit_estimator_bridge"] = checked
        assert checked["status"] in ("RETURNED", "ORIGINAL_REJECT"), checked
        # Preserve a successful source return as a strict mismatch, including
        # empty/invalid paths. Continue aggregation; never relabel it a reject.
        record["status"] = "ORIGINAL_REJECT"
        if checked["status"] == "RETURNED":
            record.update(status="STRICT_FAIL", graph_status="REJECT",
                          source_return=returned_path_evidence(events, mode))
            NUMERICAL_FAILURES.append({"name": "successful source return versus rejected graph snapshot",
                                       "mode": mode, **record})
    report["rejected_snapshots"] = results
    report["rejected_snapshots_status"] = ("STRICT_FAIL" if any(row["status"] == "STRICT_FAIL" for row in results)
                                           else "ORIGINAL_REJECT" if results else "NONE")


def case(original, root, mode, whole=False, module_root=None):
    if whole:
        return whole_case(original, root, mode, module_root)
    previous_failures = len(NUMERICAL_FAILURES)
    fs, hop, size, count, bands, end, length = map(int, (root / "geometry.txt").read_text().split())
    assert (fs, hop, size) == (48000, 240, 2048)
    magnitude, filtered, logarithmic, onset = front_end(original, read(root, "pcm"), count)
    report = {"mode": mode, "reference": original_sources.PINS["madmom"],
              "frame_origin": "future / left edge; complete frames only",
              "float_policy": ("Float32 frontend/publication/CRF decoder; explicit DesktopReference tempo and CRF smoothing in Double through candidate/interval selection; source Double Hann before original FFT"
                               if mode == 2 else "Float32 graph analytics/publication; source Double Hann before original FFT")}
    # RealFourier supplies k=1..1024, source supplies k=0..1023.
    # The excluded DC and added Nyquist have provably zero filter weights.
    report["magnitude"] = compare("raw magnitude k1..1023", read(root, "magnitude").reshape(count, 1024)[:, :-1],
                                  np.asarray(magnitude)[:, 1:], atol=5e-5)
    report["filtered"] = compare("log-frequency magnitude", read(root, "filtered").reshape(count, bands), filtered)
    report["log"] = compare("log10(1 + filtered magnitude)", read(root, "log").reshape(count, bands), logarithmic)
    report["onset"] = compare("SuperFlux / automatic lag2", read(root, "onset"), onset)
    compare("bounded chronological ODF snapshot", read(root, "snapshot"), read(root, "onset")[end-length:end], atol=0, rtol=0)
    temporal(original, root, mode, report)
    report["numerical_comparison"] = "STRICT_FAIL" if len(NUMERICAL_FAILURES) != previous_failures else "PASS_ON_EXPORTED_CASE"
    return report


def main():
    global TRACKER_CACHE, TRACKER_ROOT
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("evidence", type=Path)
    parser.add_argument("--source-cache", type=Path, default=REPO / ".agent-workspace/rhythm-pc-max/upstream")
    parser.add_argument("--report", type=Path, help="New report path; existing reports are never overwritten")
    parser.add_argument("--whole", action="store_true", help="Complete finite input exports, separate from rolling captures")
    parser.add_argument("--whole-mode", type=int, choices=(0, 1, 2), help="One flat runner capture; omit for indexed 0/1/2 test exports")
    parser.add_argument("--defined-crf-root", type=Path)
    parser.add_argument("--legacy-defined-report", type=Path)
    parser.add_argument("--legacy-graph", type=Path)
    args = parser.parse_args()
    if args.whole and any(value is None for value in (args.defined_crf_root, args.legacy_defined_report, args.legacy_graph)):
        parser.error("--whole requires defined CRF build root and the explicit legacy graph/report binding")
    if args.whole_mode is not None and not args.whole:
        parser.error("--whole-mode requires --whole")
    report_path = args.report or args.evidence / "madmom-parity.json"
    if report_path.exists():
        raise FileExistsError(f"Preserve prior evidence report: {report_path}")
    pin = verify_sources(args.source_cache)
    original = original_sources.madmom(args.source_cache)
    TRACKER_CACHE = args.source_cache.resolve()
    TRACKER_ROOT = Path(tempfile.mkdtemp(prefix=report_path.stem + "-workers-", dir=report_path.parent)).resolve()
    reports = []
    failure = None
    legacy = None
    modes = [args.whole_mode] if args.whole_mode is not None else list(range(3))
    try:
        if args.whole:
            legacy = legacy_defined_binding(args.legacy_graph, args.legacy_defined_report)
        for mode in modes:
            root = args.evidence if args.whole_mode is not None else args.evidence / str(mode)
            reports.append(case(original, root, mode, args.whole, args.defined_crf_root))
        if NUMERICAL_FAILURES:
            failure = {"exception": "AssertionError", "stage": "strict_numerical_gates", "count": len(NUMERICAL_FAILURES)}
            raise AssertionError(f"{len(NUMERICAL_FAILURES)} strict numerical comparisons failed")
    except Exception as error:
        if failure is None:
            failure = {"exception": type(error).__name__, "message": str(error),
                       "mode": modes[len(reports)] if len(reports) < len(modes) else None}
        raise
    finally:
        output = {"pinned_revision": pin["revision"], "results": reports,
                  "whole_input": args.whole, "legacy_defined_source_binding": legacy,
                  "oracle_sha256": hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),
                  "source_modules_sha256": {name: hashlib.sha256(Path(module.__file__).read_bytes()).hexdigest()
                                            for name, module in vars(original).items()},
                  "defined_build": ({"source": json.loads((args.defined_crf_root / "manifest.json").read_text(encoding="utf8")),
                                     "binary": json.loads((args.defined_crf_root / "binary.json").read_text(encoding="utf8"))}
                                    if args.whole else None),
                  "complete": len(reports) == len(modes), "required_modes": modes, "failure": failure,
                  "numerical_failures": NUMERICAL_FAILURES,
                  "numerical_status": "STRICT_FAIL" if failure else "PASS_ON_EXPORTED_CASES",
                  "tracker_workers": str(TRACKER_ROOT), "tracker_timeout_seconds": 30,
                  "scope": "synthetic changing-tone exports only; no musical-quality claim"}
        with report_path.open("x", encoding="utf8") as report_file:
            report_file.write(json.dumps(output, indent=2) + "\n")


if __name__ == "__main__":
    if len(sys.argv) > 1 and sys.argv[1] == "--tracker-worker":
        tracker_worker(sys.argv[2:])
    else:
        main()
