"""Compare pinned, separately compiled native libraries with the portable ports.

The original build lives outside the checkout. This script implements file I/O,
fixtures and comparisons only; it contains no reference algorithm formula.
"""
from __future__ import annotations
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess
import shutil
import time
import numpy as np

PINS = {
    "aubio": ("aubio/aubio", "ad5cf975aed08cc4562dd008cf9f83b12b82ffb8", "Ooura bundled, Float32"),
    "btrack": ("adamstark/BTrack", "9d6127618a5679e9caa74c594b88f1d74f0e035f", "KISS130 bundled Float32 FFT, double ODF; libsamplerate 0.2.2 c96f5e3d"),
    "essentia": ("MTG/essentia", "b9fa6cb674ca43dfb94d28d293aeda441c6745db", "KISS130 bundled, Float32"),
}
# Declared before executing any comparison. Spectral bounds are the existing
# independent FFT acceptance envelope. Descriptor arithmetic adds Float32
# reductions and state accumulation; integer events/status remain exact.
POLICY = {
    "spectral": {"rtol": 8e-5, "peak_atol": 4e-6},
    "descriptor": {"rtol": 3e-4, "peak_atol": 8e-6},
    "phase": {"circular_atol": .001, "minimum_relative_magnitude": .001},
    "integer": "exact dtype, shape and values; no floating point conversion",
    "nonfinite": "exact NaN/+Inf/-Inf classification at each element",
}

def save_json(path: Path, value: object) -> None:
    path.write_text(json.dumps(value, indent=2, allow_nan=False), encoding="utf8")

def signal(name: str, length: int, rate: int) -> np.ndarray:
    x = np.zeros(length, dtype="<f4")
    if name == "constant":
        x.fill(.25)
    elif name == "nyquist":
        x[::2] = .25
        x[1::2] = -.25
    elif name.startswith("impulse"):
        x[{"impulse_first": 0, "impulse_middle": length//2, "impulse_last": length-1}[name]] = .75
    elif name == "noise":
        x[:] = np.random.Generator(np.random.PCG64(0x52485954)).uniform(-.2, .2, length)
    elif name in {"tones8", "induction_tones8", "induction_transition"}:
        t = np.arange(length, dtype=np.float64)/rate
        for i, frequency in enumerate((97, 173, 293, 419, 701, 997, 1901, 3109)):
            x += (.045*np.sin(2*np.pi*(frequency*t+(i+1)*.7*t*t))).astype("<f4")
        x[::max(1, rate//2)] += .3
        if name=="induction_transition":
            x[:rate]=0
            x[5*rate:6*rate]=0
            x[6*rate:]=0
            for at in np.arange(6*rate,length,rate*2/3).astype(np.int64):
                stop=min(length,at+rate//25)
                time=np.arange(stop-at)/rate
                x[at:stop]=sum(.055*np.sin(2*np.pi*f*time) for f in (97,701,997,1901,3109)).astype("<f4")
    elif name not in {"silence","induction_silence"}:
        raise ValueError(name)
    return x

def specs(matrix: str):
    names = ["silence", "constant", "nyquist", "impulse_first", "impulse_middle", "impulse_last", "noise", "tones8"]
    if matrix == "smoke":
        names = ["tones8"]
    sizes = [1024] if matrix == "smoke" else [64, 256, 512, 1024, 4096]
    for library in PINS:
        if matrix=="induction":
            for name in ("induction_tones8","induction_transition","induction_silence"):
                yield library,1024,512,44100,1 if library=="btrack" else 0,name,1024
            continue
        rates = [44100] if matrix != "full" or library == "btrack" else [32000, 44100]
        windows = range(5 if library == "btrack" else 9) if matrix == "full" and library != "aubio" else [1 if library == "btrack" else 0]
        for rate in rates:
            for n in sizes:
                for window in windows:
                    for name in names:
                        yield library, n, n//2, rate, window, name, 128
        if matrix == "full":
            for name in ("induction_tones8","induction_transition","induction_silence"):
                yield library,1024,512,44100,1 if library=="btrack" else 0,name,1024

def output_shapes(library: str, n: int, hop: int, samples: int):
    frames = samples//hop
    if library == "aubio":
        return {"window": [n], "norm": [frames,n//2+1], "phase": [frames,n//2+1],
                "odf": [frames,9], "onsets": [frames,9,3], "onset_last": [frames,9],
                "tempo": [frames,5], "tempo_last": [frames], "tatum": [frames], "onset_defaults": [9,6]}
    if library == "btrack":
        return {"window": [n], "norm": [frames,n], "phase": [frames,n], "odf": [frames,10],
                "state": [frames,3], "beat": [frames], "resampled": [frames,512],
                "comb": [frames,128], "delta": [frames,41]}
    frames = 1+(samples-n)//hop
    return {"window": [n], "norm": [frames,n//2+1], "phase": [frames,n//2+1],
            "complex": [frames,n//2+1,2], "transform": [frames,n], "odf": [frames,6],"odf_status":[frames,6]}

def load(prefix: Path, field: str, shape: list[int] | None = None):
    found = [prefix.with_name(prefix.name+"."+field+"."+kind) for kind in ("f32","f64","u64")]
    found = [path for path in found if path.exists()]
    if len(found) != 1:
        raise ValueError(f"expected one output for {prefix.name}.{field}: {found}")
    path = found[0]
    dtype = {".f32":"<f4", ".f64":"<f8", ".u64":"<u8"}[path.suffix]
    value = np.fromfile(path, dtype=dtype)
    if shape is not None:
        value = value.reshape(shape)
    return value, {"path":str(path),"dtype":{"<f4":"float32-le","<f8":"float64-le","<u8":"uint64-le"}[dtype],"shape":list(value.shape),"order":"C","sha256":hashlib.sha256(path.read_bytes()).hexdigest()}

def compare(field: str, actual: np.ndarray, expected: np.ndarray, norm: np.ndarray | None):
    if actual.shape != expected.shape:
        return {"pass":False,"reason":"shape", "actual":list(actual.shape),"expected":list(expected.shape)}
    if actual.dtype.kind == "u" or expected.dtype.kind == "u":
        bad = actual != expected
        return {"pass":bool(not np.any(bad)),"mismatches":int(np.count_nonzero(bad)),"policy":"integer"}
    classes = lambda a: np.where(np.isnan(a),1,np.where(np.isposinf(a),2,np.where(np.isneginf(a),3,0)))
    nonfinite_bad = classes(actual) != classes(expected)
    finite = np.isfinite(actual) & np.isfinite(expected)
    error = np.zeros(actual.shape, dtype=np.float64)
    error[finite] = np.abs(actual[finite].astype(np.float64)-expected[finite])
    if field == "phase":
        assert norm is not None
        finite &= norm > np.max(norm, axis=-1, keepdims=True)*POLICY["phase"]["minimum_relative_magnitude"]
        error[finite] = np.abs((actual[finite]-expected[finite]+np.pi)%(2*np.pi)-np.pi)
        bad = finite & (error > POLICY["phase"]["circular_atol"])
        policy = "phase"
    else:
        policy = "spectral" if field in {"norm","window","complex","transform"} else "descriptor"
        p = POLICY[policy]
        # Per-method scale: HFC cannot hide error in another ODF column.
        axes = (-1,-2) if field=="complex" else -1 if policy=="spectral" else 0
        peak = np.max(np.where(np.isfinite(expected),np.abs(expected),0),axis=axes,keepdims=True,initial=0)
        allowed = p["rtol"]*np.abs(expected)+p["peak_atol"]*peak
        bad = finite & (error > allowed)
    bad |= nonfinite_bad
    indices = np.argwhere(bad)
    return {"pass":bool(not len(indices)),"policy":policy,"mismatches":len(indices),
            "nonfinite_class_mismatches":int(np.count_nonzero(nonfinite_bad)),
            "max_abs_error":float(np.max(error[finite],initial=0)),
            "first_index":indices[0].tolist() if len(indices) else None,
            "compared_finite_values":int(np.count_nonzero(finite))}

def execute(binary: str, args: list[str], env: dict[str,str]):
    before = time.perf_counter()
    result = subprocess.run([binary,*args], capture_output=True, text=True, timeout=180, env=env)
    return {"returncode":result.returncode,"seconds":time.perf_counter()-before,
            "stdout":result.stdout,"stderr":result.stderr}

def execute_cached(binary: str, args: list[str], env: dict[str,str], cache: Path):
    identity={"binary_sha256":hashlib.sha256(Path(binary).read_bytes()).hexdigest(),"arguments":args,
              "input_sha256":hashlib.sha256(Path(args[4]).read_bytes()).hexdigest(),
              "configuration":[*args[:4],*args[6:]]}
    if cache.exists():
        previous=json.loads(cache.read_text(encoding="utf8"))
        old=previous.get("identity",{})
        old_arguments=old.get("arguments",[])
        old_configuration=old.get("configuration",[*old_arguments[:4],*old_arguments[6:]])
        fields=output_shapes(args[0],int(args[1]),int(args[2]),Path(args[4]).stat().st_size//4)
        if args[0]=="essentia":fields.update({"global_infogain":None,"global_beat_emphasis":None})
        complete=all(sum(Path(args[5]+"."+name+suffix).exists() for suffix in (".f32",".f64",".u64"))==1 for name in fields)
        if old.get("binary_sha256")==identity["binary_sha256"] and old.get("input_sha256")==identity["input_sha256"] and old_configuration==identity["configuration"] and complete and previous["run"]["returncode"]==0:
            return {**previous["run"],"cached":True}
        archive=cache.parent/(cache.stem+"-"+old["binary_sha256"][:12]);archive.mkdir(exist_ok=True)
        shutil.copy2(cache,archive/cache.name)
        for output in cache.parent.glob(Path(args[5]).name+".*"):shutil.copy2(output,archive/output.name)
    result=execute(binary,args,env)
    save_json(cache,{"identity":identity,"run":result})
    return result

def source_observations(library: str, prefix: Path, frames: int):
    if library=="aubio":
        onset=load(prefix,"onsets",[frames,9,3])[0]
        tempo=load(prefix,"tempo",[frames,5])[0]
        return {"onset_events_per_method":np.count_nonzero(onset[...,2]>0,axis=0).tolist(),
                "beat_event_frames":np.flatnonzero(tempo[:,0]>0).tolist(),
                "frames_with_tempo_period":int(np.count_nonzero(tempo[:,3]>0)),
                "final_bpm":float(tempo[-1,1]) if np.isfinite(tempo[-1,1]) else None,
                "complete_pcm_hops":frames}
    if library=="btrack":
        state=load(prefix,"state",[frames,3])[0]
        return {"beat_event_frames":np.flatnonzero(load(prefix,"beat",[frames])[0]).tolist(),
                "final_bpm":float(state[-1,0]) if np.isfinite(state[-1,0]) else None,
                "final_period_frames":float(state[-1,2]),"complete_pcm_hops":frames}
    return {"complete_pcm_hops":frames,"global_context_is_independent_from_per_frame_odf":True}

def run_case(spec, options, env):
    library,n,hop,rate,window,name,hops = spec
    case_id=f"native.{library}.{name}.n{n}.h{hop}.fs{rate}.w{window}"
    folder=options.output/case_id
    folder.mkdir(parents=True,exist_ok=True)
    pcm=signal(name,hop*hops,rate)
    pcm_path=folder/"input.f32"
    pcm.tofile(pcm_path)
    repository,revision,backend=PINS[library]
    window_name={"aubio":["periodic_hanningz"],
                 "btrack":["rectangular","symmetric_hann","symmetric_hamming","symmetric_blackman","source_shifted_tukey"],
                 "essentia":["hann","hamming","hannnsgcq","triangular","square","blackmanharris62","blackmanharris70","blackmanharris74","blackmanharris92"]}[library][window]
    case={"format":"barelief-original-case-v1","case_id":case_id,
          "source":{"repository":repository,"revision":revision,"backend":backend,"algorithm_bodies":"unchanged"},
          "settings":{"Fs":rate,"N":n,"M":n,"hop":hop,"window_choice":window,"window":window_name,
                      "centering":False,"padding":"aubio/BTrack zero startup; Essentia complete-frame ODF plus source global EOF",
                      "sidedness":"BTrack full; aubio/Essentia positive including endpoints","normalization":"raw native recipe",
                      "zero_phase":True,"first_frame_sample":-(n-hop) if library!="essentia" else 0,
                      "window_normalization":"sum(abs(w))=2" if library=="essentia" else "none",
                      "case_role":"source_induction_and_transitions" if name.startswith("induction_") else "finite_frontend_and_startup",
                      "pcm_seconds":len(pcm)/rate,"complete_hops":hops,
                      "seed":0x52485954,"btrack_tracker_window":"source fixed Hann"},
          "inputs":{"pcm":{"path":str(pcm_path),"dtype":"float32-le","shape":[len(pcm)],"order":"C","sha256":hashlib.sha256(pcm.tobytes()).hexdigest()}},
          "comparison_policy":POLICY}
    save_json(folder/"case.json",case)
    original=folder/("original_src_capture" if library=="btrack" and options.btrack_original else "original")
    port=folder/"port"
    args=[library,str(n),str(hop),str(rate),str(pcm_path)]
    reference_binary=options.essentia if library=="essentia" else options.btrack_original if library=="btrack" and options.btrack_original else options.original
    if library=="essentia" and n<=512 and options.essentia_small_original:
        reference_binary=options.essentia_small_original
    original_run=execute_cached(reference_binary,[*args,str(original),str(window)],env,folder/(original.name+"_run.json"))
    observed=source_observations(library,original,hops) if not original_run["returncode"] else {}
    if options.reference_only:
        result={"case_id":case_id,"status":"reference_ready" if not original_run["returncode"] else "pending_reference_runtime","original_run":original_run,"source_observations":observed}
        save_json(folder/"reference_result.json",result)
        print(case_id,result["status"],flush=True)
        return {"case_id":case_id,"status":result["status"],"result":str(folder/"reference_result.json")}
    port_run=execute_cached(options.port,[*args,str(port),str(window)],env,folder/"port_run.json")
    result={"case_id":case_id,"original_run":original_run,"port_run":port_run,"comparisons":{},"outputs":{},"source_observations":observed}
    if original_run["returncode"] or port_run["returncode"]:
        result["status"]="pending_reference_runtime" if original_run["returncode"] else "fail_port_runtime"
    else:
        fields=output_shapes(library,n,hop,len(pcm))
        if library=="essentia":
            fields.update({"global_infogain":None,"global_beat_emphasis":None})
        norm=load(original,"norm",fields["norm"])[0]
        for field,shape in fields.items():
            try:
                expected,refmeta=load(original,field,shape)
                actual,portmeta=load(port,field,shape)
                result["outputs"][field]={"original":refmeta,"port":portmeta}
                if library=="aubio" and field=="onset_last":
                    decisions=load(original,"onsets",fields["onsets"])[0][...,2]
                    has_event=np.cumsum(decisions>0,axis=0)>0
                    result["comparisons"][field]=compare(field,actual[has_event],expected[has_event],None)
                    underflow=(~has_event)&(expected>np.iinfo(np.uint32).max-n*10)
                    result["valid_event_timestamps"]={str(i):{
                        "source_has_event_rows":int(np.count_nonzero(has_event[:,i])),
                        "comparison":compare(field,actual[has_event[:,i],i],expected[has_event[:,i],i],None),
                        "invalid_pre_event_wrapped_rows":int(np.count_nonzero(underflow[:,i]))}
                        for i in range(has_event.shape[1])}
                    ordinary_pre_event=(~has_event)&(~underflow)
                    result["comparisons"]["pre_event_nonwrapped"]=compare(field,actual[ordinary_pre_event],expected[ordinary_pre_event],None)
                    if np.any(underflow):
                        result.setdefault("source_extensions",{})[field]={
                            "kind":"source uint32 subtraction underflow before first detected event; port bounded timestamp clamps to zero",
                            "source_has_event":False,"elements":int(np.count_nonzero(underflow)),
                            "source_values":np.unique(expected[underflow]).tolist(),"port_values":np.unique(actual[underflow]).tolist(),
                            "valid_event_timestamp_comparison":"exact UInt64 values"}
                        result["comparisons"]["pre_event_port_contract"]={
                            "pass":bool(np.all(actual[underflow]==0)),"policy":"bounded pre-event timestamp remains zero",
                            "mismatches":int(np.count_nonzero(actual[underflow]))}
                else:
                    result["comparisons"][field]=compare(field,actual,expected,norm)
                if field=="odf":
                    names={"aubio":("energy","hfc","complex","phase","weighted_phase","spectral_difference","kl","modified_kl","spectral_flux"),
                           "btrack":("energy","energy_difference","spectral_difference","spectral_difference_hwr","phase_deviation","complex_difference","complex_difference_hwr","hfc","high_frequency_difference","high_frequency_difference_hwr"),
                           "essentia":("hfc","complex","complex_phase","flux","melflux","rms")}[library]
                    result["odf_methods"]={name:compare(field,actual[:,i],expected[:,i],None) for i,name in enumerate(names)}
            except (ValueError,OSError) as error:
                result["comparisons"][field]={"pass":False,"reason":str(error)}
        result["status"]="pass" if all(v["pass"] for v in result["comparisons"].values()) else "fail"
        if library=="essentia":
            statuses=load(original,"odf_status",fields["odf_status"])[0]
            result["method_availability"]={name:{"status":"source_geometry_rejection" if np.any(statuses[:,i]) else "available",
                "numeric_comparison":"unavailable" if np.any(statuses[:,i]) else "included"} for i,name in enumerate(("hfc","complex","complex_phase","flux","melflux","rms"))}
            for name,availability in result["method_availability"].items():
                if availability["status"]=="source_geometry_rejection":
                    result["odf_methods"][name]={"status":"source_geometry_rejection","numeric_comparison":"unavailable","status_comparison":result["comparisons"]["odf_status"]}
            errors=original.with_name(original.name+".odf_errors.txt")
            result["source_rejection_messages"]=errors.read_text(encoding="utf8") if errors.exists() else ""
            if result["status"]=="pass" and np.any(statuses):result["status"]="source_rejection"
        if result["status"]=="pass" and result.get("source_extensions"):
            result["status"]="source_extension"
    save_json(folder/"result.json",result)
    print(case_id,result["status"],[(k,v.get("mismatches",v.get("reason"))) for k,v in result["comparisons"].items() if not v["pass"]],flush=True)
    return {"case_id":case_id,"status":result["status"],"result":str(folder/"result.json")}

def main():
    parser=argparse.ArgumentParser()
    parser.add_argument("--port")
    parser.add_argument("--original",required=True)
    parser.add_argument("--btrack-original",help="Observation bridge capturing raw src_simple output before the original in-place threshold")
    parser.add_argument("--essentia",required=True)
    parser.add_argument("--essentia-small-original",help="Quarantining source wrapper for N<=512; preserves larger-geometry immutable original cache identity")
    parser.add_argument("--output",type=Path,required=True)
    parser.add_argument("--matrix",choices=("smoke","finite","induction","full"),default="smoke")
    parser.add_argument("--libraries",nargs="+",default=list(PINS))
    parser.add_argument("--reference-only",action="store_true")
    parser.add_argument("--only-failed",action="store_true",help="Retain successful case evidence; execute only previously failed or aborted cases")
    parser.add_argument("--runtime-bin",type=Path,help="Optional compiler runtime DLL directory for source-built Windows references")
    options=parser.parse_args()
    if not options.reference_only and not options.port:
        parser.error("--port is required for comparisons")
    options.output.mkdir(parents=True,exist_ok=True)
    env=os.environ.copy()
    if options.runtime_bin:env["PATH"]=str(options.runtime_bin)+os.pathsep+env["PATH"]
    selected=[spec for spec in specs(options.matrix) if spec[0] in options.libraries]
    if options.only_failed:
        retained=[]
        for spec in selected:
            library,n,hop,rate,window,name,_=spec
            previous=options.output/f"native.{library}.{name}.n{n}.h{hop}.fs{rate}.w{window}"/"result.json"
            if previous.exists() and json.loads(previous.read_text(encoding="utf8"))["status"] in {"fail","fail_port_runtime","pending_reference_runtime"}:
                retained.append(spec)
        selected=retained
    results=[run_case(spec,options,env) for spec in selected]
    summary={"format":"barelief-original-results-v1","matrix":options.matrix,"selection":"only_previous_failures" if options.only_failed else "all_selected_cases","cases":results,
             "counts":{status:sum(r["status"]==status for r in results) for status in sorted({r["status"] for r in results})},
             "coverage":"finite enumerated cases only; unlisted families/control/reset scenarios remain pending"}
    save_json(options.output/("reference_summary.json" if options.reference_only else "summary.json"),summary)
    print(json.dumps(summary["counts"]),flush=True)
    return int(any(r["status"]!=("reference_ready" if options.reference_only else "pass") for r in results))

if __name__=="__main__":
    raise SystemExit(main())
