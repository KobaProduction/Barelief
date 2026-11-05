// Fixed same-input replay of the prepared source comb path. No original rerun.
#include <rhythm_reference_ports/predictive_beat_tracker.h>
#include <kiss_backend.h>
#include <cstdio>
#include <cstdint>
#include <fstream>
#include <stdexcept>
#include <string>
#include <vector>
namespace {
void check(bool ok,const char* message){if(!ok)throw std::runtime_error(message);}
std::vector<float> read(const char* path) {
    std::ifstream file(path,std::ios::binary|std::ios::ate);check(bool(file),"open SRC rows");const auto bytes=file.tellg();
    check(bytes>0 && std::size_t(bytes)%(512*sizeof(float))==0,"complete512 SRC rows");std::vector<float> input(std::size_t(bytes)/4);
    file.seekg(0);file.read(reinterpret_cast<char*>(input.data()),bytes);check(bool(file),"read SRC rows");return input;
}
template<class T> void write(const std::string& path,const std::vector<T>& values) {
    std::ofstream out(path,std::ios::binary);out.write(reinterpret_cast<const char*>(values.data()),std::streamsize(values.size()*sizeof(T)));check(bool(out),"write stage");
}
struct Capture {
    std::vector<double> threshold,acf,comb,state;
    void collect(const rhythm_reference_ports::detail::PreparedPredictiveReferenceComb& source) {
        threshold.insert(threshold.end(),source.thresholded_onsets().begin(),source.thresholded_onsets().end());
        acf.insert(acf.end(),source.autocorrelation().begin(),source.autocorrelation().end());
        comb.insert(comb.end(),source.response().begin(),source.response().end());
        state.insert(state.end(),source.probabilities().begin(),source.probabilities().end());
    }
    void save(const std::string& prefix) const{write(prefix+".threshold.f64",threshold);write(prefix+".acf.f64",acf);write(prefix+".comb.f64",comb);write(prefix+".state.f64",state);}
};
void replay(std::span<const float> rows,std::size_t hop,const std::string& prefix) {
    rhythm_reference_ports::PreparedCombTempoTransition operation;const auto* factory=&desktop_fourier::kiss_backend_factory();std::size_t query=0;
    check(decltype(operation)::storage_bytes(hop,query,dsp_kernels::NumericalPrecision::DesktopReference,factory) &&
        operation.prepare(hop,dsp_kernels::NumericalPrecision::DesktopReference,factory) && operation.workspace_bytes()<=query,"prepare source comb/query");
    Capture capture;float period=0;
    for(std::size_t at=0;at<rows.size();at+=512){check(operation.process(rows.subspan(at,512),period),"source comb process");capture.collect(operation.reference_stage());}
    capture.save(prefix);write(prefix+".workspace.u64",std::vector<std::uint64_t>{query,operation.workspace_bytes(),1});
}
}
int main(int argc,char** argv) {
    try{
        check(argc==4,"usage: hop src-rows.f32 prefix");const auto hop=std::stoul(argv[1]);const auto input=read(argv[2]);replay(input,hop,argv[3]);
        std::printf("{\"rows\":%zu,\"status\":\"ok\"}\n",input.size()/512);return 0;
    }catch(const std::exception& e){std::fprintf(stderr,"%s\n",e.what());return 1;}
}
