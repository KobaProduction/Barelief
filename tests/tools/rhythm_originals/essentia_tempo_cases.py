"""Finite native Essentia tempo families; all references execute the pinned library."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess
import shutil
import time
import numpy as np
from native_compare import POLICY,compare,load,save_json,signal

SEED=0x52485954
PIN="b9fa6cb674ca43dfb94d28d293aeda441c6745db"

def envelope(kind):
    x=np.zeros(1024,dtype="<f4")
    if kind=="silence":return x
    x.fill(.005)
    positions=list(range(12,1024,43)) if kind=="pulses" else list(range(12,512,43))+list(range(524,1024,57))
    index=np.arange(len(x))
    for at in positions:x+=np.exp(-.5*((index-at)/1.5)**2).astype("<f4")
    return x

def cases():
    for method in ("period","novelty"):
        for kind in ("pulses","transition","silence"):
            yield method,kind,envelope(kind),44100,1024,512
    for method in ("single","multi","percival"):
        for kind in ("tones8","transition","silence"):
            name={"tones8":"induction_tones8","transition":"induction_transition","silence":"induction_silence"}[kind]
            yield method,kind,signal(name,1024*512,44100),44100,1024,128 if method=="percival" else 1024

def fields(method):
    if method=="period":return ["ticks","periods","ends","normalized"]
    if method in {"single","multi"}:
        return ["ticks"]+(["confidence"] if method=="multi" else [])+[f"{name}{i}" for i in range(5 if method=="multi" else 1) for name in ("curve","candidate")]
    if method=="percival":return ["bpm","last_magnitude","last_compressed","last_correlation","last_enhanced","lag_observations"]
    return ["bpm","candidates","magnitudes","frame_bpms","ticks","tick_magnitudes","sinusoid","tempogram","tempogram_shape"]

def cached(binary,args,input_path,cache,env):
    identity={"binary_sha256":hashlib.sha256(Path(binary).read_bytes()).hexdigest(),"input_sha256":hashlib.sha256(input_path.read_bytes()).hexdigest(),"configuration":[args[0],*args[3:]]}
    if cache.exists():
        old=json.loads(cache.read_text(encoding="utf8"))
        previous=old["identity"]
        previous_config=previous.get("configuration") or [previous["arguments"][0],*previous["arguments"][3:]]
        equivalent=previous.get("binary_sha256")==identity["binary_sha256"] and previous.get("input_sha256")==identity["input_sha256"] and previous_config==identity["configuration"]
        complete=all(any(Path(args[2]+"."+name+suffix).exists() for suffix in (".f32",".f64",".u64")) for name in fields(args[0]))
        if equivalent and complete and old["result"]["returncode"]==0:return {**old["result"],"cached":True}
        archive=cache.parent/(cache.stem+"-"+previous["binary_sha256"][:12])
        archive.mkdir(exist_ok=True)
        shutil.copy2(cache,archive/cache.name)
        for output in cache.parent.glob(Path(args[2]).name+".*"):shutil.copy2(output,archive/output.name)
    before=time.perf_counter()
    run=subprocess.run([binary,*args],text=True,capture_output=True,env=env,timeout=180)
    result={"returncode":run.returncode,"stdout":run.stdout,"stderr":run.stderr,"seconds":time.perf_counter()-before}
    save_json(cache,{"identity":identity,"result":result})
    return result

def one(spec,opt,env):
    method,kind,values,rate,n,hop=spec
    case_id=f"native.essentia.{method}.{kind}"
    folder=opt.output/case_id;folder.mkdir(parents=True,exist_ok=True)
    input_path=folder/"input.f32";values.astype("<f4").tofile(input_path)
    settings={"Fs":rate,"M":n,"N":n,"hop":hop,"seed":SEED,"C_RNG":"Windows C runtime srand/rand, explicit seed, source bodies unchanged",
              "frame_rate":rate/hop,"method":method,"minimum_bpm":50 if method=="percival" else 40,"maximum_bpm":210 if method=="percival" else 208}
    if method in {"single","multi"}:settings.update({"M":2048,"N":2048,"frame_size":2048,"hop":1024,"primary_upsampling":2,"global_frame_size":2048,"global_hop":512,"window":"symmetric Hann normalized sum(abs)=2","zero_phase":True})
    if method=="period":settings.update({"N":None,"M":None,"input_kind":"ODF","upsampling":1,"normalization":"maximum, source essentia::normalize","frame_duration_s":5.944308390022676,"hop_duration_s":5.944308390022676/4,"ODF_frame_samples":512,"ODF_hop_samples":128})
    if method=="percival":settings.update({"window":"symmetric unnormalized Hamming","zero_phase":False,"frame_size_oss":2048,"hop_size_oss":128,"minBPM":50,"maxBPM":210})
    if method=="novelty":settings.update({"N":512,"M":512,"input_kind":"novelty","frame_seconds":4,"zero_padding_factor":0,"overlap":16,"window":"symmetric normalized Hann","minBPM":30,"maxBPM":560,"maxPeaks":50,"weightByMagnitude":True,"constantTempo":False,"tempoChange":5,"inducedBPM":0,"centering":True,"zero_phase":True})
    metadata={"format":"barelief-original-case-v1","case_id":case_id,"source":{"repository":"MTG/essentia","revision":PIN,"backend":"bundled KISS130 Float32, Eigen3.4 headers","route":"actual source-built original factory and algorithms; observation-only PoolStorage taps"},
              "settings":settings,"inputs":{"values":{"path":str(input_path),"dtype":"float32-le","shape":[len(values)],"order":"C","sha256":hashlib.sha256(input_path.read_bytes()).hexdigest()}},"comparison_policy":POLICY,
              "coverage":"finite listed family/configuration only; other Essentia families remain pending"}
    save_json(folder/"case.json",metadata)
    base=[method,str(input_path)]
    original=cached(opt.original,[*base,str(folder/"original"),str(rate),str(n),str(hop),str(SEED)],input_path,folder/"original_run.json",env)
    result={"case_id":case_id,"original_run":original,"comparisons":{},"outputs":{}}
    if opt.reference_only:
        result["status"]="reference_ready" if not original["returncode"] else "pending_reference_runtime"
    else:
        port=cached(opt.port,[*base,str(folder/"port"),str(rate),str(n),str(hop),str(SEED)],input_path,folder/"port_run.json",env)
        result["port_run"]=port
        if original["returncode"] or port["returncode"]:
            result["status"]="pending_reference_runtime" if original["returncode"] else "fail_port_runtime"
        else:
            for field in fields(method):
                try:
                    reference,refmeta=load(folder/"original",field);actual,portmeta=load(folder/"port",field)
                    result["outputs"][field]={"original":refmeta,"port":portmeta}
                    result["comparisons"][field]=compare(field,actual,reference,None)
                    if field.startswith("ticks") and method in {"period","single","multi"}:
                        resolution=512/44100
                        result["comparisons"]["tick_grid_indices"]=compare("indices",np.rint(actual/resolution).astype("<u8"),np.rint(reference/resolution).astype("<u8"),None)
                except (ValueError,OSError) as error:result["comparisons"][field]={"pass":False,"reason":str(error)}
            result["status"]="pass" if all(v["pass"] for v in result["comparisons"].values()) else "fail"
            if method=="percival":
                result["port_diagnostics"]={}
                for field in ("uncorrected_bpm","octave_multiplier","selected_lag"):
                    try:
                        value,meta=load(folder/"port",field)
                        result["port_diagnostics"][field]={"values":value.tolist(),**meta,"reference_comparison":"pending; diagnostic only"}
                    except ValueError:pass
    save_json(folder/("reference_result.json" if opt.reference_only else "result.json"),result)
    print(case_id,result["status"],[(k,v.get("mismatches",v.get("reason"))) for k,v in result["comparisons"].items() if not v["pass"]],flush=True)
    return {"case_id":case_id,"status":result["status"],"path":str(folder)}

def main():
    parser=argparse.ArgumentParser()
    parser.add_argument("--original",required=True);parser.add_argument("--port");parser.add_argument("--output",type=Path,required=True)
    parser.add_argument("--reference-only",action="store_true");parser.add_argument("--runtime-bin",type=Path)
    parser.add_argument("--methods",nargs="+",default=["period","single","multi","percival","novelty"])
    parser.add_argument("--kinds",nargs="+",help="Select only affected fixture kinds, preserving successful checks")
    opt=parser.parse_args()
    if not opt.reference_only and not opt.port:parser.error("--port required for comparisons")
    opt.output.mkdir(parents=True,exist_ok=True);env=os.environ.copy()
    for key in ("OMP_NUM_THREADS","OPENBLAS_NUM_THREADS","MKL_NUM_THREADS"):env[key]="1"
    if opt.runtime_bin:env["PATH"]=str(opt.runtime_bin)+os.pathsep+env["PATH"]
    results=[one(case,opt,env) for case in cases() if case[0] in opt.methods and (not opt.kinds or case[1] in opt.kinds)]
    counts={status:sum(r["status"]==status for r in results) for status in sorted({r["status"] for r in results})}
    save_json(opt.output/("reference_summary.json" if opt.reference_only else "summary.json"),{"cases":results,"counts":counts})
    print(json.dumps(counts));return int(any(v["status"]!=("reference_ready" if opt.reference_only else "pass") for v in results))

if __name__=="__main__":raise SystemExit(main())
