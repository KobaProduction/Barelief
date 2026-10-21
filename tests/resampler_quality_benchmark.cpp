#include <dsp_kernels/prepared_resampler.h>
#include "resampler_quality_oracle.h"
#include "speex_backend.h"
#include "../examples/esp32_resampler_benchmark/common/packet_benchmark.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <charconv>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <numbers>
#include <new>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif
extern "C" std::size_t qtvis_speex_allocation_calls();
namespace {std::atomic<std::size_t> study_allocations{0};}
void* operator new(std::size_t bytes) {++study_allocations;if(auto* p=std::malloc(bytes?bytes:1))return p;throw std::bad_alloc();}
void* operator new[](std::size_t bytes) {return ::operator new(bytes);}
void operator delete(void* p) noexcept {std::free(p);}
void operator delete[](void* p) noexcept {std::free(p);}
void operator delete(void* p,std::size_t) noexcept {std::free(p);}
void operator delete[](void* p,std::size_t) noexcept {std::free(p);}

namespace {
using dsp_kernels::PreparedResampler;
using dsp_kernels::ResamplerMode;
using resampler_test_oracle::pi;
using resampler_test_oracle::amplitude;
constexpr std::size_t packet=256;
std::filesystem::path spectrum_directory;
using resampler_test_oracle::fit;
void require(bool condition,const char* message){if(!condition)throw std::runtime_error(message);}
double db(double ratio){return 20*std::log10(std::max(ratio,1e-30));}
double wrap(double phase){return std::remainder(phase,2*pi);}
const char* name(ResamplerMode mode){switch(mode){
    case ResamplerMode::ExistingFir:return "full_fir";
    case ResamplerMode::CompactFir:return "compact_fir";
    case ResamplerMode::SpeexDsp:return "speex_q5";
    case ResamplerMode::IirExperimental:return "iir";
    case ResamplerMode::LinearDiagnostic:return "linear";
    default:throw std::runtime_error("Unexpected mode");}}
std::vector<float> signal(unsigned rate,const std::vector<double>& frequencies,double phase_offset=0){
    require(frequencies.size()==1 || frequencies.size()==7,"Expected isolated or seven-tone probe");
    std::vector<float> input(2*rate);
    for(std::size_t i=0;i<input.size();++i){double sample=0;
        for(std::size_t k=0;k<frequencies.size();++k)
            sample+=amplitude*std::sin(2*pi*frequencies[k]*double(i)/rate+.13*k+phase_offset);
        input[i]=float(sample);
    }
    return input;
}
std::vector<float> process(PreparedResampler& op,const std::vector<float>& input,std::size_t block){
    const auto& c=op.config();std::vector<float> result;
    result.reserve(PreparedResampler::maximum_output(input.size(),c.input_rate,c.output_rate));
    std::vector<float> output(PreparedResampler::maximum_output(block,c.input_rate,c.output_rate));
    for(std::size_t at=0;at<input.size();){const auto count=std::min(block,input.size()-at);std::size_t written=0;
        require(op.process(std::span(input).subspan(at,count),count,count,output,output.size(),written),"Processing failed");
        result.insert(result.end(),output.begin(),output.begin()+written);at+=count;
    }
    require(result.size()==2*c.output_rate,"Unexpected output count");return result;
}
void fit_control(){
    const unsigned rate=36000;const std::vector<double> frequencies{100,230,900,2700,5300,7500,9900};
    const auto values=signal(rate,frequencies);const auto measured=fit(values,rate,frequencies);
    for(const auto& tone:measured.tones){require(std::abs(std::hypot(tone.sine,tone.cosine)-amplitude)<1e-8,"Fit gain control failed");
        require(std::abs(wrap(std::atan2(tone.cosine,tone.sine)-tone.phase))<1e-7,"Fit phase control failed");}
    require(measured.residual<1e-8,"Fit residual control failed");
}
void probe(std::ostream& file,PreparedResampler& op,unsigned in,unsigned out,ResamplerMode mode,
           const char* kind,const std::vector<double>& frequencies,bool partition_control=false,double phase_offset=0){
    const auto input=signal(in,frequencies,phase_offset);op.reset();const auto output=process(op,input,packet);
    if(partition_control){op.reset();const auto whole=process(op,input,input.size());
        require(whole==output,"Packet256 equivalence failed");
        file<<"{\"kind\":\"partition\",\"method\":\""<<name(mode)<<"\",\"in\":"<<in<<",\"out\":"<<out
            <<",\"input_count\":"<<input.size()<<",\"output_count\":"<<output.size()<<",\"max_abs\":0}\n";
    }
    const auto result=fit(output,out,frequencies,phase_offset);
    for(std::size_t k=0;k<frequencies.size();++k){const auto& tone=result.tones[k];
        const auto gain=std::hypot(tone.sine,tone.cosine)/amplitude;
        file<<"{\"kind\":\""<<kind<<"\",\"method\":\""<<name(mode)<<"\",\"in\":"<<in<<",\"out\":"<<out
            <<",\"frequency_hz\":"<<frequencies[k]<<",\"folded_hz\":"<<tone.frequency<<",\"gain_db\":"<<db(gain)
            <<",\"input_phase_rad\":"<<phase_offset<<",\"sine_coefficient\":"<<tone.sine<<",\"cosine_coefficient\":"<<tone.cosine
            <<",\"phase_rad\":"<<wrap(std::atan2(tone.cosine,tone.sine)-(frequencies[k]>out/2.?pi-tone.phase:tone.phase))<<"}\n";
    }
    if(std::string(kind)=="pass_control")file<<"{\"kind\":\"residual\",\"method\":\""<<name(mode)<<"\",\"in\":"<<in
        <<",\"out\":"<<out<<",\"residual_dbc\":"<<db(result.residual/result.fitted)<<",\"dc\":"<<result.dc<<"}\n";
    if(std::string(kind)=="pass_control" || std::string(kind)=="spectrum"){
        const std::string case_name=std::string(kind)=="pass_control"?"multitone":"tone5300";
        const auto basename=std::string(name(mode))+"_"+std::to_string(in)+"_"+std::to_string(out)+"_"+case_name+".f32";
        std::ofstream samples(spectrum_directory/basename,std::ios::binary);
        samples.write(reinterpret_cast<const char*>(output.data()+out),std::streamsize(out*sizeof(float)));
        require(bool(samples),"Cannot write spectrum samples");
        file<<"{\"kind\":\"spectrum_buffer\",\"method\":\""<<name(mode)<<"\",\"in\":"<<in<<",\"out\":"<<out
            <<",\"case\":\""<<case_name<<"\",\"sample_begin\":"<<out<<",\"count\":"<<out<<",\"dtype\":\"float32le\",\"path\":\"spectra/"<<basename<<"\"}\n";
    }
}
void sweep(std::ostream& file,PreparedResampler& op,unsigned in,unsigned out,ResamplerMode mode,bool stop){
    std::set<double> unique;
    for(unsigned f=100;f<in/2;f+=100)if((f>out/2)==stop && f!=out/2)unique.insert(f);
    for(unsigned f=14000;f<=17000;f+=25)if((f>out/2)==stop && f<in/2 && f!=out/2)unique.insert(f);
    for(double f:{230.,900.,2700.,5300.,9900.,12000.,14000.,15000.,18000.,20000.})
        if((f>out/2)==stop && f<in/2 && f!=out/2)unique.insert(f);
    if(stop)for(double delta:{1.,10.,100.}){
        const double frequency=out/2.+delta;
        if(frequency<in/2.)unique.insert(frequency);
    }
    // Isolate sweep tones: rational resamplers are periodically time varying,
    // so interpolation sidebands of simultaneous tones can contaminate a fit.
    for(double frequency:unique)for(double phase:{0.,pi/2})
        probe(file,op,in,out,mode,stop?"stop":"pass",{frequency},false,phase);
}

void rate_study(std::ostream& file,bool timing_only,std::span<const unsigned> outputs){
#ifdef _WIN32
    require(SetThreadAffinityMask(GetCurrentThread(),DWORD_PTR(1))!=0,"Study thread affinity failed");
#endif
    constexpr std::array<std::size_t,9> packets{49,64,76,98,128,147,196,245,256};
    const auto& factory=desktop_resampling::speex_backend_factory();
    const resampler_packet_benchmark::Hooks hooks{
        []()->std::uint64_t{return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();},1000.,false,
        [](){std::this_thread::yield();},[]()->std::size_t{return 0;},[](){return true;},
        [](){return study_allocations.load()+qtvis_speex_allocation_calls();},2000};
    std::printf("{\"check\":\"study_platform\",\"platform\":\"windows\",\"logical_cpu\":0,\"individual_latency\":false,\"minimum_batch_target_us\":2000,\"heap_measurement_available\":false}\n");
    // Finish all timing before the CPU-heavy frequency sweep.
    for(unsigned in:{48000u,44100u})for(unsigned out:outputs){
        PreparedResampler op;require(op.prepare({ResamplerMode::SpeexDsp,in,out,1,5},&factory),"Study preparation failed");
        const auto r=op.resources();
        file<<"{\"kind\":\"study_resources\",\"method\":\"speex_q5\",\"in\":"<<in<<",\"out\":"<<out
            <<",\"coefficient_bytes\":"<<r.coefficient_bytes<<",\"state_bytes\":"<<r.state_bytes<<",\"owned_bytes\":"<<r.owned_bytes<<"}\n";
        if(!timing_only){
            // Same seven-tone stream, changing packet boundaries only.
            std::vector<float> input(2*in);resampler_packet_benchmark::fill(input,in);
            op.reset();const auto reference=process(op,input,input.size());
            for(auto n:packets){op.reset();const auto fragmented=process(op,input,n);const auto after=op.resources();
                require(fragmented==reference,"Study packet partition mismatch");
                require(after.coefficient_bytes==r.coefficient_bytes && after.state_bytes==r.state_bytes && after.owned_bytes==r.owned_bytes,"Packet changed prepared resources");
                file<<"{\"kind\":\"study_partition\",\"method\":\"speex_q5\",\"in\":"<<in<<",\"out\":"<<out
                    <<",\"packet\":"<<n<<",\"outputs\":"<<fragmented.size()<<",\"max_abs\":0,\"coefficient_bytes\":"<<after.coefficient_bytes<<"}\n";
            }
        }
        for(auto n:packets)resampler_packet_benchmark::time_packet(op,n,hooks,0);
        require(resampler_packet_benchmark::success,"Study timing guard/allocation check failed");
    }
    if(!timing_only)for(unsigned in:{48000u,44100u})for(unsigned out:outputs){
        PreparedResampler op;require(op.prepare({ResamplerMode::SpeexDsp,in,out,1,5},&factory),"Study quality preparation failed");
        probe(file,op,in,out,ResamplerMode::SpeexDsp,"pass_control",{230,900,2700,5300,9900,15000,16000},true);
        probe(file,op,in,out,ResamplerMode::SpeexDsp,"spectrum",{5300});
        sweep(file,op,in,out,ResamplerMode::SpeexDsp,false);sweep(file,op,in,out,ResamplerMode::SpeexDsp,true);
        std::cout<<"study "<<in<<"->"<<out<<" quality done\n"<<std::flush;
    }
}
}
namespace {
std::vector<unsigned> selected_study_rates(int argc, char **argv) {
    std::vector<unsigned> study_rates{35000, 35200, 35280, 35700, 36000, 36750, 36960, 37000};
    if (argc != 4)
        return study_rates;
    study_rates.clear();
    std::string_view remaining(argv[3]);
    require(!remaining.empty() && remaining.back() != ',', "Empty study output rate");
    while (!remaining.empty()) {
        const auto comma = remaining.find(',');
        const auto token = remaining.substr(0, comma);
        unsigned rate = 0;
        const auto parsed = std::from_chars(token.data(), token.data() + token.size(), rate);
        require(parsed.ec == std::errc{} && parsed.ptr == token.data() + token.size() && rate > 32000 && rate < 44100 &&
                    rate % 2 == 0,
                "Study rates must be even integer Hz in (32000,44100)");
        require(study_rates.size() < 32 && std::find(study_rates.begin(), study_rates.end(), rate) == study_rates.end(),
                "Duplicate or excessive study rates");
        study_rates.push_back(rate);
        if (comma == std::string_view::npos)
            break;
        remaining.remove_prefix(comma + 1);
    }

    return study_rates;
}
} // namespace
int main(int argc, char **argv) {
    try {
        require(argc == 2 || ((argc == 3 || argc == 4) && (std::string(argv[2]) == "--rate-study" ||
                                                           std::string(argv[2]) == "--rate-study-timing")),
                "Usage: resampler_quality_benchmark output.jsonl [--rate-study|--rate-study-timing "
                "[even_output_rates_csv]]");
        const auto study_rates = selected_study_rates(argc, argv);
        fit_control();
        spectrum_directory = std::filesystem::path(argv[1]).parent_path() / "spectra";
        std::filesystem::create_directories(spectrum_directory);
        std::ofstream file(argv[1]);
        require(bool(file), "Cannot open output");
        file << std::setprecision(17);
        file << "{\"kind\":\"methodology\",\"packet\":256,\"signal_seconds\":2,\"settling_seconds\":1,\"measurement_"
                "seconds\":1,\"control_tones\":7,\"sweep_tones\":1,\"sweep_phases\":2,\"amplitude_per_tone\":0.08,"
                "\"fit_control\":true}\n";
        if (argc >= 3) {
            rate_study(file, std::string(argv[2]) == "--rate-study-timing", study_rates);
            file.flush();
            require(bool(file), "Study output write failed");
            std::cout << "RESAMPLER_RATE_STUDY_PASS\n";
            return 0;
        }
        const auto &factory = desktop_resampling::speex_backend_factory();
        for (unsigned in : {48000u, 44100u})
            for (unsigned out : {36000u})
                for (auto mode : {ResamplerMode::ExistingFir, ResamplerMode::CompactFir, ResamplerMode::SpeexDsp,
                                  ResamplerMode::IirExperimental, ResamplerMode::LinearDiagnostic}) {
                    PreparedResampler op;
                    require(op.prepare({mode, in, out, 1, 5}, &factory), "Preparation failed");
                    const auto resources = op.resources();
                    file << "{\"kind\":\"resources\",\"method\":\"" << name(mode) << "\",\"in\":" << in
                         << ",\"out\":" << out
                         << ",\"fixed_delay_input_samples\":" << resources.fixed_delay_input_samples
                         << ",\"frequency_dependent_delay\":"
                         << (resources.frequency_dependent_delay ? "true" : "false")
                         << ",\"owned_bytes\":" << resources.owned_bytes
                         << ",\"coefficient_bytes\":" << resources.coefficient_bytes
                         << ",\"state_bytes\":" << resources.state_bytes << "}\n";
                    probe(file, op, in, out, mode, "pass_control", {230, 900, 2700, 5300, 9900, 15000, 16000}, true);
                    probe(file, op, in, out, mode, "spectrum", {5300});
                    sweep(file, op, in, out, mode, false);
                    sweep(file, op, in, out, mode, true);
                    for (double center : {900., 5300., 9900., 16000.}) {
                        for (int delta : {-1, 0, 1})
                            probe(file, op, in, out, mode, "delay", {center + delta});
                    }
                    std::cout << name(mode) << ' ' << in << "->" << out << " done\n" << std::flush;
                }
        file.flush();
        require(bool(file), "Output write failed");
        std::cout << "RESAMPLER_QUALITY_PASS\n";
        return 0;
    } catch (const std::exception &e) {
        std::cerr << "RESAMPLER_QUALITY_FAIL: " << e.what() << '\n';
        return 1;
    }
}
