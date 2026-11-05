"""Additional finite Essentia families against actual pinned native algorithms."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess
import time
import numpy as np
from native_compare import POLICY, compare, load, save_json, signal

PIN="b9fa6cb674ca43dfb94d28d293aeda441c6745db"
SEED=0x52485954
FIELDS={"ibi":["histogram","peaks"],"rubato":["starts","stops","count"],
        "harmonic":["harmonic"],"meter":["meter"],"beatogram":["beatogram","shape"],
        "band_novelty":["novelty"],"fixedbpm":["bpms","amplitudes"],
        "legacy":["bpm","ticks","estimates","intervals"],"superflux":["onsets"],
        "loop":["bpm"],"loop_confidence":["confidence"],"dfa":["score","dfa"],
        "loudness":["loudness","ratios","shape"],"onsetrate":["rate","onsets"]}
FIELDS["rhythm_transform"]=["rhythm","shape"]
for _method in ("summary_single","summary_multi","descriptors"):
    FIELDS[_method]=["bpm","confidence","ticks","estimates","intervals"]+(["histogram","peaks"] if _method=="descriptors" else [])
PARAMETERS={
    "ibi":{},"rubato":{"tolerance":.08,"longRegionsPruningTime":20.,"shortRegionsMergingTime":4.},
    "harmonic":{"bpm":60.,"threshold":20.,"tolerance":5.},"meter":{},"beatogram":{"size":16},
    "band_novelty":{"frameRate":44100/128,"weightCurveType":"hybrid","weightCurve":[],"normalize":False},
    "fixedbpm":{"sampleRate":44100.,"hopSize":512,"minBpm":30.,"maxBpm":560.,"tolerance":3.},
    "legacy":{"useOnset":True,"useBands":True,"hopSize":256,"frameSize":1024,"numberFrames":1024,"frameHop":1024,"sampleRate":44100.,"tolerance":.24,"tempoHints":[],"maxTempo":208,"minTempo":40,"lastBeatInterval":.1},
    "superflux":{"frameSize":2048,"hopSize":256,"sampleRate":44100.,"threshold":.05,"ratioThreshold":16.,"combine":20.},
    "loop":{"confidenceThreshold":.95},"loop_confidence":{"sampleRate":44100.},
    "dfa":{"minTau":310.,"maxTau":8800.,"tauMultiplier":1.1,"sampleRate":44100.},
    "loudness":{"sampleRate":44100.,"beatWindowDuration":.1,"beatDuration":.05,"frequencyBands":[20.,150.,400.,3200.,7000.,22000.]},
    "onsetrate":{"fixed_sample_rate":44100,"fixed_frame_size":1024,"fixed_hop":512,"centering":False,"window":"symmetric Hann normalized sum(abs)=2","zero_phase":True}}
PARAMETERS["rhythm_transform"]={"frameSize":256,"hopSize":32,"window":"symmetric normalized BlackmanHarris62","zero_phase":True,"padding":"every start < input frames including all tails"}
PARAMETERS["summary_single"]={"method":"degara","minTempo":40,"maxTempo":208,"C_RNG":"Windows C srand/rand, explicit seed"}
PARAMETERS["summary_multi"]={**PARAMETERS["summary_single"],"method":"multifeature"}
PARAMETERS["descriptors"]={"parameters":{},"nested_RhythmExtractor2013":PARAMETERS["summary_multi"]}


def cases():
    for kind in ("steady","transition"):
        intervals=np.full(128,.5,dtype="<f4")
        if kind=="transition":intervals[48:80]=np.linspace(.5,.7,32,dtype="<f4");intervals[80:]=.7
        yield "ibi",kind,intervals,None,128,1
        yield "rubato",kind,np.cumsum(intervals,dtype="float32"),None,128,1
    yield "harmonic","clusters",np.array([30,40,59.8,60,61,80,90,119.5,120,180,240,300],dtype="<f4"),None,12,1
    yield "harmonic","isolated",np.array([31,47,63,79,97,113,137,163],dtype="<f4"),None,8,1
    for kind in ("pattern","silence"):
        beats=np.arange(64);bands=np.arange(8)
        ratios=(.02+((beats[:,None]+bands[None,:])%4==0)*.6+(beats[:,None]%7==0)*.2).astype("<f4")
        energy=(.2+(beats%4==0)*.6+(beats%7==0)*.1).astype("<f4")
        if kind=="silence":ratios.fill(0);energy.fill(0)
        yield "beatogram",kind,ratios,energy,64,8
        yield "meter",kind,ratios.T.copy(),None,8,64
        t=np.arange(1024)[:,None];b=np.arange(8)[None,:]
        values=(.05+.04*np.sin(t*.1+b)**2+((t+b*7)%43<3)*(.2+b*.03)).astype("<f4")
        if kind=="silence":values.fill(0)
        yield "band_novelty",kind,values,None,1024,8
        yield "rhythm_transform",kind,values,None,1024,8
    t=np.arange(2048);novelty=np.full(2048,.005,dtype="<f4")
    for period,amplitude in ((43,1.),(57,.6),(71,.4),(96,.25)):
        novelty+=(amplitude*np.exp(-.5*((t%period)/1.5)**2)).astype("<f4")
    yield "fixedbpm","multiple_periods",novelty,None,2048,1
    for method in ("legacy","superflux","loop","loop_confidence","dfa","loudness","onsetrate","summary_single","summary_multi","descriptors"):
        for kind in (("tones8","transition") if method=="legacy" else ("tones8","silence")):
            values=signal("induction_"+kind,1024*512,44100)
            aux=np.array([120.],dtype="<f4") if method=="loop_confidence" else None
            if method=="loudness":aux=np.arange(.25,11.5,.5,dtype="<f4")
            yield method,kind,values,aux,len(values),1


def digest(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def immutable_input(path, values):
    data=values.astype("<f4").tobytes(order="C")
    if path.exists():
        if path.read_bytes()!=data:raise ValueError("immutable fixture differs: "+str(path))
    else:path.write_bytes(data)


def execute(binary,args,input_path,aux,record,env,timeout,settings):
    identity={"binary_sha256":digest(binary),"input_sha256":digest(input_path),
              "aux_sha256":digest(aux) if aux.exists() else None,
              "configuration":[args[0],*args[3:]],"settings":settings}
    key=hashlib.sha256(json.dumps(identity,sort_keys=True).encode()).hexdigest()
    # Narrow Windows original ofstream retains MAX_PATH: keep prefixes short.
    # The complete identity is still checked before reusing this short key.
    folder=record.parent/"r"/(record.stem[0]+"-"+key[:16])
    folder.mkdir(parents=True,exist_ok=True)
    run_record=folder/"run.json";prefix=folder/"output"
    if run_record.exists():
        old=json.loads(run_record.read_text(encoding="utf8"))
        if old["identity"]!=identity:raise ValueError("run identity collision")
        for name,expected in old["outputs"].items():
            if digest(folder/name)!=expected:raise ValueError("immutable run output differs: "+str(folder/name))
        return {**old["result"],"cached":True}
    if list(folder.iterdir()):raise ValueError("unfinished immutable run: "+str(folder))
    command=list(args);command[2]=str(prefix)
    run_env=env.copy();run_env["BARELIEF_ORIGINAL_OBSERVER_PREFIX"]=str(prefix)
    before=time.perf_counter()
    try:
        run=subprocess.run([binary,*command],capture_output=True,text=True,env=run_env,timeout=timeout)
        result={"returncode":run.returncode,"stdout":run.stdout,"stderr":run.stderr}
    except subprocess.TimeoutExpired as error:
        result={"returncode":-999,"stdout":str(error.stdout),"stderr":str(error.stderr),"timeout_seconds":timeout}
    result.update({"seconds":time.perf_counter()-before,"output_prefix":str(prefix),"record":str(run_record)})
    outputs={path.name:digest(path) for path in sorted(folder.glob("output.*"))}
    save_json(run_record,{"identity":identity,"arguments":command,"outputs":outputs,"result":result})
    return result


def one(spec,opt,env):
    method,kind,values,aux,rows,columns=spec;case_id=f"native.essentia.family.{method}.{kind}"
    folder=opt.output/case_id;folder.mkdir(parents=True,exist_ok=True)
    input_path=folder/"input.f32";immutable_input(input_path,values);aux_path=Path(str(input_path)+".aux.f32")
    if aux is not None:immutable_input(aux_path,aux)
    elif aux_path.exists():raise ValueError("unexpected immutable auxiliary input: "+str(aux_path))
    settings={"method":method,"Fs":44100,"rows":rows,"columns":columns,"seed":SEED,
              "parameters":PARAMETERS[method],"port_window_precision":"DesktopReference: Double-evaluate/Float32-store; Float32 normalization and FFT","parameter_policy":"pinned original declared defaults retained explicitly in this manifest"}
    if method in {"legacy","superflux","loop","dfa","loudness","onsetrate"}:settings["duration_seconds"]=len(values)/44100
    metadata={"format":"barelief-original-case-v1","case_id":case_id,
              "source":{"repository":"MTG/essentia","revision":PIN,"variant":opt.reference_variant,"wrapper_sha256":opt.wrapper_sha256,"backend":"original bundled KISS130 Float32; additional original OnsetRate/RhythmTransform/RhythmDescriptors TUs linked without edits"},
              "settings":settings,"comparison_policy":POLICY,
              "inputs":{"values":{"path":str(input_path),"dtype":"float32-le","shape":list(values.shape),"order":"C","sha256":digest(input_path)}}}
    if aux is not None:metadata["inputs"]["aux"]={"path":str(aux_path),"dtype":"float32-le","shape":list(aux.shape),"order":"C","sha256":digest(aux_path)}
    save_json(folder/"case.json",metadata)
    args=[method,str(input_path),str(folder/"original"),"44100",str(rows),str(columns),str(SEED)]
    original=execute(opt.original,args,input_path,aux_path,folder/"original_run.json",env,30 if method=="fixedbpm" else 180,settings)
    result={"case_id":case_id,"original_run":original,"comparisons":{},"outputs":{}}
    if opt.reference_only:result["status"]="reference_ready" if not original["returncode"] else "pending_reference_runtime"
    else:
        args[2]=str(folder/"port");port=execute(opt.port,args,input_path,aux_path,folder/"port_run.json",env,180,settings);result["port_run"]=port
        if original["returncode"] or port["returncode"]:result["status"]="pending_reference_runtime" if original["returncode"] else "fail_port_runtime"
        else:
            for field in FIELDS[method]:
                try:
                    reference,refmeta=load(Path(original["output_prefix"]),field);actual,portmeta=load(Path(port["output_prefix"]),field)
                    result["outputs"][field]={"original":refmeta,"port":portmeta};result["comparisons"][field]=compare(field,actual,reference,None)
                except (ValueError,OSError) as error:result["comparisons"][field]={"pass":False,"reason":str(error)}
            result["status"]="pass" if all(v["pass"] for v in result["comparisons"].values()) else "fail"
    save_json(folder/("reference_result.json" if opt.reference_only else "result.json"),result)
    print(case_id,result["status"],[(k,v.get("mismatches",v.get("reason"))) for k,v in result["comparisons"].items() if not v["pass"]],flush=True)
    return {"case_id":case_id,"status":result["status"],"path":str(folder)}


def main():
    parser=argparse.ArgumentParser();parser.add_argument("--original",required=True);parser.add_argument("--port")
    parser.add_argument("--output",type=Path,required=True);parser.add_argument("--runtime-bin",type=Path)
    parser.add_argument("--reference-variant",default="pinned-original",choices=["pinned-original","storage-corrected-global","storage-corrected-percival","storage-corrected-both"])
    parser.add_argument("--reference-wrapper",type=Path,help="Exact wrapper source used to build the reference binary")
    parser.add_argument("--reference-only",action="store_true");parser.add_argument("--methods",nargs="+",default=list(FIELDS))
    opt=parser.parse_args();env=os.environ.copy();opt.output.mkdir(parents=True,exist_ok=True)
    opt.wrapper_sha256=digest(opt.reference_wrapper) if opt.reference_wrapper else None
    if not opt.reference_only and not opt.port:parser.error("--port required")
    for key in ("OMP_NUM_THREADS","OPENBLAS_NUM_THREADS","MKL_NUM_THREADS"):env[key]="1"
    if opt.runtime_bin:env["PATH"]=str(opt.runtime_bin)+os.pathsep+env["PATH"]
    results=[one(spec,opt,env) for spec in cases() if spec[0] in opt.methods]
    counts={status:sum(row["status"]==status for row in results) for status in sorted({row["status"] for row in results})}
    save_json(opt.output/("reference_summary.json" if opt.reference_only else "summary.json"),{"cases":results,"counts":counts,"reference_variant":opt.reference_variant,"comparison_policy":POLICY,"coverage":"finite enumerated families/configurations; lifecycle/reset and other configurations remain pending"})
    print(json.dumps(counts));return int(any(row["status"] not in {"pass","reference_ready"} for row in results))


if __name__=="__main__":raise SystemExit(main())
