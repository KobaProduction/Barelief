"""Evaluate production-graph CSV against separately read MIDI attack labels.

MIDI timing is a source-performance reference, not independently annotated
perceived beat/downbeat. The executable under test never receives this file.
"""
from __future__ import annotations

import argparse
from array import array
from collections import defaultdict
import csv
import hashlib
import importlib.metadata
import json
import math
from pathlib import Path
import platform

GROUP_SECONDS = 0.030
TOLERANCES = (0.050, 0.090)  # target, then historical comparison; never fitted
WARMUP_SECONDS = 8.0


def sha256(path):
    digest = hashlib.sha256()
    with Path(path).open("rb") as stream:
        for data in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(data)
    return digest.hexdigest()


def group_events(events, interval=GROUP_SECONDS):
    """Anchor each group to its first event, retaining strongest velocity."""
    grouped = []
    for event in sorted(events, key=lambda e: e["time"]):
        if grouped and event["time"] - grouped[-1]["time"] <= interval:
            grouped[-1]["strength"] = max(grouped[-1]["strength"], event["strength"])
            grouped[-1]["ready"] = max(grouped[-1]["ready"], event["ready"])
        else:
            grouped.append(dict(event))
    return grouped


def midi_reference(path):
    import mido
    midi = mido.MidiFile(path)
    if midi.type == 2 or midi.ticks_per_beat <= 0:
        raise ValueError("Only synchronous PPQ MIDI is a reference")
    now, notes, tempi = 0.0, [], []
    for message in midi:
        now += message.time
        if message.type == "set_tempo":
            tempi.append({"time": now, "bpm": mido.tempo2bpm(message.tempo)})
        if message.type == "note_on" and message.velocity:
            notes.append({"time": now, "ready": now, "strength": message.velocity / 127})
    if not tempi:
        tempi = [{"time": 0.0, "bpm": 120.0}]
    return notes, group_events(notes), tempi


def optimal_matches(predicted, reference, tolerance):
    """Maximum one-to-one cardinality, then minimum total timing error.

    Sorted monotone matching avoids the nearest-greedy counterexample where
    choosing one closer label makes the next otherwise valid event unmatched.
    """
    width = len(reference)
    previous = [(0, 0.0)] * (width + 1)
    directions = [bytearray(width + 1) for _ in range(len(predicted) + 1)]
    for i, observed in enumerate(predicted, 1):
        current = [(0, 0.0)] * (width + 1)
        for j, expected in enumerate(reference, 1):
            score, direction = previous[j], 1
            if current[j - 1] > score:
                score, direction = current[j - 1], 2
            error = abs(observed - expected)
            if error <= tolerance + 1e-12:
                candidate = (previous[j - 1][0] + 1, previous[j - 1][1] - error)
                if candidate > score:
                    score, direction = candidate, 3
            current[j], directions[i][j] = score, direction
        previous = current
    i, j, pairs = len(predicted), len(reference), []
    while i and j:
        direction = directions[i][j]
        if direction == 3:
            pairs.append((i - 1, j - 1))
            i, j = i - 1, j - 1
        elif direction == 1:
            i -= 1
        else:
            j -= 1
    return list(reversed(pairs))


def percentile(values, fraction):
    if not values:
        return None
    ordered = sorted(values)
    position = fraction * (len(ordered) - 1)
    first, last = math.floor(position), math.ceil(position)
    return ordered[first] + (ordered[last] - ordered[first]) * (position - first)


def event_metrics(predicted, reference, tolerance):
    predicted = sorted(predicted, key=lambda e: e["time"])
    pairs = optimal_matches([e["time"] for e in predicted], [e["time"] for e in reference], tolerance)
    count = len(pairs)
    precision = count / len(predicted) if predicted else None
    recall = count / len(reference) if reference else None
    f1 = 2 * count / (len(predicted) + len(reference)) if predicted or reference else None
    errors = [predicted[i]["time"] - reference[j]["time"] for i, j in pairs]
    latencies = [predicted[i]["ready"] - reference[j]["time"] for i, j in pairs]
    return {"tolerance_s": tolerance, "predicted": len(predicted), "reference": len(reference),
            "matches": count, "precision": precision, "recall": recall, "f1": f1,
            "timing_mae_s": sum(map(abs, errors)) / count if count else None,
            "availability_latency_p95_s": percentile(latencies, .95),
            "peak_confirmation_p95_s": percentile([e["ready"] - e["time"] for e in predicted], .95)}


def read_predictions(directory, filename="events.csv"):
    result = defaultdict(list)
    with (directory / filename).open(newline="") as stream:
        for row in csv.DictReader(stream):
            event = {"time": float(row["t_obs"]), "ready": float(row["t_ready"]), "strength": float(row["strength"])}
            if not all(math.isfinite(v) for v in event.values()) or event["ready"] < event["time"]:
                raise ValueError("Invalid recorded causal event")
            result[row["stream"]].append(event)
    return result


def read_features(directory):
    data = {name: [array("d") for _ in range(96)] for name in ("compact_float", "compact_u8")}
    counts = defaultdict(int)
    with (directory / "features.csv").open(newline="") as stream:
        for row in csv.DictReader(stream):
            name, feature, frame = row["stream"], int(row["feature"]), int(row["frame"])
            if name not in data or feature != counts[name] % 96 or frame != counts[name] // 96 + 1:
                raise ValueError("Noncontiguous or wrong-geometry recorded features")
            value = float(row["value"])
            if not math.isfinite(value) or value < 0:
                raise ValueError("Invalid recorded linear amplitude")
            data[name][feature].append(value)
            counts[name] += 1
    if not counts["compact_float"] or (counts["compact_u8"] and counts["compact_float"] != counts["compact_u8"]):
        raise ValueError("Missing Float32 features or mismatched optional quantization coverage")
    return data


def quantization_metrics(data):
    if not data["compact_u8"][0]:
        return None
    bands = []
    for band, (raw, quantized) in enumerate(zip(data["compact_float"], data["compact_u8"])):
        errors = [abs(a - b) for a, b in zip(raw, quantized)]
        bands.append({"feature": band, "frames": len(raw), "minimum": min(raw), "maximum": max(raw),
                      "q50": percentile(raw, .5), "q95": percentile(raw, .95), "q99": percentile(raw, .99),
                      "above_one": sum(v > 1 for v in raw), "zero_u8": sum(v == 0 for v in quantized),
                      "upper_u8": sum(v == 1 for v in quantized),
                      "positive_lost_to_zero": sum(a > 0 and b == 0 for a, b in zip(raw, quantized)),
                      "mae": sum(errors) / len(errors), "max_error": max(errors)})
    return bands


def timing_metrics(directory):
    data = defaultdict(dict)
    with (directory / "observations.csv").open(newline="") as stream:
        for row in csv.DictReader(stream):
            data[row["stream"]][int(row["frame"])] = float(row["process_us"])
    return {name: {"frames": len(values), "process_us_p50": percentile(list(values.values()), .5),
                   "process_us_p95": percentile(list(values.values()), .95),
                   "process_us_p99": percentile(list(values.values()), .99), "process_us_max": max(values.values())}
            for name, values in data.items()}


def runtime_metrics(directory, tempi):
    frames = defaultdict(dict)
    with (directory / "runtime.csv").open(newline="") as stream:
        for row in csv.DictReader(stream):
            frame = frames[int(row["frame"])]
            frame["time"] = float(row["t_obs"])
            frame[int(row["port"])] = float(row["value"]) if int(row["available"]) else None
    eligible = [f for f in frames.values() if f["time"] >= WARMUP_SECONDS]
    accepted = [f for f in eligible if f.get(3) == 2 and f.get(1) is not None]
    correct = 0
    for frame in accepted:
        bpm = max((t for t in tempi if t["time"] <= frame["time"]), key=lambda t: t["time"], default={"bpm": 120})["bpm"]
        correct += abs(frame[1] - bpm) <= .02 * bpm
    return {"warmup_s": WARMUP_SECONDS, "eligible_frames": len(eligible), "accepted_frames": len(accepted),
            "coverage": len(accepted) / len(eligible) if eligible else None,
            "strict_accuracy_vs_midi_clock": correct / len(accepted) if accepted else None,
            "determined_grid_frames": sum(f.get(11) == 2 for f in eligible),
            "human_beat_downbeat_accuracy": None}


def evaluate(directory, midi_path):
    summary = json.loads((directory / "summary.json").read_text())
    audio = Path(summary["input"])
    if sha256(audio) != summary["input_sha256"] or sha256(directory / "graph.json") != summary["graph_sha256"]:
        raise ValueError("Replay input/graph hash mismatch")
    notes, reference, tempi = midi_reference(midi_path)
    duration = summary["input_samples"] / summary["source_rate"]
    reference = [e for e in reference if 0 <= e["time"] <= duration]
    predictions = read_predictions(directory)
    fused = read_predictions(directory, "fused_events.csv") if (directory / "fused_events.csv").is_file() else None
    scores = {}
    for entry in summary["streams"]:
        name = entry["stream"]
        events = predictions[name]
        scores[name] = {"raw": [event_metrics(events, reference, tolerance) for tolerance in TOLERANCES]}
        if fused is not None:
            scores[name]["causal_fusion"] = [event_metrics(fused[name], reference, tolerance) for tolerance in TOLERANCES]
        if entry["channels"] > 1:
            scores[name]["offline_union_30ms"] = [event_metrics(group_events(events), reference, tolerance) for tolerance in TOLERANCES]
    return {"schema": "rhythm-evaluation.v1", "input_sha256": summary["input_sha256"], "midi_sha256": sha256(midi_path),
            "graph_sha256": summary["graph_sha256"], "midi_notes": len(notes), "attack_groups": len(reference), "tempo_metadata": tempi,
            "protocol": {"attack_grouping_s": GROUP_SECONDS, "tolerances_s": TOLERANCES, "warmup_s": WARMUP_SECONDS,
                         "matching": "maximum monotone one-to-one cardinality, then minimum total absolute timing error",
                         "multichannel_union": "offline diagnostic consolidation only; no deployed causal fusion or reaction latency claim",
                         "causal_fusion": "recorded causal detector/fusion output with pending-peak watermark and actual release readiness, when CSV is present",
                         "readiness": summary["readiness_model"], "reference": "MIDI attacks/clock, not independent perceived beats"},
            "event_metrics": scores, "quantization": quantization_metrics(read_features(directory)),
            "detector_timing": timing_metrics(directory), "runtime": runtime_metrics(directory, tempi),
            "versions": {"python": platform.python_version(), "mido": importlib.metadata.version("mido")},
            "evaluator_sha256": sha256(__file__),
            "csv_sha256": {p.name: sha256(p) for p in directory.glob("*.csv")}}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--replay", type=Path, required=True)
    parser.add_argument("--midi", type=Path, required=True)
    parser.add_argument("--out", type=Path, required=True)
    args = parser.parse_args()
    report = evaluate(args.replay, args.midi)
    with args.out.open("x", encoding="utf-8", newline="\n") as stream:
        json.dump(report, stream, ensure_ascii=False, indent=2, allow_nan=False)
        stream.write("\n")
    print(json.dumps({"events": report["event_metrics"], "runtime": report["runtime"]}, allow_nan=False))


if __name__ == "__main__":
    main()
