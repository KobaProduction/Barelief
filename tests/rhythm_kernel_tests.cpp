#include <dsp_kernels/rhythm.h>
#include <array>
#include <cassert>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <new>

namespace {bool observe_allocations=false;std::size_t allocations=0;}
void* operator new(std::size_t size) {
    if(observe_allocations)++allocations;
    if(auto* result=std::malloc(size?size:1))return result;
    throw std::bad_alloc{};
}
void operator delete(void* value) noexcept {std::free(value);}
void operator delete(void* value,std::size_t) noexcept {std::free(value);}

namespace {
namespace r=dsp_kernels::rhythm;
struct Fixture {
    r::Analyzer analyzer;
    std::array<std::uint8_t,64> features{};
    r::Frame frame;
    Fixture(){assert(analyzer.prepare());frame.features=features;frame.sample_rate_hz=32768;}
    void step(unsigned index) {
        frame.sequence=index;frame.t_obs=double(index)/60;frame.t_ready=frame.t_obs;frame.t_pub=frame.t_obs;
        frame.sample_index=std::uint64_t(std::llround(frame.t_obs*32768));assert(analyzer.process(frame));
    }
};
void bounded_processing() {
    Fixture fixture;allocations=0;observe_allocations=true;
    for(unsigned i=0;i<800;++i){fixture.features.fill(i%30==0?200:0);fixture.step(i);}
    observe_allocations=false;
    assert(allocations==0);assert(fixture.analyzer.output().history_frames==512);
    assert(fixture.analyzer.output().tempo.status!=r::Status::Determined);
    assert(fixture.analyzer.output().rhythm_grid.status!=r::Status::Determined);
}
void invalid_is_atomic() {
    Fixture fixture;fixture.step(0);fixture.step(1);
    const auto history=fixture.analyzer.output().history_frames;
    fixture.frame.t_pub=-1;assert(!fixture.analyzer.process(fixture.frame));
    assert(fixture.analyzer.output().history_frames==history);
    fixture.frame.t_pub=fixture.frame.t_obs;fixture.frame.features=std::span{fixture.features}.first(63);
    assert(!fixture.analyzer.process(fixture.frame));assert(fixture.analyzer.output().history_frames==history);
    r::Config invalid;invalid.acf_window=513;assert(!fixture.analyzer.prepare(invalid));
    assert(fixture.analyzer.output().history_frames==history);
}
void explicit_empty_and_saturation() {
    Fixture fixture;fixture.frame.accents=std::span<const r::Accent>{};
    for(unsigned i=0;i<180;++i){fixture.features.fill(i%30==0?255:0);fixture.step(i);assert(!fixture.analyzer.output().event_count);}
    fixture.frame.saturated=true;fixture.step(180);
    assert(fixture.analyzer.output().reset==r::Reset::Saturated);assert(!fixture.analyzer.output().history_frames);
    assert(fixture.analyzer.output().tempo.status==r::Status::Unknown);
}
void packing() {
    std::array<float,4098> coefficients{};std::array<std::uint8_t,64> output{};r::PackingStatistics stats;
    coefficients[0]=10000;assert(r::pack_rfft(coefficients,output,stats));assert(stats.nonzero_bands==0);
    coefficients[4096]=1024;assert(r::pack_rfft(coefficients,output,stats));assert(output.back()==255);
    assert(stats.nonzero_bands==1);const auto before=output;
    coefficients[2]=std::numeric_limits<float>::quiet_NaN();assert(!r::pack_rfft(coefficients,output,stats));assert(output==before);
}
void normalized_amplitude_packing() {
    std::array<float,2048> amplitudes{};std::array<std::uint8_t,64> output{};r::PackingStatistics stats;
    amplitudes[2047]=.5f;assert(r::pack_peak_amplitudes(amplitudes,output,stats));
    assert(output.back()==128 && stats.nonzero_bands==1 && stats.clipped_bands==0);
    const auto before=output;amplitudes[0]=std::numeric_limits<float>::quiet_NaN();
    assert(!r::pack_peak_amplitudes(amplitudes,output,stats) && output==before);
    amplitudes[0]=0;amplitudes[2047]=1;
    std::array<std::uint8_t,96> other{};
    assert(r::pack_peak_amplitudes(amplitudes,other,stats));
    assert(other.back()==255 && stats.clipped_bands==1);
    std::array<float,4098> raw{};raw[4096]=1024;
    assert(r::pack_rfft(raw,output,stats) && output.back()==255);
}
void bounded_clock_span() {
    r::Analyzer analyzer;r::Config config;config.accent_horizon_s=1e9;config.max_event_age_s=1e9;
    assert(analyzer.prepare(config,r::Mode::Full));
    std::array<std::uint8_t,64> features{};
    std::array<r::Accent,2> accents{};
    r::Frame frame;frame.features=features;frame.sample_rate_hz=32768;frame.accents=accents;
    for(unsigned i=0;i<110;++i){
        features.fill(i%30==0?200:0);frame.sequence=i;frame.sample_index=std::uint64_t(i)*256*32768;
        frame.t_obs=1e18+double(i)*256;frame.t_ready=frame.t_obs;frame.t_pub=frame.t_obs;
        accents={r::Accent{frame.t_obs,.9,r::Kind::Bass,1,1},r::Accent{frame.t_obs-128,.9,r::Kind::Hat,2,2}};
        assert(analyzer.process(frame));
    }
    assert(analyzer.output().heavy_executed);
    assert(std::isfinite(analyzer.output().activity));
    assert(analyzer.output().tempo.status!=r::Status::Determined);
}
}
int main() {
    bounded_processing();invalid_is_atomic();explicit_empty_and_saturation();packing();normalized_amplitude_packing();bounded_clock_span();
    std::cout<<"rhythm workspace bytes="<<r::Analyzer::workspace_bytes()<<'\n';
}
