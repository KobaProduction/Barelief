#include <dsp_kernels/beat_tracker.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <limits>
#include <string>

namespace {
using dsp_kernels::PreparedBeatTracker;using dsp_kernels::BeatTrackerConfig;
std::size_t allocations=0;
void check(bool ok,const char* why){if(!ok){std::fprintf(stderr,"FAIL beat tracker: %s\n",why);std::exit(1);}}
void regular_pulses() {
    PreparedBeatTracker tracker;BeatTrackerConfig config;config.frame_rate=80;
    check(tracker.prepare(400,config),"explicit prepared tracker");
    std::array<float,400> onset{};std::array<std::uint8_t,400> beats{};
    for(std::size_t i=10;i<onset.size();i+=40)onset[i]=1;
    const std::array bpm{120.f};const auto before=allocations;
    check(tracker.process(onset,bpm,beats) && allocations==before,"no allocation on hot path");
    // The original whole-window tail rule extrapolates to frame399, selecting
    // predecessor369; trim removes399 but preserves369. Keep that boundary
    // behavior instead of silently snapping the imported method to a peak.
    for(std::size_t i=0;i<beats.size();++i)check(beats[i]==std::uint8_t((i>=10 && i<370 && (i-10)%40==0) || i==369),"original periodic path and window-tail boundary");
    check(tracker.local_scores().size()==400 && tracker.cumulative_scores().size()==400 && tracker.backlinks().size()==400,"observable intermediate scores and links");
    const auto previous=beats;std::array<float,400> curve{};curve.fill(120);
    check(tracker.process(onset,curve,beats) && beats==previous,"explicit constant curve matches scalar tempo");
    for(std::size_t i=0;i<curve.size();++i)curve[i]=90.f+float(i)*.2f;
    const auto dynamic_before=allocations;
    check(tracker.process(onset,curve,beats) && allocations==dynamic_before,"changing tempo and Gaussian periods use only prepared storage");
    const auto links=tracker.backlinks();
    for(std::size_t i=0;i<links.size();++i)check(links[i]<std::int32_t(i),"time-varying DP path has no forward or self links");
    check(tracker.prepare(400,config) && tracker.process(onset,bpm,beats) && beats==previous,"no hidden tracker history between estimates");
}
void silence_and_short_inputs() {
    PreparedBeatTracker tracker;check(tracker.prepare(8),"prepare silence test");
    const std::array bpm{120.f};std::array<float,8> zero{};std::array<std::uint8_t,8> output{};output.fill(9);
    check(tracker.process(zero,bpm,output) && std::all_of(output.begin(),output.end(),[](auto x){return !x;}),"silence gives no beats");
    check(tracker.process({},bpm,{}),"empty history has an empty path");
    const std::array one{1.f};std::array<std::uint8_t,1> single{9};
    check(tracker.process(one,bpm,single) && single[0]==0,"single frame cannot establish a beat interval");
    zero.fill(-1);check(tracker.process(zero,bpm,output),"signed finite envelope is accepted");
    check(std::all_of(output.begin(),output.end(),[](auto x){return !x;}),"no positive activity cannot establish beats");
}
void invalid_inputs_are_atomic() {
    PreparedBeatTracker tracker;check(tracker.prepare(8),"prepare validation test");
    std::array<float,8> envelope{0,0,1,0,0,0,1,0};std::array<std::uint8_t,8> out{};out.fill(7);
    const std::array bpm{120.f};envelope[3]=std::numeric_limits<float>::quiet_NaN();
    check(!tracker.process(envelope,bpm,out) && out[0]==7 && tracker.local_scores().empty(),"nonfinite rejection leaves caller output unchanged");
    envelope[3]=0;const std::array wrong{120.f,130.f};
    check(!tracker.process(envelope,wrong,out),"tempo curve must have one value or one per frame");
    for(const float invalid:std::array{0.f,-100.f,INFINITY,1e-30f,1e30f}){
        const std::array value{invalid};check(!tracker.process(envelope,value,out),"invalid or out-of-domain tempo rejected");
    }
    check(std::all_of(out.begin(),out.end(),[](auto x){return x==7;}),"invalid tempo leaves whole output unchanged");
    BeatTrackerConfig invalid;invalid.tightness=0;std::size_t bytes=17;
    check(!tracker.prepare(8,invalid) && !PreparedBeatTracker::storage_bytes(8,invalid,bytes) && bytes==17,"invalid preparation and accounting are atomic");
    check(tracker.process(envelope,bpm,out),"failed preparation preserves prior valid configuration");
}
template<class T> std::vector<T> read(const char* path,std::size_t size) {
    std::vector<T> values(size);std::ifstream file(path,std::ios::binary);
    file.read(reinterpret_cast<char*>(values.data()),std::streamsize(values.size()*sizeof(T)));
    check(bool(file),"read reference fixture");return values;
}
template<class T> void write(const std::string& path,std::span<const T> values) {
    std::ofstream file(path,std::ios::binary);file.write(reinterpret_cast<const char*>(values.data()),std::streamsize(values.size()*sizeof(T)));
    check(bool(file),"write reference result");
}
int reference(int argc,char** argv) {
    check(argc==10,"reference arguments");const auto count=std::stoull(argv[4]),tempi=std::stoull(argv[5]);
    BeatTrackerConfig config;config.frame_rate=std::stof(argv[6]);config.tightness=std::stof(argv[7]);config.trim=std::stoul(argv[8])!=0;
    config.maximum_period_frames=std::stoull(argv[9]);PreparedBeatTracker tracker;
    check(tracker.prepare(std::max<std::size_t>(1,count),config),"reference prepare");
    const auto onset=read<float>(argv[1],count),bpm=read<float>(argv[2],tempi);std::vector<std::uint8_t> beats(count);
    check(tracker.process(onset,bpm,beats),"reference process");
    write<std::uint8_t>(std::string(argv[3])+".u8",beats);write(std::string(argv[3])+"-local.f32",tracker.local_scores());
    write(std::string(argv[3])+"-cumulative.f32",tracker.cumulative_scores());write(std::string(argv[3])+"-backlink.i32",tracker.backlinks());return 0;
}
}
void* operator new(std::size_t n){++allocations;auto* p=std::malloc(n?n:1);if(!p)std::abort();return p;}
void* operator new[](std::size_t n){return ::operator new(n);}
void operator delete(void* p) noexcept{std::free(p);}void operator delete[](void* p) noexcept{std::free(p);}
void operator delete(void* p,std::size_t) noexcept{std::free(p);}void operator delete[](void* p,std::size_t) noexcept{std::free(p);}
int main(int argc,char** argv){if(argc>1)return reference(argc,argv);regular_pulses();silence_and_short_inputs();invalid_inputs_are_atomic();std::puts("Float32 DP beat tracker validation/no-allocation/path PASS");}
