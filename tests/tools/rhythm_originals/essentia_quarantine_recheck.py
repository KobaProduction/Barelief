"""Recapture only rejected original methods; reuse verified, unchanged port bytes."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import numpy as np
from native_compare import execute,load,compare,output_shapes,POLICY,save_json

def digest(path):return hashlib.sha256(Path(path).read_bytes()).hexdigest()

def one(previous,opt,env,port_hash):
    metadata=json.loads((previous/"case.json").read_text(encoding="utf8"))
    settings=metadata["settings"];n,hop,rate=settings["N"],settings["hop"],settings["Fs"]
    cached=json.loads((previous/"port_run.json").read_text(encoding="utf8"))
    if cached["identity"]["binary_sha256"]!=port_hash:raise ValueError("port binary changed: "+metadata["case_id"])
    input_path=previous/"input.f32"
    if digest(input_path)!=metadata["inputs"]["pcm"]["sha256"]:raise ValueError("input bytes changed")
    folder=opt.output/previous.name;folder.mkdir(parents=True,exist_ok=True)
    shutil.copy2(input_path,folder/input_path.name)
    for source in previous.glob("port.*"):shutil.copy2(source,folder/source.name)
    shutil.copy2(previous/"port_run.json",folder/"reused_port_run.json")
    metadata["inputs"]["pcm"]["path"]=str(folder/"input.f32")
    metadata["source"]["wrapper_policy"]="After the first insufficient-triangular-bins exception, quarantine this method for the unchanged geometry; later numeric values are unavailable. Original algorithm bodies unchanged."
    metadata["reference_replaces"]={"case":str(previous),"reason":"previous wrapper reused partial filter state after original exception"}
    save_json(folder/"case.json",metadata)
    args=["essentia",str(n),str(hop),str(rate),str(folder/"input.f32"),str(folder/"original_quarantined"),str(settings["window_choice"])]
    run=execute(opt.original,args,env)
    result={"case_id":metadata["case_id"],"original_run":run,"original_binary_sha256":digest(opt.original),
            "port_run":{"cached":True,"binary_sha256":port_hash,"reused_from":str(previous/"port_run.json")},
            "comparisons":{},"outputs":{},"policy":POLICY}
    if run["returncode"]:
        result["status"]="pending_reference_runtime"
    else:
        samples=metadata["inputs"]["pcm"]["shape"][0]
        fields=output_shapes("essentia",n,hop,samples)
        fields.update({"global_infogain":None,"global_beat_emphasis":None})
        original=folder/"original_quarantined";port=folder/"port"
        norm=load(original,"norm",fields["norm"])[0]
        unchanged={}
        for field,shape in fields.items():
            reference,refmeta=load(original,field,shape);actual,portmeta=load(port,field,shape)
            result["outputs"][field]={"original":refmeta,"port":portmeta}
            result["comparisons"][field]=compare(field,actual,reference,norm)
            if field not in {"odf","odf_status"}:
                old=load(previous/"original",field,shape)[0]
                unchanged[field]=bool(np.array_equal(old,reference,equal_nan=True))
        old=load(previous/"original","odf",fields["odf"])[0]
        new=load(original,"odf",fields["odf"])[0]
        unchanged["five_available_odf_methods"]=bool(np.array_equal(old[:,[0,1,2,3,5]],new[:,[0,1,2,3,5]],equal_nan=True))
        if not all(unchanged.values()):raise ValueError("quarantine changed another original stage")
        result["unaffected_reference_outputs_bitidentical"]=unchanged
        statuses=load(original,"odf_status",fields["odf_status"])[0]
        result["method_availability"]={name:{"status":"source_geometry_rejection" if np.any(statuses[:,i]) else "available",
            "numeric_comparison":"unavailable" if np.any(statuses[:,i]) else "included"} for i,name in enumerate(("hfc","complex","complex_phase","flux","melflux","rms"))}
        result["source_rejection_messages"]=original.with_name(original.name+".odf_errors.txt").read_text(encoding="utf8")
        result["status"]="source_rejection" if all(v["pass"] for v in result["comparisons"].values()) else "fail"
    save_json(folder/"result.json",result)
    print(metadata["case_id"],result["status"],[(k,v.get("mismatches")) for k,v in result["comparisons"].items() if not v["pass"]],flush=True)
    return {"case_id":result["case_id"],"status":result["status"],"result":str(folder/"result.json")}

def main():
    parser=argparse.ArgumentParser()
    parser.add_argument("--previous",type=Path,required=True);parser.add_argument("--output",type=Path,required=True)
    parser.add_argument("--original",required=True);parser.add_argument("--port",required=True);parser.add_argument("--runtime-bin",type=Path)
    opt=parser.parse_args();opt.output.mkdir(parents=True,exist_ok=True);env=os.environ.copy()
    for key in ("OMP_NUM_THREADS","OPENBLAS_NUM_THREADS","MKL_NUM_THREADS"):env[key]="1"
    if opt.runtime_bin:env["PATH"]=str(opt.runtime_bin)+os.pathsep+env["PATH"]
    port_hash=digest(opt.port)
    previous=[p.parent for p in sorted(opt.previous.glob("native.essentia.*/case.json")) if json.loads(p.read_text(encoding="utf8"))["settings"]["N"] in (64,256,512)]
    if len(previous)!=24:raise ValueError("expected exactly 24 previously rejected small-N cases")
    results=[one(path,opt,env,port_hash) for path in previous]
    counts={status:sum(row["status"]==status for row in results) for status in sorted({row["status"] for row in results})}
    save_json(opt.output/"summary.json",{"selection":"only 24 original small-N geometry rejection cases; zero port executions","cases":results,"counts":counts})
    print(json.dumps(counts));return int(any(row["status"]=="pending_reference_runtime" for row in results))

if __name__=="__main__":raise SystemExit(main())
