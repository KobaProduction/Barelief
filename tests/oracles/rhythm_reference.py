#!/usr/bin/env python3
"""R4.3 minimal executable reference of the *joint* rhythm method.

This module implements a causal, deterministic PC algorithm for comparing
fixed-light / fixed-full / gated on IDENTICAL feature frames. It is not an
ESP32 benchmark, a calibrated probability model, or proof of musical beat.
Only Python standard-library modules are needed.

Unit convention: source sample index is int, all t_* / T / age are seconds,
BPM is musical beat candidates/min under a chosen tactus hypothesis;
estimated_cost_units is an abstract direct-ACF multiply-pair count, never µs.
"""
from __future__ import annotations

import argparse
from collections import deque
from dataclasses import asdict, dataclass
import json
import math
import sys
from typing import Any, Literal, Optional

Status = Literal["determined", "ambiguous", "unknown"]
Mode = Literal["fixed-light", "fixed-full", "gated"]
KIND = {"bass-like", "snare-like", "clap-like", "hat-like", "unknown"}
MODES = ("fixed-light", "fixed-full", "gated")


def require(condition: bool, message: str) -> None:
    if not condition:
        raise ValueError(message)


def finite(x: Any, name: str) -> float:
    require(isinstance(x, (float, int)) and not isinstance(x, bool), name + ": numeric required")
    v = float(x)
    require(math.isfinite(v), name + ": finite required")
    return v


@dataclass(frozen=True)
class AccentCandidate:
    """Evidence from an upstream detector, not an annotation / ground truth."""
    t_obs: float
    strength: float
    kind: str
    provenance: str
    provenance_group: str = "spectral-onset"

    @classmethod
    def parse(cls, o: dict[str, Any]) -> "AccentCandidate":
        require(isinstance(o, dict), "event_candidates item must be object")
        t = finite(o["t_obs"], "event.t_obs")
        s = finite(o["strength"], "event.strength")
        require(0 <= s <= 1, "event.strength outside [0,1]")
        kind = str(o.get("kind", "unknown"))
        require(kind in KIND, "unsupported event kind")
        prov = str(o.get("provenance", "external-onset"))
        require(bool(prov), "provenance required")
        group=str(o.get("provenance_group","spectral-onset"))
        require(bool(group), "provenance_group required")
        return cls(t, s, kind, prov, group)


@dataclass(frozen=True)
class FrameInput:
    """One causal feature frame. Time fields are in a common source clock.

    event_candidates=None: use the built-in spectral-flux onset observer.
    event_candidates=[]: an attached upstream observer found no events.
    These are observations, never reference beat/music labels.
    """
    source_epoch: str
    seq: int
    sample_index: int
    sample_rate_hz: float
    t_obs: float
    t_ready: float
    t_pub: float
    profile_id: str
    features: tuple[int, ...]
    event_candidates: Optional[tuple[AccentCandidate, ...]] = None
    budget_ops: Optional[int] = None
    saturated: bool = False

    @classmethod
    def parse(cls, o: dict[str, Any]) -> "FrameInput":
        require(isinstance(o, dict), "frame must be object")
        epoch, profile = str(o["source_epoch"]), str(o["profile_id"])
        require(bool(epoch) and bool(profile), "source_epoch and profile_id required")
        seq, idx = o["seq"], o["sample_index"]
        require(type(seq) is int and seq >= 0, "seq must be uint")
        require(type(idx) is int and idx >= 0, "sample_index must be uint")
        sr = finite(o["sample_rate_hz"], "sample_rate_hz")
        require(sr > 0, "sample_rate_hz must be positive")
        to, tr, tp = (finite(o[name], name) for name in ("t_obs", "t_ready", "t_pub"))
        require(0 <= to <= tr <= tp, "causality t_obs<=t_ready<=t_pub")
        raw = o["features"]
        require(isinstance(raw, (list, tuple)) and len(raw) in (64, 96), "features must contain 64 or 96 UInt8 values")
        require(all(type(x) is int and 0 <= x <= 255 for x in raw), "features must be UInt8")
        b = o.get("budget_ops")
        require(b is None or (type(b) is int and b >= 0), "budget_ops must be nonnegative integer or null")
        events = o.get("event_candidates")
        require(events is None or isinstance(events, list), "event_candidates must be null or array")
        e = None if events is None else tuple(AccentCandidate.parse(x) for x in events)
        if e is not None:
            require(all(0 <= x.t_obs <= to for x in e), "onset observation must not be in the future")
        saturated = o.get("saturated", False)
        require(type(saturated) is bool, "saturated must be bool")
        return cls(epoch, seq, idx, sr, to, tr, tp, profile, tuple(raw), e, b, saturated)


@dataclass(frozen=True)
class MethodConfig:
    feature_rate_fps: int = 60
    history_frames: int = 512
    acf_window: int = 128
    acf_min_frames: int = 90
    min_bpm: float = 60.0
    max_bpm: float = 180.0
    min_ioi_s: float = 0.15
    max_ioi_s: float = 2.0
    onset_threshold: float = 0.08
    onset_min_positive_u8: int = 8
    onset_refractory_s: float = 0.085
    min_events: int = 3
    max_provenance_groups: int = 8
    score_threshold: float = 0.65
    score_margin: float = 0.14
    phase_threshold: float = 0.65
    max_age_s: float = 1.5
    max_event_age_s: float = 12.0
    refinement_gap_frames: int = 4
    default_budget_ops: int = 30000
    octave_accent_min: float = 0.78
    octave_accent_margin: float = 0.18
    accent_phase_tolerance: float = 0.14
    accent_min_strength: float = 0.5
    subdivision_min_strength: float = 0.25
    accent_horizon_s: float = 8.0
    change_min_intervals: int = 3
    change_stability_relative: float = 0.10
    change_separation_relative: float = 0.17
    change_hold_s: float = 2.5
    change_min_strong_strength: float = 0.65
    phase_innovation_tolerance_fraction: float = 0.14
    relock_primary_period_tolerance: float = 0.025

    def __post_init__(self) -> None:
        require(self.feature_rate_fps in (30, 45, 60), "frame rate must use R4 profile 30/45/60")
        require(self.history_frames == 512, "R4 ring must be 512 frames")
        require(self.acf_min_frames >= 12 and self.acf_window >= self.acf_min_frames, "invalid ACF windows")
        require(self.acf_window <= self.history_frames, "ACF window must fit ring")
        require(0 < self.min_bpm < self.max_bpm, "invalid BPM range")
        require(self.refinement_gap_frames >= 1 and self.min_events >= 2, "invalid recurrence parameters")
        require(self.max_provenance_groups >= 1, "at least one provenance group required")
        require(self.onset_min_positive_u8 > 0 and 0 < self.onset_threshold < 1,
                "invalid relative spectral-onset thresholds")
        require(0 < self.octave_accent_min <= 1 and
                0 < self.octave_accent_margin <= 1, "invalid octave discrimination thresholds")
        require(0 < self.accent_phase_tolerance < .25, "invalid phase tolerance")
        require(0 <= self.accent_min_strength <= 1 and
                0 <= self.subdivision_min_strength <= 1, "invalid accent strength domain")
        require(self.accent_horizon_s > 0, "invalid accent horizon")
        require(self.change_min_intervals >= 2, "need repeated IOI for change detection")
        require(0 < self.change_stability_relative < self.change_separation_relative < .5,
                "invalid change-detection tolerances")
        require(self.change_hold_s > 0 and 0 <= self.change_min_strong_strength <= 1,
                "invalid change hold or strong threshold")
        require(0 < self.phase_innovation_tolerance_fraction < .25 and
                0 < self.relock_primary_period_tolerance < .1,
                "invalid causal relock parameters")


def field(value: Any, status: Status, score: Optional[float],
          t_obs: Optional[float], t_ready: Optional[float], t_pub: float,
          reason: Optional[str] = None) -> dict[str, Any]:
    require(status in ("determined", "ambiguous", "unknown"), "invalid status")
    if score is not None:
        require(0 <= score <= 1 and math.isfinite(score), "invalid score")
    if t_obs is not None:
        require(t_ready is not None and 0 <= t_obs <= t_ready <= t_pub,
                "invalid causal observation/ready/publication ordering")
    if status!="unknown" and value is not None:
        require(t_obs is not None and t_ready is not None,
                "nonempty determined/ambiguous estimate lacks causal timestamps")
    return {
        "value": value, "status": status, "score": score,
        "t_obs": t_obs, "t_ready": t_ready, "t_pub": t_pub,
        "age_s": None if t_obs is None else max(0, t_pub-t_obs),
        "reason": reason,
    }


def acf_scores(values: list[float], fr: int, lo_bpm: float, hi_bpm: float
               ) -> tuple[dict[float, float], int]:
    """Centered lag ACF with pair norms. ACF is periodicity, not true BPM."""
    L = len(values)
    lmin = max(1, math.ceil(60*fr/hi_bpm))
    lmax = min(L-8, math.floor(60*fr/lo_bpm))
    if L < 12 or lmax < lmin:
        return {}, 0
    av = sum(values)/L
    v = [x-av for x in values]
    out: dict[float, float] = {}
    ops = 0
    for lag in range(lmin, lmax+1):
        a, b = v[lag:], v[:L-lag]
        dot = sum(x*y for x,y in zip(a,b))
        aa,bb = sum(x*x for x in a), sum(x*x for x in b)
        ops += L-lag
        if aa > 1e-18 and bb > 1e-18:
            rho = max(-1., min(1.,dot / math.sqrt(aa*bb)))
            out[round(60*fr/lag, 3)] = max(0.,rho)
    return out,ops


def phase_fit(times: list[float], period: float) -> tuple[Optional[float], float]:
    if len(times)<2 or period <= 0:
        return None, 0.0
    t0 = times[0]
    re=sum(math.cos(2*math.pi*(t-t0)/period) for t in times)
    im=sum(math.sin(2*math.pi*(t-t0)/period) for t in times)
    r=math.hypot(re,im)/len(times)
    if r < 1e-12:
        return None, 0.0
    phase=(t0+math.atan2(im,re)*period/(2*math.pi)) % period
    return phase, min(1.0,r)


def ioi_support(times: list[float], period: float) -> float:
    if len(times) < 3 or period<=0:
        return 0.0
    residual=[]
    for a,b in zip(times,times[1:]):
        dt=b-a
        nearest=max(1,round(dt/period))
        residual.append(max(0.0,1.0 - abs(dt-nearest*period)/(0.18*period)))
    return sum(residual)/len(residual) if residual else 0.0


def candidate_periods(times: list[float], min_bpm: float, max_bpm: float
                      ) -> set[float]:
    out=set()
    for a,b in zip(times,times[1:]):
        dt=b-a
        if dt <=0:
            continue
        for scale in (0.5,1,2):
            p=dt*scale
            if 60/max_bpm <=p<=60/min_bpm:
                out.add(round(p,6))
    return out



def acf_resolution_equivalent(period_a: float, period_b: float, frame_rate: int) -> bool:
    """Lag-quantization equivalent, not independent musical-tempo evidence.

    An ACF at frame rate F sees only integer lag l; its temporal rounding
    uncertainty is at most 1/(2F). Also restrict the equivalence to 2% to
    avoid merging distinct slow tempos or true x2/x0.5 hypotheses. This is
    a *sufficient local alias filter*, not a BPM correctness certificate.
    """
    if not (period_a>0 and period_b>0 and frame_rate>0):
        return False
    difference=abs(period_a-period_b)
    return difference <= .5/frame_rate and difference/min(period_a,period_b) <= .02


def octave_competitors(bpm_a: float, bpm_b: float, tolerance: float = .015) -> bool:
    """Detect doubled/halved candidate beat rates without a hard-coded BPM."""
    low,high=sorted((bpm_a,bpm_b))
    return low>0 and abs(high/(2*low)-1)<=tolerance


def hierarchy_evidence(events: list[AccentCandidate], period: float,
                       cfg: MethodConfig, now: float) -> dict[str, Any]:
    """Optional *metrical hypothesis*, conditional on typed independent events.

    High-salience bass/clap/snare-like events are presumed plausible main beats;
    hat-like events are plausible subdivisions. Different provenance groups
    and distinct event times are required. This is a falsifiable inductive
    assumption, NOT a universal meter rule (e.g. syncopation/backbeat).
    """
    result={"available":False,"score":None,"strong_phase_s":None,
            "strong_fit":None,"strong_coverage":None,"weak_offbeat":None,
            "strong_count":0,"subdivision_count":0,"provenance_groups":[],
            "reason":"no-independent-accent-evidence"}
    if not (period>0 and math.isfinite(period)):
        return result
    recent=[e for e in events if 0<=now-e.t_obs<=cfg.accent_horizon_s]
    strong=[e for e in recent if e.kind in {"bass-like","snare-like","clap-like"}
            and e.strength>=cfg.accent_min_strength]
    weak=[e for e in recent if e.kind=="hat-like"
          and e.strength>=cfg.subdivision_min_strength]
    sg={e.provenance_group for e in strong}
    wg={e.provenance_group for e in weak}
    if len(strong)<3 or len(weak)<2 or not (sg-wg and wg-sg):
        return result
    # Dedup independent source observations by timestamp; require a genuine
    # phase-separated weak stream, not a second label on the same onset.
    st=sorted({round(e.t_obs,6) for e in strong})
    wt=sorted({round(e.t_obs,6) for e in weak})
    if len(st)<3 or len(wt)<2 or any(abs(a-b)<.08*period
                                     for a in st for b in wt):
        return result
    if st[-1]-st[0]<2*period:
        return result
    phase,fit=phase_fit(st,period)
    if phase is None:
        # When contrasting, independently observed type streams exist and
        # salient attacks cancel at period T, this is NEGATIVE evidence for
        # that particular phase grid, not absence of evidence.
        result.update({"available":True,"score":0.0,"strong_fit":0.0,
                       "strong_coverage":0.0,"weak_offbeat":0.0,
                       "strong_count":len(st),"subdivision_count":len(wt),
                       "provenance_groups":sorted(sg|wg),
                       "reason":"strong-phase-cancellation"})
        return result
    # Each predicted beat grid position in the *observed* strong span must
    # be supported by a typed salient accent. Counting only observed accents
    # would falsely assign both T and T/2 a perfect score.
    first=math.ceil((st[0]-phase)/period-1e-8)
    last=math.floor((st[-1]-phase)/period+1e-8)
    ticks=[phase+k*period for k in range(first,last+1)]
    if len(ticks)<3:
        return result
    tol=cfg.accent_phase_tolerance*period
    occupied=sum(1 for tick in ticks if any(abs(t-tick)<=tol for t in st))
    coverage=occupied/len(ticks)
    off=sum(1 for t in wt
            if abs((t-phase-period/2+period/2)%period-period/2)<=tol)
    weak_offbeat=off/len(wt)
    score=min(1.,max(0.,.5*fit+.3*coverage+.2*weak_offbeat))
    result.update({"available":True,"score":round(score,6),
                   "strong_phase_s":round(phase,8),
                   "strong_fit":round(fit,6),
                   "strong_coverage":round(coverage,6),
                   "weak_offbeat":round(weak_offbeat,6),
                   "strong_count":len(st),"subdivision_count":len(wt),
                   "provenance_groups":sorted(sg|wg),"reason":None})
    return result



class RhythmReference:
    """Single causal state machine; modes differ ONLY in heavy scheduling policy."""

    def __init__(self, mode: Mode = "gated", config: Optional[MethodConfig] = None):
        require(mode in MODES, "unknown mode")
        self.mode = mode
        self.cfg = config or MethodConfig()
        self.reset()

    def reset(self) -> None:
        self.epoch: Optional[str] = None
        self.profile: Optional[str] = None
        self.sample_rate: Optional[float] = None
        self.seq: Optional[int] = None
        self.sample_index: Optional[int] = None
        self.t_obs: Optional[float] = None
        self.features: Optional[tuple[int,...]] = None
        self.feature_history: deque[tuple[int,...]] = deque(maxlen=self.cfg.history_frames)
        self.flux_history: deque[float] = deque(maxlen=self.cfg.history_frames-1)
        self.event_history: deque[float] = deque(maxlen=96)
        self.group_times: dict[str,deque[float]] = {}
        # Causal observer events contain strength/kind/provenance in addition
        # to timestamps; this is NOT a musical beat annotation.
        self.typed_events: deque[AccentCandidate] = deque(maxlen=256)
        self.evidence_group_overflow = 0
        self.last_onset: Optional[float] = None
        self.last_heavy_seq: Optional[int] = None
        self.last_heavy_obs: Optional[float] = None
        self.acf: dict[float,float] = {}
        self.event_seq = 0
        self.seen_event_keys: deque[tuple[float,str,str]] = deque(maxlen=256)
        self.last_determined: Optional[tuple[float,float]] = None
        self.strong_times: dict[str,deque[float]] = {}
        self.stable_strong_period: Optional[float] = None
        # A snare/backbeat provenance at half rate is an additional metric
        # level, not evidence that the original bass track changed tempo.
        self.stable_group_periods: dict[str,float] = {}
        self.tempo_anchor_group: Optional[str] = None
        self.pending_tempo_conflict_since: Optional[float] = None
        self.pending_subdivision_conflict_since: Optional[float] = None
        self.subdivision_recovery_count=0
        # Acoustic onset cadence is NOT a beat/tactus decision. It is a
        # separate, explicitly ambiguous observable of a single DSP source.
        self.spectral_last_event_s: Optional[float] = None
        self.spectral_initial_iois: deque[float] = deque(maxlen=self.cfg.change_min_intervals)
        self.spectral_stable_period_s: Optional[float] = None
        self.spectral_pending_iois: deque[float] = deque(maxlen=self.cfg.change_min_intervals)
        self.last_spectral_rate_change: Optional[dict[str,Any]] = None
        self.last_change: Optional[dict[str,Any]] = None
        self.tracking_start_s: Optional[float] = None

    def _ingest_events(self, f: FrameInput, flux: float,
                       contributions: list[float],
                       onset_contrast: float) -> list[dict[str,Any]]:
        if f.event_candidates is None:
            # Absolute mean flux divides sparse true onsets by ALL 64/96
            # bands, silently suppressing PCM-derived events. Gate relative
            # positive spectral change with a minimum absolute UInt8 rise to
            # avoid calling a 1-count quantization fluctuation an attack.
            positive_u8=sum(contributions)*255
            if (self.features is None or positive_u8<self.cfg.onset_min_positive_u8
                    or onset_contrast<self.cfg.onset_threshold):
                candidates: tuple[AccentCandidate,...] = ()
            elif self.last_onset is not None and f.t_obs-self.last_onset < self.cfg.onset_refractory_s:
                candidates = ()
            else:
                # Partition = feature-bank hypothesis, not instrument recognition.
                n=len(contributions)
                bands=[sum(contributions[i*n//4:(i+1)*n//4]) for i in range(4)]
                tags=("bass-like","snare-like","clap-like","hat-like")
                index=max(range(4),key=lambda k:bands[k])
                candidates=(AccentCandidate(f.t_obs,min(1,onset_contrast),tags[index],
                                            "spectral-flux:"+f.profile_id),)
        else:
            candidates=f.event_candidates
        events=[]
        signatures=set()
        for item in sorted(candidates,key=lambda c:(c.t_obs,c.provenance,c.kind)):
            key=(round(item.t_obs,6),item.provenance,item.kind)
            if key in signatures or key in self.seen_event_keys:
                continue
            signatures.add(key)
            self.seen_event_keys.append(key)
            record={
                "source_epoch":f.source_epoch,
                "event_seq":self.event_seq,
                "frame_seq":f.seq,
                "t_obs":item.t_obs,
                "t_ready":f.t_ready,
                "t_pub":f.t_pub,
                "kind":item.kind,
                "strength":item.strength,
                "score":item.strength,
                "status":"ambiguous" if item.kind=="unknown" else "determined",
                "provenance":item.provenance,
                "provenance_group":item.provenance_group,
            }
            if (item.provenance_group not in self.group_times
                    and len(self.group_times) >= self.cfg.max_provenance_groups):
                # Preserve the accent event, but do not silently allocate
                # unlimited new hypothesis groups or trust it as tempo evidence.
                record["evidence_omitted"]="provenance-group-capacity"
                self.evidence_group_overflow += 1
            else:
                group=self.group_times.setdefault(item.provenance_group,deque(maxlen=96))
                if not group or item.t_obs-group[-1]>1e-6:
                    group.append(item.t_obs)
                self.typed_events.append(item)
            self.event_seq+=1
            events.append(record)
            # IOI operates on distinct chronological acoustic *times*. Duplicate
            # spectral reports at identical t_obs must not multiply the votes.
            if not self.event_history or item.t_obs-self.event_history[-1] > 1e-6:
                self.event_history.append(item.t_obs)
                self.last_onset=item.t_obs
        while self.event_history and f.t_obs-self.event_history[0]>self.cfg.max_event_age_s:
            self.event_history.popleft()
        for group in self.group_times.values():
            while group and f.t_obs-group[0]>self.cfg.max_event_age_s:
                group.popleft()
        while self.typed_events and f.t_obs-self.typed_events[0].t_obs>self.cfg.accent_horizon_s:
            self.typed_events.popleft()
        return events

    def _causal_tempo_change(self, emitted: list[dict[str,Any]]) -> Optional[dict[str,Any]]:
        """Confirm a new strong-accent IOI only after repeated arrivals.

        The observer never reads future frames or reference BPM labels. A
        change is a *hypothesis* of acoustic beat period, not proof of music
        tempo. Existing emitted events and event_seq must remain intact.
        """
        result=None
        for event in emitted:
            if (event["kind"] not in ("bass-like","snare-like","clap-like")
                    or event["strength"]<self.cfg.change_min_strong_strength
                    or "evidence_omitted" in event):
                continue
            group=event["provenance_group"]
            history=self.strong_times.setdefault(group,deque(maxlen=24))
            t=event["t_obs"]
            if history and t<=history[-1]+1e-6:
                continue
            history.append(t)
            # Causal early warning: one contradictory IOI cannot confirm a
            # change, but it already invalidates a confident unchanged tempo
            # until the incumbent source returns or a new episode locks.
            if (group==self.tempo_anchor_group
                    and self.stable_strong_period is not None and len(history)>=2):
                newest=history[-1]-history[-2]
                mismatch=abs(newest-self.stable_strong_period)/self.stable_strong_period
                if mismatch>self.cfg.change_separation_relative:
                    self.pending_tempo_conflict_since=t
                else:
                    self.pending_tempo_conflict_since=None
            need=self.cfg.change_min_intervals
            if len(history)<need+1:
                continue
            recent=list(history)[-need-1:]
            intervals=[b-a for a,b in zip(recent,recent[1:])]
            candidate=sum(intervals)/len(intervals)
            if not (60/self.cfg.max_bpm <=candidate<=60/self.cfg.min_bpm):
                continue
            if max(abs(x-candidate) for x in intervals)>self.cfg.change_stability_relative*candidate:
                continue
            baseline=self.stable_strong_period
            group_baseline=self.stable_group_periods.get(group)
            if group_baseline is None:
                # A new independent stream must establish its own historical
                # rate; comparing its initial IOI with another stream's rate
                # confuses metric subdivisions and tempo changes.
                self.stable_group_periods[group]=candidate
                if baseline is None:
                    self.stable_strong_period=candidate
                    self.tempo_anchor_group=group
                continue
            group_deviation=abs(candidate-group_baseline)/group_baseline
            if group_deviation<=self.cfg.change_separation_relative:
                self.stable_group_periods[group]=.9*group_baseline+.1*candidate
                if group==self.tempo_anchor_group:
                    self.stable_strong_period=self.stable_group_periods[group]
                continue
            if group!=self.tempo_anchor_group:
                # Without agreement from the incumbent track, this is only
                # an unconfirmed acoustic group transition, not a tactus
                # switch. A later relock policy can explicitly handle a
                # disappeared incumbent after independent evidence arrives.
                continue
            if baseline is None:
                continue
            deviation=abs(candidate-baseline)/baseline
            if deviation<=self.cfg.change_separation_relative:
                continue
            start=recent[0]
            self.stable_group_periods[group]=candidate
            self.stable_strong_period=candidate
            self.pending_tempo_conflict_since=None
            self.pending_subdivision_conflict_since=None
            self.subdivision_recovery_count=0
            self.tracking_start_s=start
            # Relock only the period/phase evidence windows. In particular,
            # do not reset the event sequence or erase emitted FIFO records.
            for g,times in self.group_times.items():
                self.group_times[g]=deque((v for v in times if v>=start-1e-6),maxlen=96)
            self.event_history=deque((v for v in self.event_history if v>=start-1e-6),maxlen=96)
            self.typed_events=deque((v for v in self.typed_events if v.t_obs>=start-1e-6),maxlen=256)
            self.acf={}
            self.last_heavy_obs=None
            self.last_heavy_seq=None
            self.last_determined=None
            t_start=start
            from_bpm=60/baseline
            to_bpm=60/candidate
            kind="acceleration" if to_bpm>from_bpm else "deceleration"
            result={"kind":kind,"from_bpm":round(from_bpm,4),
                    "to_bpm":round(to_bpm,4),
                    "change_point_s":round(t_start,8),
                    "t_confirm_s":round(t,8),
                    "relock_intervals":need,
                    "ioi_relative_spread":round(max(abs(x-candidate) for x in intervals)/candidate,6),
                    "source_group":group,
                    "reason":"causal-consecutive-salient-ioi"}
            self.last_change=result
        return result

    def _causal_spectral_rate_change(self, emitted: list[dict[str,Any]]) -> Optional[dict[str,Any]]:
        """Observe a changed PCM-onset cadence, WITHOUT identifying musical BPM.

        Each interval Δ_i=t_i−t_(i−1) comes from one real acoustic-onset
        source, never from the reference grid, oracle tempo, or future frames.
        A baseline requires K stable intervals. Three stable contradictory
        intervals are needed for a *candidate* rate transition; missing events,
        one extra click, and silence cannot independently confirm one. This
        signal is ambiguous even when the acoustic intervals are perfectly
        periodic: a repeated subdivision can be mistaken for the tactus.
        """
        need=self.cfg.change_min_intervals
        for e in emitted:
            if (e["provenance_group"]!="spectral-onset"
                    or "evidence_omitted" in e):
                continue
            t=e["t_obs"]
            previous=self.spectral_last_event_s
            self.spectral_last_event_s=t
            if previous is None:continue
            delta=t-previous
            if not (60/self.cfg.max_bpm<=delta<=60/self.cfg.min_bpm):
                self.spectral_pending_iois.clear()
                self.spectral_initial_iois.clear()
                continue
            if self.spectral_stable_period_s is None:
                self.spectral_initial_iois.append(delta)
                if len(self.spectral_initial_iois)==need:
                    avg=sum(self.spectral_initial_iois)/need
                    if max(abs(x-avg) for x in self.spectral_initial_iois) <= self.cfg.change_stability_relative*avg:
                        self.spectral_stable_period_s=avg
                    else:
                        self.spectral_initial_iois.popleft()
                continue
            baseline=self.spectral_stable_period_s
            mismatch=abs(delta-baseline)/baseline
            if mismatch<=self.cfg.change_separation_relative:
                self.spectral_pending_iois.clear()
                # Drift within stable evidence is tracked slowly, not reset by
                # one unusual but admissible onset interval.
                self.spectral_stable_period_s=.9*baseline+.1*delta
                continue
            if self.spectral_pending_iois:
                candidate=sum(self.spectral_pending_iois)/len(self.spectral_pending_iois)
                if abs(delta-candidate)>self.cfg.change_stability_relative*candidate:
                    self.spectral_pending_iois.clear()
            self.spectral_pending_iois.append(delta)
            if len(self.spectral_pending_iois)<need:continue
            new_period=sum(self.spectral_pending_iois)/need
            spread=max(abs(x-new_period) for x in self.spectral_pending_iois)/new_period
            if (spread>self.cfg.change_stability_relative
                    or abs(new_period-baseline)/baseline<=self.cfg.change_separation_relative):
                continue
            self.spectral_stable_period_s=new_period
            self.spectral_pending_iois.clear()
            former=60/baseline;new=60/new_period
            change={"kind":"acceleration" if new>former else "deceleration",
                    "from_acoustic_rate_per_min":round(former,4),
                    "to_acoustic_rate_per_min":round(new,4),
                    "t_confirm_s":round(t,8),"source_group":"spectral-onset",
                    "evidence_intervals":need,
                    "ioi_relative_spread":round(spread,6),
                    "interpretation":"acoustic-onset-rate-candidate-not-musical-tactus"}
            self.last_spectral_rate_change=change
            return change
        return None

    def _causal_subdivision_innovation(self, emitted: list[dict[str,Any]]) -> None:
        """Weak-event phase innovation suspends a prior strong-source lock.

        The bound uses only already accepted source timestamps and one
        independently observed prior weak-accent stream. This is evidence of
        model failure, NOT proof of new BPM (syncopation remains possible).
        """
        g=self.tempo_anchor_group
        T=self.stable_strong_period
        if not (g is not None and T is not None and
                self.strong_times.get(g)):
            return
        strong=self.strong_times[g][-1]
        tol=self.cfg.phase_innovation_tolerance_fraction*T
        for event in emitted:
            if (event["kind"]!="hat-like" or event["provenance_group"]==g
                    or "evidence_omitted" in event
                    or event["strength"]<self.cfg.subdivision_min_strength):
                continue
            group=event["provenance_group"]
            # At least three past events from one independent observer are
            # required before using its offsets to challenge the incumbent.
            history=self.group_times.get(group)
            if history is None or len(history)<4:
                continue
            dt=event["t_obs"]-strong
            if not (0 <= dt <= T):
                continue
            residual=abs(dt-T/2)
            if residual>tol:
                self.pending_subdivision_conflict_since=event["t_obs"]
                self.subdivision_recovery_count=0
            elif self.pending_subdivision_conflict_since is not None:
                # A single spurious hat onset is a reason to suspend, not
                # a permanent lockout. Require three subsequent causal
                # phase-consistent weak observations to restore support.
                self.subdivision_recovery_count+=1
                if self.subdivision_recovery_count>=self.cfg.change_min_intervals:
                    self.pending_subdivision_conflict_since=None
                    self.subdivision_recovery_count=0

    def _confirmed_relock_candidate(self, ranked: list[dict[str,Any]],
                                    now: float) -> Optional[dict[str,Any]]:
        """Causal primary-IOI and typed accent evidence after confirmed change.

        Distinct from a generic ACF/IOI score: the incumbent strong group
        supplies three recent stable intervals and independently timestamped
        weak subdivisions must support the same tempo/phase. No reference BPM
        or oracle onset locations are supplied to this state machine.
        """
        change=self.last_change
        group=self.tempo_anchor_group
        T=self.stable_strong_period
        if not (change and group and T and
                0 <= now-change["t_confirm_s"]<=self.cfg.change_hold_s and
                self.pending_tempo_conflict_since is None and
                self.pending_subdivision_conflict_since is None):
            return None
        recent=list(self.strong_times.get(group,()))[-self.cfg.change_min_intervals-1:]
        if len(recent)<self.cfg.change_min_intervals+1 or now-recent[-1]>self.cfg.max_age_s:
            return None
        durations=[b-a for a,b in zip(recent,recent[1:])]
        empirical=sum(durations)/len(durations)
        if max(abs(x-empirical) for x in durations)>self.cfg.change_stability_relative*empirical:
            return None
        qualified=[c for c in ranked if
                   abs(c["period_s"]-empirical)/empirical<=self.cfg.relock_primary_period_tolerance
                   and c["accent_discriminator"]["available"]
                   and c["accent_discriminator"]["strong_coverage"] is not None
                   and c["accent_discriminator"]["strong_coverage"]>=.9
                   and c["accent_discriminator"]["score"] is not None
                   and c["accent_discriminator"]["score"]>=self.cfg.octave_accent_min
                   and c["phase_candidates"]["tactus_phase_s"] is not None]
        if not qualified:
            return None
        chosen=min(qualified,key=lambda c:(abs(c["period_s"]-empirical),-c["score"]))
        # If an octave competitor has equivalent typed accent support, never
        # assert a unique tactus solely from a short strong-event cadence.
        for rival in ranked:
            if octave_competitors(rival["bpm"],chosen["bpm"]):
                other=rival["accent_discriminator"]
                if (other["available"] and other["score"] is not None and
                        chosen["accent_discriminator"]["score"]-other["score"] <
                        self.cfg.octave_accent_margin):
                    return None
        return chosen

    def _feature_flux(self, f: FrameInput) -> tuple[float,list[float],float]:
        if self.features is None:
            return 0.,[0.0]*len(f.features),0.
        contributions=[max(0,cur-old)/255 for cur,old in zip(f.features,self.features)]
        # Relative contrast is a separate acoustic-onset heuristic; the
        # original mean flux remains the same-input ACF time series.
        onset_contrast=sum(contributions)*255/max(
            1,sum(f.features),sum(self.features))
        return sum(contributions)/len(contributions),contributions,onset_contrast

    def _potential_acf_ops(self) -> int:
        L=min(self.cfg.acf_window,len(self.flux_history))
        if L<self.cfg.acf_min_frames:
            return 0
        lo=max(1,math.ceil(60*self.cfg.feature_rate_fps/self.cfg.max_bpm))
        hi=min(L-8,math.floor(60*self.cfg.feature_rate_fps/self.cfg.min_bpm))
        return sum(L-lag for lag in range(lo,hi+1)) if hi>=lo else 0

    def _rank(self, times: list[float], acf: dict[float,float],
              t_now: float) -> list[dict[str,Any]]:
        # IOI is calculated WITHIN each declared provenance group, never
        # from intervals between simultaneous, distinct source streams.
        active={g:list(xs) for g,xs in self.group_times.items() if len(xs)>=3}
        # Never create a phantom 'spectral-onset' source solely because ACF
        # was computed. Without observed events in that provenance, its zero
        # support is NOT an independent vote against valid bass/hat IOIs.
        # Real spectral-onset groups already enter through self.group_times;
        # their ACF and IOI remain combined once, avoiding double counting.
        periods=set()
        for gt in active.values():
            periods.update(candidate_periods(gt,self.cfg.min_bpm,self.cfg.max_bpm))
        periods.update(round(60/bpm,6) for bpm,s in acf.items() if s>0)
        # Explicit octave shadows cannot be eliminated merely because the
        # underlying detector emitted a subdivision interval (T/4 instead
        # of T/2). A shadow has NO fabricated positive source support.
        observed=list(periods)
        for period in observed:
            for alternative in (period/2,period*2):
                if 60/self.cfg.max_bpm<=alternative<=60/self.cfg.min_bpm:
                    periods.add(round(alternative,6))
        ranking=[]
        for period in sorted(periods):
            bpm=60/period
            ac=max((score for v,score in acf.items() if abs(v-bpm)<1.05),default=0.)
            per_group={}
            io_best=0.0
            for group,gt in active.items():
                io=ioi_support(gt,period)
                io_best=max(io_best,io)
                # Source-derived IOI and ACF belong to the SAME spectral
                # evidence group; do not count them as independent votes.
                per_group[group]=max(io,ac) if group=="spectral-onset" else io
            support=sum(per_group.values())/len(per_group) if per_group else 0.
            if active:
                best_phase=sorted(
                    (phase_fit(gt,period)[1],phase_fit(gt,period)[0],group)
                    for group,gt in active.items())
                r,phase,phase_group=best_phase[-1]
                phase_t_obs=active[phase_group][-1] if active[phase_group] else None
            else:
                phase,r=phase_fit(times,period)
                phase_group=None
                phase_t_obs=times[-1] if times else None
            evidence_times=[gt[-1] for group,gt in active.items()
                            if gt and per_group[group]>0]
            if ac>0 and self.last_heavy_obs is not None:
                evidence_times.append(self.last_heavy_obs)
            support_t_obs=max(evidence_times,default=None)
            if self.last_determined is None:
                temporal_penalty=0.
            else:
                old_bpm,old_t=self.last_determined
                temporal_penalty=(0. if t_now-old_t>self.cfg.max_age_s else
                                  min(.1,.1*abs(bpm-old_bpm)/max(1,old_bpm)))
            # A candidate with incoherent observed phases loses support, but
            # even coherent phase cannot establish the musical tactus.
            phase_penalty=0.1*(1-r) if phase is not None else 0.0
            rank=max(0.,support-temporal_penalty-phase_penalty)
            accent=hierarchy_evidence(list(self.typed_events),period,self.cfg,t_now)
            # Distinguish a candidate's most coherent observed phase from a
            # causal candidate for the tactus. High-hat coherence is not an
            # acceptable beat anchor for a bass-like decisive octave vote.
            strong_events=[e for e in self.typed_events
                           if e.kind in ("bass-like","snare-like","clap-like")
                           and e.strength>=self.cfg.accent_min_strength]
            hat_events=[e for e in self.typed_events if e.kind=="hat-like"]
            strong_groups=set(accent["provenance_groups"]) if accent["available"] else set()
            salient=[e for e in strong_events if e.provenance_group in strong_groups]
            hat=[e for e in hat_events if e.provenance_group in strong_groups]
            s_times=sorted({round(e.t_obs,6) for e in salient})
            w_times=sorted({round(e.t_obs,6) for e in hat})
            anchor=accent["strong_phase_s"] if accent["available"] else None
            sf=phase_fit(s_times,period)[1] if len(s_times)>=2 else 0.
            subphase,subfit=phase_fit(w_times,period)
            phase_candidates={
                "tactus_phase_s":anchor,
                "tactus_support":round(sf,6) if anchor is not None else None,
                "tactus_t_obs":max(s_times,default=None) if anchor is not None else None,
                "subdivision_phase_s":None if subphase is None else round(subphase,8),
                "subdivision_support":round(subfit,6) if subphase is not None else None,
                "subdivision_t_obs":max(w_times,default=None) if subphase is not None else None,
                "generic_phase_s":None if phase is None else round(phase,8),
                "generic_provenance_group":phase_group,
                "anchor_kind":"typed-salient-onset" if anchor is not None else None,
            }
            ranking.append({
                "bpm":round(bpm,4),"period_s":period,
                "score":round(rank,6),
                "accent_discriminator":accent,
                "phase_candidates":phase_candidates,
                "ioi_support":round(io_best,6),"acf_support":round(ac,6),
                "group_support":{k:round(v,6) for k,v in sorted(per_group.items())},
                "phase_s":None if phase is None else round(phase,8),
                "phase_support":round(r,6),"phase_provenance_group":phase_group,
                "phase_penalty":round(phase_penalty,6),
                "t_obs":support_t_obs,
                "phase_t_obs":phase_t_obs,
                "age_s":max(0.,t_now-support_t_obs) if support_t_obs is not None else None,
            })
        # A 60-fps ACF integer lag near 140 BPM is 26 frames (138.46
        # BPM), whereas precise event IOIs give 140 BPM. Their 1.1% gap
        # is bin quantization, not independent contrary tempo evidence.
        # Cluster close candidates within 1.5%, but never octave levels.
        # The tolerance is an explicit finite-resolution hypothesis to test.
        families=[]
        for h in sorted(ranking,key=lambda c:c["bpm"]):
            if (families and
                    abs(h["bpm"]-families[-1][-1]["bpm"]) <= .015*h["bpm"] and
                    abs(h["bpm"]-families[-1][0]["bpm"]) <= .03*h["bpm"]):
                # Also bound a transitive cluster's full width; otherwise
                # many overlapping 1.5% steps could absorb distinct tempi.
                families[-1].append(h)
            else:
                families.append([h])
        ranking=[max(family,key=lambda c:(
            c["accent_discriminator"]["available"],
            c["accent_discriminator"]["score"] or 0.,
            c["score"],-c["bpm"])) for family in families]
        ranking.sort(key=lambda x:(-x["score"],x["bpm"]))
        # Keep a small set of leading period hypotheses AND the matching
        # octave competitors, including a typed-accent-supported candidate
        # whose generic periodicity is relatively weak. Truncating solely
        # by IOI/ACF would silently restore false "determined" decisions.
        leading=ranking[:12]
        salient=sorted((c for c in ranking if c["accent_discriminator"]["available"]),
                       key=lambda c:-c["accent_discriminator"]["score"])[:2]
        keep={c["period_s"]:c for c in leading+salient}
        for chosen in list(keep.values()):
            for scale in (.5,2.):
                aim=chosen["bpm"]*scale
                if not self.cfg.min_bpm<=aim<=self.cfg.max_bpm:
                    continue
                matching=(c for c in ranking
                          if abs(c["bpm"]-aim)<=.015*aim)
                close=min(matching,key=lambda c:(abs(c["bpm"]-aim),-c["score"]),
                          default=None)
                if close is not None:
                    keep[close["period_s"]]=close
        return sorted(keep.values(),key=lambda c:(-c["score"],c["bpm"]))

    def _octave_decision(self, ranked: list[dict[str,Any]], now: float
                         ) -> dict[str,Any]:
        pairs=[(a["bpm"],b["bpm"]) for i,a in enumerate(ranked)
               for b in ranked[i+1:]
               if octave_competitors(a["bpm"],b["bpm"])]
        if not pairs:
            return {"status":"not-applicable","chosen_bpm":None,
                    "score_gap":None,"reason":"no-octave-alternatives",
                    "provenance_groups":[]}
        scored=[c for c in ranked if c["accent_discriminator"]["available"]]
        if not scored:
            return {"status":"ambiguous","chosen_bpm":None,
                    "score_gap":None,"reason":"no-independent-accent-evidence",
                    "provenance_groups":[]}
        scored.sort(key=lambda c:(-c["accent_discriminator"]["score"],-c["score"],c["bpm"]))
        winner=scored[0]
        # Octave competition is a relation BETWEEN the proposed winning
        # tactus and its half/double hypotheses. Another unrelated pair
        # (e.g. 66.7/133.3 while the verified winner is 100 BPM) cannot
        # invalidate that 100-BPM decision. Do not bypass this check when
        # the source-backed winner differs from the generic top candidate.
        relevant=[c for c in ranked if c is not winner and
                  octave_competitors(c["bpm"],winner["bpm"])]
        if not relevant and winner is ranked[0]:
            return {"status":"not-applicable","chosen_bpm":None,
                    "score_gap":None,"reason":"unrelated-octave-pair",
                    "provenance_groups":[]}
        if len(scored)<2:
            return {"status":"ambiguous","chosen_bpm":None,
                    "score_gap":None,"reason":"no-independent-accent-evidence",
                    "provenance_groups":[]}
        # When a relevant octave exists, require independently qualified
        # evidence on BOTH candidates; an ACF alias is not a metrical vote.
        competitors=[c for c in relevant if c["accent_discriminator"]["available"]]
        if not competitors:
            return {"status":"ambiguous","chosen_bpm":None,"score_gap":None,
                    "reason":"no-comparable-octave-evidence","provenance_groups":[]}
        gap=winner["accent_discriminator"]["score"]-max(
            c["accent_discriminator"]["score"] for c in competitors)
        # Non-octave alternatives use their *joint* source rank, not only
        # their accent fit. Jittered ~85 and ~82 BPM can both fit strong
        # accents while only one has coherent observed IOI and phase.
        others=[c for c in ranked if c is not winner and
                not octave_competitors(c["bpm"],winner["bpm"])]
        # The non-octave tie-breaker is conditioned on actually observed ACF
        # corroboration. Without heavy data preserve conservative typed-accent
        # arbitration; with ACF, use joint rank to reject short-lag aliases.
        acf_observed=any(c["acf_support"]>0 for c in ranked)
        if acf_observed:
            others=[c for c in others if not acf_resolution_equivalent(
                winner["period_s"],c["period_s"],self.cfg.feature_rate_fps)]
        other_gap=(winner["score"]-max((c["score"] for c in others),default=0.)
                   if acf_observed else
                   winner["accent_discriminator"]["score"]-
                   scored[1]["accent_discriminator"]["score"])
        ok=(winner["accent_discriminator"]["score"]>=self.cfg.octave_accent_min
            and gap>=self.cfg.octave_accent_margin
            and other_gap>=(self.cfg.score_margin if acf_observed
                            else self.cfg.octave_accent_margin))
        return {
            "status":"resolved" if ok else "ambiguous",
            "chosen_bpm":winner["bpm"] if ok else None,
            "score_gap":round(min(gap,other_gap),6),
            "reason":"typed-accent-phase-and-coverage" if ok else "accent-contrast-insufficient",
            "provenance_groups":winner["accent_discriminator"]["provenance_groups"],
        }

    def step(self, source: FrameInput | dict[str,Any]) -> dict[str,Any]:
        f=FrameInput.parse(source) if isinstance(source,dict) else source
        require(isinstance(f,FrameInput), "step expects FrameInput/dict")
        # Ingest validation applies even for typed frames constructed manually.
        f=FrameInput.parse({
            **{k:v for k,v in asdict(f).items() if k!="event_candidates"},
            "features":list(f.features),
            "event_candidates":(
                None if f.event_candidates is None
                else [asdict(x) for x in f.event_candidates]),
        })
        issue: Optional[str]=None
        if f.saturated:
            preserved_seq=self.event_seq if self.epoch==f.source_epoch else 0
            self.reset()
            self.epoch=f.source_epoch
            self.event_seq=preserved_seq
            issue="saturated-input"
        elif self.epoch is None:
            self.epoch=f.source_epoch
        elif f.source_epoch!=self.epoch:
            self.reset()
            self.epoch=f.source_epoch
            issue="epoch-change"
        elif f.profile_id!=self.profile or f.sample_rate_hz!=self.sample_rate or len(f.features)!=len(self.features or ()):
            old_event_seq=self.event_seq
            self.reset()
            self.epoch=f.source_epoch
            self.event_seq=old_event_seq
            issue="profile-change"
        elif (self.seq is not None and
              (f.seq!=self.seq+1 or f.sample_index<=int(self.sample_index)
               or f.t_obs<=float(self.t_obs)
               or abs((f.sample_index-int(self.sample_index))/f.sample_rate_hz
                      -(f.t_obs-float(self.t_obs)))>2/f.sample_rate_hz)):
            old_event_seq=self.event_seq
            self.reset()
            self.epoch=f.source_epoch
            self.event_seq=old_event_seq
            issue="sequence-or-timestamp-gap"

        flux, contributions,onset_contrast=self._feature_flux(f)
        new_events=[] if f.saturated else self._ingest_events(
            f,flux,contributions,onset_contrast)
        change_event=self._causal_tempo_change(new_events)
        spectral_rate_change=self._causal_spectral_rate_change(new_events)
        self._causal_subdivision_innovation(new_events)
        if not f.saturated:
            # One change needs TWO accepted frames of one profile/epoch.
            if self.features is not None:
                self.flux_history.append(flux)
            self.feature_history.append(f.features)
            self.features=f.features
        self.profile=f.profile_id
        self.sample_rate=f.sample_rate_hz
        self.seq=f.seq
        self.sample_index=f.sample_index
        self.t_obs=f.t_obs
        times=list(self.event_history)
        acf_current={}
        if self.last_heavy_obs is not None and f.t_obs-self.last_heavy_obs<=self.cfg.max_age_s:
            acf_current=self.acf

        # A pre-refinement candidate ranking only controls the *gated*
        # scheduler. Fixed-light never refines and fixed-full always does
        # when eligible; for them the speculative _rank/_octave_decision is
        # dead computation. Avoid its repeated phase/IOI O(group*periods).
        if self.mode=="gated":
            before=self._rank(times,acf_current,f.t_obs)
            preliminary_octave=self._octave_decision(before,f.t_obs)
            ambiguous=(not before or before[0]["score"]<self.cfg.score_threshold
                       or (len(before)>1 and before[0]["score"]-before[1]["score"]<self.cfg.score_margin)
                       or preliminary_octave["status"]=="ambiguous")
        else:
            ambiguous=True
        possible_ops=self._potential_acf_ops()
        eligible=possible_ops>0 and not f.saturated
        gap_ok=(self.last_heavy_seq is None
                or f.seq-self.last_heavy_seq>=self.cfg.refinement_gap_frames)
        if self.mode=="fixed-light":
            run=False; reason="fixed-light-policy"
        elif self.mode=="fixed-full":
            run=eligible; reason="fixed-full-policy" if eligible else "insufficient-history"
        else:
            budget=self.cfg.default_budget_ops if f.budget_ops is None else f.budget_ops
            run=eligible and gap_ok and ambiguous and possible_ops<=budget
            reason=("gated-run" if run else
                    "insufficient-history" if not eligible else
                    "refinement-gap" if not gap_ok else
                    "not-ambiguous" if not ambiguous else "budget-exceeded")
        ops=0
        if run:
            vals=list(self.flux_history)[-self.cfg.acf_window:]
            self.acf,ops=acf_scores(vals,self.cfg.feature_rate_fps,
                                     self.cfg.min_bpm,self.cfg.max_bpm)
            self.last_heavy_seq=f.seq
            self.last_heavy_obs=f.t_obs
            acf_current=self.acf
        ranked=self._rank(times,acf_current,f.t_obs)
        octave=self._octave_decision(ranked,f.t_obs)
        relocked=self._confirmed_relock_candidate(ranked,f.t_obs)
        if relocked is not None:
            octave={"status":"resolved","chosen_bpm":relocked["bpm"],
                    "score_gap":None,
                    "reason":"post-change-primary-ioi-and-typed-phase",
                    "provenance_groups":relocked["accent_discriminator"]["provenance_groups"]}
        if octave["status"]=="resolved":
            ranked.sort(key=lambda c:(c["bpm"]!=octave["chosen_bpm"],
                                      -c["score"],c["bpm"]))
        if not ranked:
            ts=None
            if not times:
                why="no-accepted-events-or-periodicity"
            else:
                why="insufficient-period-evidence"
            tstat: Status="unknown"
        else:
            ts=ranked[0]["t_obs"]
            fresh=ts is not None and f.t_pub-ts<=self.cfg.max_age_s
            # Integer-lag ACF creates nearby numerical aliases. Distinct
            # BPM alternatives are those outside the quantified lag-error
            # cell. True octaves always remain distinct.
            alternatives=(
                [c for c in ranked[1:] if not acf_resolution_equivalent(
                    ranked[0]["period_s"],c["period_s"],self.cfg.feature_rate_fps)]
                if acf_current else ranked[1:])
            margin=(ranked[0]["score"]-max(c["score"] for c in alternatives)
                    if alternatives else 1.0)
            # An octave disagreement is never a "determined" BPM based only
            # on high generic IOI/ACF support. Distinct typed observations
            # MAY resolve it under a falsifiable salience/subdivision prior.
            octave_ok=(octave["status"]!="ambiguous")
            distinguishable=(octave["status"]=="resolved" or margin>=self.cfg.score_margin)
            tstat=("unknown" if not fresh else
                   "determined" if len(times)>=self.cfg.min_events
                   and ranked[0]["score"]>=self.cfg.score_threshold
                   and octave_ok and distinguishable
                   else "ambiguous")
            why=(None if tstat=="determined" else
                 "stale-evidence" if not fresh else
                 "octave-not-identifiable" if not octave_ok else
                 "competing-or-weak-periods")
        # Confidence in a period estimate must not contradict the most
        # recent, repeatedly observed incumbent strong-event cadence. A
        # suspected transition is allowed to abstain before three IOIs have
        # confirmed it; no future beats or ground-truth tempos are consulted.
        if tstat=="determined" and self.pending_tempo_conflict_since is not None:
            tstat="ambiguous"
            why="pending-primary-ioi-contradiction"
        if tstat=="determined" and self.pending_subdivision_conflict_since is not None:
            tstat="ambiguous"
            why="independent-subdivision-phase-innovation"
        if (tstat=="determined" and self.stable_strong_period is not None
                and self.tempo_anchor_group is not None
                and self.strong_times.get(self.tempo_anchor_group)
                and f.t_obs-self.strong_times[self.tempo_anchor_group][-1] <= self.cfg.max_age_s
                and abs(ranked[0]["period_s"]-self.stable_strong_period)
                    /self.stable_strong_period >self.cfg.change_separation_relative):
            tstat="ambiguous"
            why="primary-strong-ioi-contradicts-period"
        # Retain both episode-level tempo hypotheses over the relock
        # interval. The former estimate is historical information, not a
        # fresh accepted BPM or an independent vote for the new episode.
        episode_hypotheses=[]
        if (self.last_change is not None and
                0<=f.t_pub-self.last_change["t_confirm_s"]<=self.cfg.change_hold_s):
            ev=self.last_change
            episode_hypotheses=[
                {"role":"previous-episode","bpm":ev["from_bpm"],
                 "period_s":round(60/ev["from_bpm"],8),
                 "t_obs":ev["change_point_s"],"status":"ambiguous",
                 "score":None},
                {"role":"relocked-episode","bpm":ev["to_bpm"],
                 "period_s":round(60/ev["to_bpm"],8),
                 "t_obs":ev["t_confirm_s"],"status":tstat,
                 "score":None},
            ]
        tempo_value={
            "bpm":ranked[0]["bpm"] if tstat=="determined" else None,
            "period_s":ranked[0]["period_s"] if tstat=="determined" else None,
            "candidates":ranked,"counterfactual_multiples":["x0.5","x2"],
            "octave_resolution":octave,
            "episode_hypotheses":episode_hypotheses,
        }
        # The raw periodicity metric is retained in evidence below, but a
        # non-identifiable octave cannot advertise a high DECISION score.
        raw_support=ranked[0]["score"] if ranked else None
        decision_score=(min(raw_support,0.49)
                        if raw_support is not None and
                        (octave["status"]=="ambiguous" or why in (
                            "pending-primary-ioi-contradiction",
                            "primary-strong-ioi-contradicts-period",
                            "independent-subdivision-phase-innovation"))
                        else raw_support)
        if episode_hypotheses:
            episode_hypotheses[-1]["score"]=decision_score
        tempo=field(tempo_value,tstat,decision_score,
                    ts,f.t_ready if ts is not None else None,f.t_pub,why)

        if ranked:
            trial=ranked[0]; T=trial["period_s"]
            phase_model=trial["phase_candidates"]
            anchored=(tstat=="determined"
                      and octave["status"]=="resolved"
                      and phase_model["tactus_phase_s"] is not None)
            phase=phase_model["tactus_phase_s"] if anchored else None
            age=phase_model["tactus_t_obs"]
            if anchored and age is not None and f.t_pub-age<=self.cfg.max_age_s:
                grid_status: Status="determined"
                # Predict from a salient acoustic event grid, not a globally
                # most-coherent, possibly subdivision-only, phase group.
                n=max(0,math.ceil((f.t_pub-phase)/T-1e-10))
                next_tick=phase+n*T
                if next_tick<f.t_pub-1e-8:
                    next_tick+=T
                grid_value={"period_s":T,"phase_s":phase,
                            "next_beat_prediction_s":round(next_tick,8),
                            "anchor_role":"typed-salient-onset",
                            "phase_candidates":phase_model}
                grid_reason="salient-phase-anchor"
            else:
                grid_status="unknown" if tstat=="unknown" else "ambiguous"
                grid_value={"period_s":T,"phase_s":None,
                            "next_beat_prediction_s":None,
                            "anchor_role":None,
                            "phase_candidates":phase_model}
                grid_reason="no-identifiable-beat-phase-anchor"
            # An ambiguous grid may describe a candidate period even when no
            # salient phase is anchored. Such a value must trace to real
            # periodicity evidence, never to a fabricated timestamp.
            grid_source_time=age if age is not None else trial["t_obs"]
            if grid_source_time is None:
                grid=field(None,"unknown",None,None,None,f.t_pub,
                           "no-causal-grid-support")
            else:
                grid=field(grid_value,grid_status,phase_model["tactus_support"],
                           grid_source_time,f.t_ready,f.t_pub,grid_reason)
        else:
            grid=field(None,"unknown",None,None,None,f.t_pub,"no-tempo-phase")
        dynamics=field(None,"unknown",None,None,None,f.t_pub,"insufficient-tempo-transitions")
        if (self.last_change is not None
                and 0<=f.t_pub-self.last_change["t_confirm_s"]<=self.cfg.change_hold_s):
            transition=self.last_change
            elapsed=max(1e-9,transition["t_confirm_s"]-transition["change_point_s"])
            change_value={**transition,
                          "bpm_per_s":round((transition["to_bpm"]-transition["from_bpm"])/elapsed,6)}
            dynamics=field(change_value,"determined",max(0.,1.-transition["ioi_relative_spread"]),
                           transition["t_confirm_s"],f.t_ready,f.t_pub,
                           "conditional-strong-ioi-transition")
        # Only acoustic cadence is observed here. A single spectral-onset
        # provenance is NOT independent beat/harmonic evidence, so no
        # determined tempo, tactus grid or assured tempo dynamics is inferred.
        if (dynamics["status"]=="unknown" and self.last_spectral_rate_change is not None
                and 0<=f.t_pub-self.last_spectral_rate_change["t_confirm_s"]<=self.cfg.change_hold_s):
            detected=self.last_spectral_rate_change
            dynamics=field(detected,"ambiguous",min(.49,max(0.,1.-detected["ioi_relative_spread"])),
                           detected["t_confirm_s"],f.t_ready,f.t_pub,
                           "single-spectral-source-rate-change-not-tactus")
        if tstat=="determined" and ts is not None:
            if self.last_determined and ts>self.last_determined[1]+1e-9:
                prev_bpm,prev_t=self.last_determined
                accel=(ranked[0]["bpm"]-prev_bpm)/(ts-prev_t)
                kind=("acceleration" if accel>3 else
                      "deceleration" if accel<-3 else "stable")
                if dynamics["status"]!="determined":
                    dynamics=field({"bpm_per_s":round(accel,6),"kind":kind},
                                   "ambiguous",min(1.0,ranked[0]["score"]),
                                   ts,f.t_ready,f.t_pub,"uncalibrated-transition-rule")
            self.last_determined=(ranked[0]["bpm"],ts)
        events_field=field(new_events,
                           "ambiguous" if new_events and any(x["status"]=="ambiguous" for x in new_events)
                           else "determined" if new_events else "unknown",
                           max((x["score"] for x in new_events),default=None),
                           max((x["t_obs"] for x in new_events),default=None),
                           f.t_ready if new_events else None,f.t_pub,
                           None if new_events else "no-new-accent")
        evidence=field({
            "activity":round(flux,6),
            "onset_contrast":round(onset_contrast,6),
            "periodicity":max(acf_current.values(),default=None),
            "tempo_support":decision_score,
            "period_candidate_support_raw":raw_support,
            "octave_resolution":octave,
            "phase_support":ranked[0]["phase_support"] if ranked else None,
            "accent_support":max((x["score"] for x in new_events),default=None),
            "score_semantics":"heuristic-not-calibrated-probability",
            "provenance_groups":sorted(self.group_times),
        },"determined",None,f.t_obs,f.t_ready,f.t_pub)
        diag=field({
            "source_epoch":f.source_epoch,"frame_seq":f.seq,
            "sample_index":f.sample_index,"sample_rate_hz":f.sample_rate_hz,
            "profile_id":f.profile_id,"mode":self.mode,"history_frames":len(self.feature_history),
            "valid_positive_changes":len(self.flux_history),
            "heavy_executed":run,"heavy_reason":reason,
            "estimated_cost_units":ops,"possible_cost_units":possible_ops,
            "cost_is_not_cpu_time":True,
            "reset_reason":issue,"saturated":f.saturated,
            "new_event_count":len(new_events),
            "onset_contrast":round(onset_contrast,6),
            "evidence_group_overflow":self.evidence_group_overflow,
            "queue_transport":"external-component-required",
            "method_version":"B-R4.4-typed-phase-ioi-relock-v1",
            "last_acf_t_obs":self.last_heavy_obs,
            "tracking_start_s":self.tracking_start_s,
            "confirmed_strong_ioi_s":self.stable_strong_period,
            "tempo_anchor_group":self.tempo_anchor_group,
            "pending_tempo_conflict_since":self.pending_tempo_conflict_since,
            "pending_subdivision_conflict_since":self.pending_subdivision_conflict_since,
            "subdivision_recovery_count":self.subdivision_recovery_count,
            "last_tempo_change":self.last_change,
            "last_acoustic_rate_change":self.last_spectral_rate_change,
            "spectral_stable_onset_period_s":self.spectral_stable_period_s,
            "spectral_pending_intervals":len(self.spectral_pending_iois),
        },"determined",None,f.t_obs,f.t_ready,f.t_pub)
        return {
            "accent_events":events_field,
            "tempo":tempo,
            "rhythm_grid":grid,
            "tempo_dynamics":dynamics,
            "evidence":evidence,
            "diagnostics":diag,
        }


def main(argv: Optional[list[str]] = None) -> int:
    p=argparse.ArgumentParser(description="R4.3 PC rhythm reference, NDJSON frame input/output")
    p.add_argument("--mode",choices=MODES,default="gated")
    p.add_argument("--fps",type=int,choices=(30,45,60),default=60)
    p.add_argument("--budget-ops",type=int,default=None)
    args=p.parse_args(argv)
    method=RhythmReference(args.mode,MethodConfig(feature_rate_fps=args.fps))
    for index,line in enumerate(sys.stdin,1):
        if not line.strip():continue
        try:
            inp=json.loads(line)
            if args.budget_ops is not None and "budget_ops" not in inp:
                inp["budget_ops"]=args.budget_ops
            result=method.step(inp)
        except (ValueError,TypeError,KeyError) as exc:
            print(f"line {index}: {exc}",file=sys.stderr)
            return 2
        print(json.dumps(result,ensure_ascii=False,allow_nan=False,separators=(",",":")))
    return 0


if __name__=="__main__":
    raise SystemExit(main())
