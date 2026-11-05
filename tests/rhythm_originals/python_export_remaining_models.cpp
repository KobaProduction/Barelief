#include "python_export_remaining.h"
#include "python_export_benchmark.h"
#include "python_export_partitions.h"
#include <dsp_kernels/downbeat_state_decoder.h>
#include <dsp_kernels/bar_state_decoder.h>
#include <dsp_kernels/state_beat_sequence.h>
#include <array>

namespace originals {
namespace {
std::array<dsp_kernels::DownbeatPatternConfig,2> downbeat_patterns() {
    std::array<dsp_kernels::DownbeatPatternConfig,2> patterns{};patterns[0].beats=3;patterns[1].beats=4;
    for(auto& p:patterns){p.minimum_bpm=100;p.maximum_bpm=150;p.num_tempi=0;}
    return patterns;
}
void downbeat_outputs(const dsp_kernels::PreparedDownbeatStateDecoder& kernel,Result& result) {
    result.floats("times",kernel.beat_times(),{kernel.beat_times().size()});
    result.states("numbers",kernel.beat_numbers());result.states("path",kernel.state_path());
    result.floats("likelihoods",kernel.model_likelihoods(),{kernel.model_likelihoods().size()});
    const auto selected=kernel.selected_model();result.integers("selected",selected==SIZE_MAX?std::span<const std::size_t>{}:std::span(&selected,1));
    const auto first=kernel.state_path_first_frame();result.integers("first_frame",std::span(&first,1));
}

bool downbeat(const Request& r,const Matrix& input,Result& result) {
    const auto patterns=downbeat_patterns();
    dsp_kernels::DownbeatStateDecoderConfig c;c.frame_rate=50;c.correct=r.size(0)!=0;c.activation_threshold=float(r.number(1));
    dsp_kernels::PreparedDownbeatStateDecoder kernel;
    require(benchmark::prepare([&](std::size_t& b){return decltype(kernel)::storage_bytes(r.frames,patterns,c,b);},[&]{return kernel.prepare(r.frames,patterns,c);}),"prepare supplied downbeat models");
    const bool ok=benchmark::measured_process(r.prefix,kernel.workspace_bytes(),[&]{return kernel.process(input.values);});
    downbeat_outputs(kernel,result);report_workspace(result,kernel.workspace_bytes());return ok;
}
bool bar(const Request& r,const Matrix& input,Result& result) {
    const auto beats=sidecar(r,".beats",1,r.frames);const std::array<std::uint32_t,2> meters{3,4};
    dsp_kernels::BarStateDecoderConfig c;c.meter_change_probability=float(r.number(0));
    dsp_kernels::PreparedBarStateDecoder kernel;
    require(benchmark::prepare([&](std::size_t& b){return decltype(kernel)::storage_bytes(r.frames,meters,c,{},b);},[&]{return kernel.prepare(r.frames,meters,c);}),"prepare bar models");
    const bool ok=benchmark::measured_process(r.prefix,kernel.workspace_bytes(),[&]{return kernel.process(beats.values,input.values);});result.states("numbers",kernel.beat_numbers());result.states("path",kernel.state_path());
    report_workspace(result,kernel.workspace_bytes());return ok;
}
dsp_kernels::BeatStateDecoderConfig state_config(const Request& r) {
    dsp_kernels::BeatStateDecoderConfig c;c.frame_rate=50;c.min_bpm=100;c.max_bpm=150;
    c.correct=r.size(1)!=0;c.activation_threshold=float(r.number(2));return c;
}
void state_outputs(const dsp_kernels::PreparedStateBeatSequence& kernel,Result& result,const std::string& suffix) {
    const auto counter=std::size_t(kernel.frame_counter()),first=kernel.decoder().state_path_first_frame();const auto bpm=kernel.online_bpm();
    result.floats("times"+suffix,kernel.events(),{kernel.events().size()});result.states("path"+suffix,kernel.decoder().state_path());
    result.integers("first_frame"+suffix,std::span(&first,1));result.integers("counter"+suffix,std::span(&counter,1));
    result.floats("bpm"+suffix,std::span(&bpm,1),{1});
}
bool state_offline(const Matrix& input,dsp_kernels::PreparedStateBeatSequence& kernel,Result& result,const std::string& suffix) {
    const bool ok=kernel.process_offline(input.values);state_outputs(kernel,result,suffix);return ok;
}
bool state_online(const Request& r,const Matrix& input,dsp_kernels::PreparedStateBeatSequence& kernel,Result& result) {
    const auto chunks=sidecar(r,".chunks",1,r.size(3)),resets=sidecar(r,".resets",1,r.size(3));std::size_t first=0;
    for(std::size_t i=0;i<chunks.values.size();++i){
        const auto length=std::size_t(chunks.values[i]);require(length && length<=input.values.size()-first,"explicit online wrapper partition");
        if(!kernel.process(std::span(input.values).subspan(first,length),resets.values[i]!=0))return false;
        state_outputs(kernel,result,std::to_string(i));first+=length;
        if(r.size(4) && !i && !state_offline(input,kernel,result,"offline"))return false;
    }
    return first==input.values.size();
}
bool state(const Request& r,const Matrix& input,Result& result) {
    const auto c=state_config(r);dsp_kernels::PreparedStateBeatSequence kernel;
    require(benchmark::prepare([&](std::size_t& b){return decltype(kernel)::storage_bytes(r.frames,c,b);},[&]{return kernel.prepare(r.frames,c,r.size(0)!=0);}),"prepare complete state-beat wrapper");
    if(benchmark::enabled()){
        require(r.size(0) && !r.size(4),"explicit online representative without interlude");
        const auto chunks=sidecar(r,".chunks",1,r.size(3)),resets=sidecar(r,".resets",1,r.size(3));
        const auto partitions=replay_partitions(chunks.values,resets.values,input.values.size());
        if(!benchmark::measured_process(r.prefix,kernel.workspace_bytes(),[&]{return replay(input.values,partitions,[&](auto values,bool reset){return kernel.process(values,reset);});}))return false;
    }
    const bool ok=r.size(0)?state_online(r,input,kernel,result):state_offline(input,kernel,result,"");
    report_workspace(result,kernel.workspace_bytes());return ok;
}
}
bool remaining_models(const Request& r,const Matrix& input,Result& result) {
    if(r.method=="state_wrapper")return state(r,input,result);
    return r.method=="downbeat"?downbeat(r,input,result):bar(r,input,result);
}
}
