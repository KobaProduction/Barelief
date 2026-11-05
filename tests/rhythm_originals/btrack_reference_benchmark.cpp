#include "python_export_benchmark.h"
#include <rhythm_reference_ports/pcm_predictive_beat_tracker.h>
#include <kiss_backend.h>
#include <sinc_backend.h>
#include <algorithm>
#include <cstdio>
#include <fstream>
#include <stdexcept>
#include <string_view>
#include <vector>
namespace {
namespace r=rhythm_reference_ports;
namespace b=originals::benchmark;
void check(bool ok,const char* message){if(!ok)throw std::runtime_error(message);}
struct Arguments {
    r::PcmPredictiveOnsetConfig config;
    std::filesystem::path input,prefix;
};
Arguments arguments(int argc,char** argv) {
    check(argc==6,"usage: frame hop input.f32 output-prefix portable-f32|desktop-reference");Arguments result;
    result.config.frame_samples=std::stoul(argv[1]);result.config.hop_samples=std::stoul(argv[2]);
    check((result.config.frame_samples==1024 || result.config.frame_samples==4096) &&
        result.config.hop_samples==result.config.frame_samples/2,"bounded same native geometries");
    const std::string_view profile=argv[5];check(profile=="portable-f32" || profile=="desktop-reference","explicit precision profile");
    if(profile=="desktop-reference")result.config.precision=dsp_kernels::NumericalPrecision::DesktopReference;
    result.input=argv[3];result.prefix=argv[4];return result;
}
std::vector<float> read(const std::filesystem::path& path) {
    std::ifstream file(path,std::ios::binary|std::ios::ate);check(bool(file),"open same native PCM");const auto bytes=file.tellg();
    check(bytes>0 && bytes%4==0 && bytes<=4*16384*128,"bounded Float32 PCM");std::vector<float> result(std::size_t(bytes)/4);
    file.seekg(0);file.read(reinterpret_cast<char*>(result.data()),bytes);check(bool(file),"read native PCM");return result;
}
template<class T> void write(const std::filesystem::path& prefix,const char* suffix,const std::vector<T>& values) {
    std::ofstream file(prefix.string()+suffix,std::ios::binary);
    file.write(reinterpret_cast<const char*>(values.data()),std::streamsize(values.size()*sizeof(T)));check(bool(file),"write untimed functional output");
}
struct Outputs {
    std::vector<float> state,resampled,comb,delta,onsets;
    std::vector<std::uint64_t> beat;
    explicit Outputs(std::size_t frames):state(3*frames),resampled(512*frames),comb(128*frames),delta(41*frames),onsets(frames),beat(frames){}
    void collect(std::size_t frame,const r::PreparedPcmPredictiveBeatTracker& operation,const r::PcmPredictiveBeatObservation& row) {
        const auto& tracker=operation.tracker_stage();const auto& tempo=tracker.tempo_stage();
        state[3*frame]=row.bpm;state[3*frame+1]=tracker.latest_cumulative_score();state[3*frame+2]=row.prediction.period_frames;
        onsets[frame]=row.onset;beat[frame]=std::uint64_t(row.prediction.beat_due);
        std::copy(tracker.resampled_onsets().begin(),tracker.resampled_onsets().end(),resampled.begin()+std::ptrdiff_t(512*frame));
        std::copy(tempo.response().begin(),tempo.response().end(),comb.begin()+std::ptrdiff_t(128*frame));
        std::copy(tempo.state_probabilities().begin(),tempo.state_probabilities().end(),delta.begin()+std::ptrdiff_t(41*frame));
    }
    void save(const std::filesystem::path& prefix) const {
        write(prefix,".state.f32",state);write(prefix,".resampled.f32",resampled);write(prefix,".comb.f32",comb);
        write(prefix,".delta.f32",delta);write(prefix,".odf.f32",onsets);write(prefix,".beat.u64",beat);
    }
};
class Replay {
public:
    explicit Replay(Arguments args):args_(std::move(args)),pcm_(read(args_.input)),rows_(pcm_.size()/args_.config.hop_samples),outputs_(rows_.size()) {
        check(pcm_.size()%args_.config.hop_samples==0 && rows_.size()==128,"same128 complete native hops");
    }
    bool prepare() {
        const auto* fft=args_.config.precision==dsp_kernels::NumericalPrecision::DesktopReference?&desktop_fourier::kiss_backend_factory():nullptr;
        const auto& factory=desktop_resampling::sinc_backend_factory();std::size_t bound=0;
        if(!decltype(operation_)::storage_bytes(factory,args_.config,bound,fft))return false;
        return b::measured_prepare(bound,[&](){return operation_.prepare(factory,args_.config,fft);});
    }
    bool timed() {
        auto process=[this](){return replay();};
        return b::measured_process(args_.prefix,operation_.workspace_bytes(),process);
    }
    void functional_output() {
        check(operation_.restart(),"untimed functional reset");
        for(std::size_t i=0;i<rows_.size();++i){
            check(operation_.process_hop(block(i),rows_[i]),"untimed full functional replay");outputs_.collect(i,operation_,rows_[i]);
        }
        outputs_.save(args_.prefix);
    }
private:
    std::span<const float> block(std::size_t frame) const noexcept {
        return std::span(pcm_).subspan(frame*args_.config.hop_samples,args_.config.hop_samples);
    }
    bool replay() noexcept {
        if(!operation_.restart())return false;
        for(std::size_t i=0;i<rows_.size();++i)if(!operation_.process_hop(block(i),rows_[i]))return false;
        return true;
    }
    Arguments args_;
    std::vector<float> pcm_;
    std::vector<r::PcmPredictiveBeatObservation> rows_;
    Outputs outputs_;
    r::PreparedPcmPredictiveBeatTracker operation_;
};
}
int main(int argc,char** argv) {
    try{
        check(b::enabled(),"BARELIEF_DSP_BENCHMARK=1 required");Replay replay(arguments(argc,argv));
        check(replay.prepare() && replay.timed(),"whole prepared BTrack metrology");replay.functional_output();
        std::puts("PreparedBTrack 3warm/64complete128hop DSP-only replays +untimedfunctionaloutput done");return 0;
    }catch(const std::exception& error){std::fprintf(stderr,"%s\n",error.what());return 1;}
}
