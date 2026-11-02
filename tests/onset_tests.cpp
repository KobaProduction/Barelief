#include <dsp_kernels/onsets.h>
#include <array>
#include <cassert>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <new>

namespace {bool count_allocations=false;unsigned allocation_count=0;}
void* operator new(std::size_t size) {
    if(count_allocations)++allocation_count;
    if(void* value=std::malloc(size?size:1))return value;
    throw std::bad_alloc{};
}
void operator delete(void* value) noexcept {std::free(value);}
void operator delete(void* value,std::size_t) noexcept {std::free(value);}
namespace {
using namespace dsp_kernels;
struct Fixture {
    PreparedOnsets detector;
    std::array<float,8> values{};
    explicit Fixture(bool split=false) {
        OnsetConfig config;config.feature_count=8;
        if(split){config.channel_count=2;config.channels[0]={0,4};config.channels[1]={4,4};}
        assert(detector.prepare(config));
    }
    void step(unsigned i){assert(detector.process(values,double(i)*.02,double(i)*.02+.001));}
    const OnsetObservation& channel(unsigned i=0) const {return detector.output()[i];}
};
void causal_peak_and_plateau() {
    Fixture f;f.step(0);
    for(unsigned i=1;i<=3;++i){
        f.values.fill(0);f.values[i-1]=1;f.step(i);assert(!f.channel().peak);
    }
    f.step(4);assert(f.channel().peak);
    const auto peak=*f.channel().peak;
    assert(peak.t_obs==.02 && peak.t_ready==.081 && peak.strength==1);
    for(unsigned i=5;i<20;++i){f.step(i);assert(!f.channel().peak);}
}
void separate_refractory_and_simultaneous_peaks() {
    Fixture f(true);f.step(0);f.values[0]=1;f.step(1);f.values.fill(0);f.step(2);
    assert(f.channel().peak && !f.channel(1).peak);
    f.values[0]=1;f.values[4]=1;f.step(3);f.values.fill(0);f.step(4);
    assert(!f.channel().peak && f.channel(1).peak && f.channel(1).peak->t_obs==.06);
    f.step(5);f.step(6);f.step(7);f.values[0]=1;f.values[4]=1;f.step(8);f.values.fill(0);f.step(9);
    assert(f.channel().peak && f.channel(1).peak);
    assert(f.channel().peak->t_obs==f.channel(1).peak->t_obs);
}
void learns_background_without_clipping_input() {
    Fixture f;f.values.fill(1);f.step(0);
    for(unsigned i=1;i<140;++i){
        f.values.fill(i%2?1.2f:1.f);f.step(i);
        if(i>96)assert(!f.channel().peak);
    }
    assert(f.channel().threshold>.08);
    f.values.fill(4);f.step(140);
    assert(f.channel().above_unit_values==8 && f.channel().mean_level==4 && !f.channel().peak);
    f.step(141);assert(f.channel().peak && f.channel().peak->strength>.65);
}
void normalized_uint8_matches_float() {
    OnsetConfig config;config.feature_count=8;
    PreparedOnsets bytes,floats;assert(bytes.prepare(config) && floats.prepare(config));
    std::array<std::uint8_t,8> packed{};std::array<float,8> normalized{};
    for(unsigned i=0;i<200;++i){
        for(unsigned k=0;k<8;++k){packed[k]=std::uint8_t((i*17+k*31)%256);normalized[k]=float(packed[k])/255.f;}
        const double time=double(i)/60;
        assert(bytes.process(packed,time,time+.002) && floats.process(normalized,time,time+.002));
        const auto a=bytes.output()[0],b=floats.output()[0];
        assert(a.flux==b.flux && a.contrast==b.contrast && a.threshold==b.threshold);
        assert(a.peak.has_value()==b.peak.has_value());
        if(a.peak)assert(a.peak->t_obs==b.peak->t_obs && a.peak->strength==b.peak->strength);
    }
}
void invalid_inputs_preserve_pending_peak() {
    Fixture f;f.step(0);f.values[0]=1;f.step(1);
    const auto before=f.channel();
    f.values[0]=-1;assert(!f.detector.process(f.values,.04,.041));
    f.values[0]=std::numeric_limits<float>::quiet_NaN();assert(!f.detector.process(f.values,.04,.041));
    f.values.fill(0);assert(!f.detector.process(f.values,.04,.039));
    assert(!f.detector.process(std::span{f.values}.first(7),.04,.041));
    OnsetConfig bad=f.detector.config();bad.channel_count=1;bad.channels[0]={7,2};
    assert(!f.detector.prepare(bad));
    assert(f.channel().contrast==before.contrast && f.channel().threshold==before.threshold);
    f.step(2);assert(f.channel().peak && f.channel().peak->t_obs==.02);
    assert(!f.detector.process(f.values,.04,.041));assert(f.channel().peak);
}
void priming_reset_and_absolute_floor() {
    Fixture f;f.values.fill(2);f.step(0);
    assert(f.channel().mean_level==2 && f.channel().above_unit_values==8 && !f.channel().contrast);
    f.values.fill(0);f.step(1);assert(!f.channel().peak);
    f.values[0]=1.f/255;f.step(2);f.values.fill(0);f.step(3);assert(!f.channel().peak);
    f.values[0]=1;f.step(4);f.detector.reset();f.values.fill(0);f.step(0);assert(!f.channel().peak);
    PreparedOnsets empty;assert(!empty.process(f.values,0,0));
}
void bounded_hot_path() {
    Fixture f(true);allocation_count=0;count_allocations=true;
    for(unsigned i=0;i<4000;++i){f.values.fill(i%9==0?1.f:0.f);f.step(i);}
    count_allocations=false;assert(!allocation_count);
    assert(PreparedOnsets::workspace_bytes()==sizeof(PreparedOnsets));
}
}
int main() {
    causal_peak_and_plateau();separate_refractory_and_simultaneous_peaks();
    learns_background_without_clipping_input();normalized_uint8_matches_float();
    invalid_inputs_preserve_pending_peak();priming_reset_and_absolute_floor();bounded_hot_path();
    std::cout<<"causal onset contract passed; workspace="<<dsp_kernels::PreparedOnsets::workspace_bytes()<<'\n';
}
