#include "python_export.h"
#include <dsp_kernels/mel_filter_bank.h>
#include <dsp_kernels/power_to_db.h>
#include <dsp_kernels/log_compression.h>

namespace originals {
namespace {
bool mel(const Request& r,const Matrix& input,Result& result) {
    dsp_kernels::MelFilterBankConfig c;c.sample_rate=r.number(0);c.bin_hz=c.sample_rate/r.number(1);c.first_bin=0;
    c.bands=r.size(2);c.fmin_hz=r.number(3);c.fmax_hz=r.number(4);c.scale=dsp_kernels::MelScale(r.size(5));
    c.normalization=dsp_kernels::MelNormalization(r.size(6));dsp_kernels::MelFilterBank kernel;
    require(kernel.prepare(r.columns,c),"prepare Mel filters");Matrix output(r.frames,c.bands);bool ok=true;
    for(std::size_t row=0;row<r.frames && ok;++row)ok=kernel.process(std::span(input.values).subspan(row*r.columns,r.columns),std::span(output.values).subspan(row*c.bands,c.bands));
    result.floats("values",output.values,{r.frames,c.bands});result.floats("weights",kernel.weights(),{c.bands,r.columns});return ok;
}
bool decibels(const Request& r,const Matrix& input,Result& result) {
    dsp_kernels::PowerToDbConfig c;c.reference=float(r.number(0));c.amin=float(r.number(1));c.top_db=float(r.number(2));
    c.reference_mode=dsp_kernels::DbReference(r.size(3));c.top_db_enabled=r.size(4)!=0;
    dsp_kernels::PreparedPowerToDb kernel;require(kernel.prepare(r.frames,r.columns,c),"prepare power to dB");
    Matrix output(r.frames,r.columns);const bool ok=kernel.process(input.source(),r.frames,r.columns,output.destination());
    result.floats("values",output.values,{r.frames,r.columns});return ok;
}
bool logarithm(const Request& r,const Matrix& input,Result& result) {
    std::vector<float> output(input.values.size());const bool ok=dsp_kernels::logarithmic_compression(input.values,output,float(r.number(0)),float(r.number(1)),dsp_kernels::LogarithmBase(r.size(2)));
    result.floats("values",output,{r.frames,r.columns});return ok;
}
}
bool filters(const Request& r,const Matrix& input,Result& result) {
    if(r.method=="mel")return mel(r,input,result);
    if(r.method=="decibels")return decibels(r,input,result);
    if(r.method=="logarithm")return logarithm(r,input,result);
    require(false,"unknown filter method");return false;
}
}
