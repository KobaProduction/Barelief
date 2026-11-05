"""Isolate aubio descriptor arithmetic using the exact original Float32 polar rows."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import numpy as np
from native_compare import execute,load,compare,POLICY,PINS,save_json

def main():
    parser=argparse.ArgumentParser()
    parser.add_argument("--case",type=Path,required=True)
    parser.add_argument("--original",required=True)
    parser.add_argument("--port",required=True)
    parser.add_argument("--output",type=Path,required=True)
    parser.add_argument("--runtime-bin",type=Path)
    options=parser.parse_args()
    source_case=json.loads((options.case/"case.json").read_text(encoding="utf8"))
    settings=source_case["settings"]
    n,hop,rate=settings["N"],settings["hop"],settings["Fs"]
    norm=load(options.case/"original","norm")[0].reshape(-1,n//2+1)
    phase=load(options.case/"original","phase")[0].reshape(norm.shape)
    values=np.stack((norm,phase),axis=1).astype("<f4")
    options.output.mkdir(parents=True,exist_ok=True)
    path=options.output/"shared-polar.f32"
    values.tofile(path)
    case={"format":"barelief-original-case-v1","case_id":source_case["case_id"]+".shared-polar",
          "source":source_case["source"],"settings":{**settings,"operation":"aubio_specdesc_do; FFT bypassed in both calls"},
          "inputs":{"polar":{"path":str(path),"dtype":"float32-le","shape":list(values.shape),"order":"C",
                               "sha256":hashlib.sha256(path.read_bytes()).hexdigest()}},"comparison_policy":POLICY}
    save_json(options.output/"case.json",case)
    env=os.environ.copy()
    if options.runtime_bin:env["PATH"]=str(options.runtime_bin)+os.pathsep+env["PATH"]
    args=["aubio_features",str(n),str(hop),str(rate),str(path)]
    results={name:execute(binary,[*args,str(options.output/name),"0"],env) for name,binary in (("original",options.original),("port",options.port))}
    if any(run["returncode"] for run in results.values()):
        save_json(options.output/"result.json",{"status":"runtime_failure","runs":results})
        return 1
    shape=[len(values),9]
    expected=load(options.output/"original","odf",shape)[0]
    actual=load(options.output/"port","odf",shape)[0]
    saved=load(options.case/"original","odf",shape)[0]
    names=("energy","hfc","complex","phase","wphase","specdiff","kl","mkl","specflux")
    comparisons={name:compare("odf",actual[:,i],expected[:,i],None) for i,name in enumerate(names)}
    summary={"status":"pass" if all(v["pass"] for v in comparisons.values()) else "fail","runs":results,
             "comparisons":comparisons,"reference_matches_original_pcm_descriptor":bool(np.array_equal(saved,expected,equal_nan=True))}
    save_json(options.output/"result.json",summary)
    print(json.dumps(summary,indent=2))
    return int(summary["status"]!="pass")

if __name__=="__main__":
    raise SystemExit(main())
