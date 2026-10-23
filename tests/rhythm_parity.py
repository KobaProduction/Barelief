"""Compare the native state machine to the pinned, unmodified research oracle."""
import importlib.util
import math
from pathlib import Path
import subprocess
import sys

SOURCE = Path(__file__).parent / "oracles/rhythm_reference.py"
spec = importlib.util.spec_from_file_location("rhythm_oracle", SOURCE)
oracle = importlib.util.module_from_spec(spec)
sys.modules[spec.name] = oracle
spec.loader.exec_module(oracle)
STATES = {"unknown": 0, "ambiguous": 1, "determined": 2}
KINDS = {"bass-like": 0, "snare-like": 1, "clap-like": 2, "hat-like": 3, "unknown": 4}


def event(time, kind, group):
    return dict(t_obs=time, strength=.9, kind=kind,
                provenance=group, provenance_group=group)


def frames(fps, scenario, seconds=8):
    width = 96 if fps == 45 else 64
    previous = -1
    for i in range(int(seconds * fps)):
        time = i / fps
        features = [0] * width
        accents = []
        beat = int(time * 2 + 1e-8)
        onset = beat != previous
        if scenario == "spectral" and onset:
            features[:width // 4] = [200] * (width // 4)
        if scenario in ("typed", "faults"):
            if onset:
                accents.append(event(beat / 2, "bass-like", "bass"))
            hat = math.floor((time - .25) * 2 + 1e-8)
            old_hat = math.floor(((i-1)/fps - .25) * 2 + 1e-8)
            if i and hat >= 0 and hat != old_hat:
                accents.append(event(.25 + hat / 2, "hat-like", "hat"))
        epoch = "1" if scenario != "faults" or i < fps * 4 else "2"
        saturated = scenario == "faults" and i == fps * 2
        seq = i + (1 if scenario == "faults" and i >= fps * 6 else 0)
        yield dict(source_epoch=epoch, seq=seq, sample_index=round(time*32768), sample_rate_hz=32768.,
                   t_obs=time, t_ready=time+.001, t_pub=time+.002, profile_id="r4",
                   features=features, event_candidates=None if scenario == "spectral" else accents, saturated=saturated)
        previous = beat


def changing_frames(fps):
    """Independent onset observations across a causal 90 -> 140 transition."""
    observations = []
    for start, stop, period in ((0., 6., 2/3), (6., 15., 3/7)):
        time = start
        while time < stop - 1e-8:
            observations.append(event(time, "bass-like", "bass"))
            if time + period/2 < stop:
                observations.append(event(time + period/2, "hat-like", "hat"))
            time += period
    observations.sort(key=lambda e: e["t_obs"])
    offset = 0
    for frame in frames(fps, "typed", seconds=15):
        accents = []
        while offset < len(observations) and observations[offset]["t_obs"] <= frame["t_obs"]:
            accents.append(observations[offset])
            offset += 1
        frame["event_candidates"] = accents
        yield frame


def selected(out):
    tempo, grid, dynamics = out["tempo"], out["rhythm_grid"], out["tempo_dynamics"]
    value = tempo["value"]
    candidates = value["candidates"]
    diagnostic = out["diagnostics"]["value"]
    events = out["accent_events"]["value"]
    change = dynamics["value"] or {}
    candidate = candidates[0] if candidates else {}
    grid_value = grid["value"] or {}
    return [STATES[tempo["status"]], value["bpm"], tempo["score"],
            out["evidence"]["value"]["period_candidate_support_raw"], len(candidates), candidate.get("bpm"), value["period_s"],
            STATES[grid["status"]], grid_value.get("phase_s"), grid_value.get("next_beat_prediction_s"),
            STATES[dynamics["status"]], change.get("bpm_per_s"), diagnostic["heavy_executed"],
            diagnostic["estimated_cost_units"], diagnostic["possible_cost_units"], diagnostic["history_frames"],
            diagnostic["valid_positive_changes"], len(events), events[-1]["event_seq"] if events else None,
            out["evidence"]["value"]["periodicity"], tempo["t_obs"], tempo["t_ready"], grid["t_obs"],
            change.get("from_bpm", change.get("from_acoustic_rate_per_min")),
            change.get("to_bpm", change.get("to_acoustic_rate_per_min")), change.get("t_confirm_s"),
            diagnostic["evidence_group_overflow"], diagnostic["pending_tempo_conflict_since"],
            diagnostic["pending_subdivision_conflict_since"], "from_acoustic_rate_per_min" in change,
            candidate.get("phase_support"), candidate.get("ioi_support"), candidate.get("acf_support")]


def encode(frame):
    values = [frame[k] for k in ("source_epoch", "seq", "sample_index", "sample_rate_hz", "t_obs", "t_ready", "t_pub")]
    values += [1, int(frame["saturated"]), *frame["features"]]
    accents = frame["event_candidates"]
    values.append(-1 if accents is None else len(accents))
    groups = {"spectral-onset": 0, "bass": 1, "hat": 2}
    for a in accents or []:
        group = groups[a["provenance_group"]]
        values += [a["t_obs"], a["strength"], KINDS[a["kind"]], group, group]
    return " ".join(map(str, values))


def compare(executable, fps, scenario, mode):
    inputs = list(changing_frames(fps) if scenario == "change" else frames(fps, scenario))
    engine = oracle.RhythmReference(mode, oracle.MethodConfig(feature_rate_fps=fps))
    expected = [selected(engine.step(f)) for f in inputs]
    mode_id = ("fixed-light", "fixed-full", "gated").index(mode)
    data = f"{mode_id} {fps} {len(inputs[0]['features'])}\n" + "\n".join(map(encode, inputs)) + "\n"
    process = subprocess.run([executable], input=data, text=True, capture_output=True, check=True, timeout=90)
    actual = [list(map(float, row.split())) for row in process.stdout.splitlines()]
    if len(actual) != len(expected):
        raise AssertionError((scenario, mode, "frame count", len(actual), len(expected)))
    for frame, (left, right) in enumerate(zip(actual, expected)):
        if len(left) != len(right):
            raise AssertionError(("field count", len(left), len(right)))
        for field, (a, b) in enumerate(zip(left, right)):
            same = math.isnan(a) if b is None else math.isclose(a, b, rel_tol=1e-7, abs_tol=2e-6)
            if not same:
                raise AssertionError((scenario, fps, mode, frame, field, a, b))
    print(f"PASS {scenario} {fps}fps {mode}: {len(inputs)} frames", flush=True)
    return len(inputs)


if __name__ == "__main__":
    total = 0
    for fps, scenario in ((30, "silence"), (45, "spectral"), (60, "typed"), (60, "faults"), (60, "change")):
        for mode in ("fixed-light", "fixed-full", "gated"):
            total += compare(sys.argv[1], fps, scenario, mode)
    print(f"PASS {total} frames x 33 output fields")
