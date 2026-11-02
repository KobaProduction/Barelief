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
    explicit Fixture(const r::Config& config={}){assert(analyzer.prepare(config));frame.features=features;frame.sample_rate_hz=32768;}
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
void spectral_peaks_preserve_time_without_instrument_claims() {
    r::Config config;config.onset_channel_count=2;config.onset_channels[0]={0,32};config.onset_channels[1]={32,32};
    Fixture fixture(config);fixture.step(0);
    fixture.features.fill(200);fixture.step(1);assert(!fixture.analyzer.output().event_count);
    fixture.features.fill(0);fixture.step(2);
    assert(!fixture.analyzer.output().event_count && fixture.analyzer.output().onset_channel_count==2);
    assert(fixture.analyzer.output().onset_channels[0].peak->t_ready==2./60);
    fixture.step(3);const auto& result=fixture.analyzer.output();assert(result.event_count==1);
    assert(result.events[0].observation.t_obs==1./60 && result.events[0].t_ready==3./60);
    assert(result.events[0].observation.kind==r::Kind::Unknown && result.events[0].observation.group==0);
    assert(result.events[0].source_channels==3);
    assert(result.rhythm_grid.status!=r::Status::Determined);
    fixture.frame.accents=std::span<const r::Accent>{};fixture.features.fill(255);fixture.step(4);
    assert(!fixture.analyzer.output().event_count && !fixture.analyzer.output().onset_channel_count);
    fixture.frame.accents.reset();fixture.step(5);assert(!fixture.analyzer.output().event_count);
}
void assert_clipping_statistics(const r::Analyzer& analyzer,std::uint64_t frames,std::uint64_t values,float maximum) {
    const auto statistics=analyzer.clipping_statistics();
    assert(statistics.clipped_frames==frames);
    assert(statistics.clipped_values==values);
    assert(statistics.maximum_before_clamp==maximum);
}

void float_input_retains_weak_attacks_and_headroom() {
    Fixture fixture;std::array<float,64> linear{};fixture.frame.features=std::span<const float>{linear};fixture.step(0);
    assert_clipping_statistics(fixture.analyzer,0,0,0);
    linear.fill(.0005f);fixture.step(1);
    assert(fixture.analyzer.analysis().features[0]==.0005f);
    assert_clipping_statistics(fixture.analyzer,0,0,.0005f);
    linear.fill(0);fixture.step(2);assert(fixture.analyzer.output().event_count==1);
    for(unsigned i=3;i<10;++i)fixture.step(i);
    linear.fill(1.f);fixture.step(10);
    assert(fixture.analyzer.output().onset_channels[0].above_unit_values==0);
    assert(fixture.analyzer.output().onset_channels[0].mean_level==1);
    assert_clipping_statistics(fixture.analyzer,0,0,1);
    linear.fill(0);fixture.step(11);assert(fixture.analyzer.output().event_count==1);
    linear.fill(.0005f);linear[0]=1.33944046f;linear[1]=1.01f;fixture.step(12);
    const auto features=fixture.analyzer.analysis().features;
    assert(features[0]==1 && features[1]==1 && features[2]==.0005f);
    assert(linear[0]==1.33944046f && linear[1]==1.01f);
    assert(fixture.analyzer.output().onset_channels[0].above_unit_values==0);
    assert_clipping_statistics(fixture.analyzer,1,2,1.33944046f);
    fixture.step(13);assert_clipping_statistics(fixture.analyzer,2,4,1.33944046f);
}
void float_invalid_is_atomic_and_format_change_resets() {
    Fixture fixture;std::array<float,64> linear{};fixture.frame.features=std::span<const float>{linear};fixture.step(0);
    linear[0]=1.33944046f;
    for(const float invalid:{std::numeric_limits<float>::quiet_NaN(),std::numeric_limits<float>::infinity(),-1.f}){
        linear[1]=invalid;assert(!fixture.analyzer.process(fixture.frame));
        assert(fixture.analyzer.output().history_frames==1);
        assert_clipping_statistics(fixture.analyzer,0,0,0);
    }
    linear[1]=0;fixture.frame.features=std::span<const float>{linear}.first(63);
    assert(!fixture.analyzer.process(fixture.frame));assert_clipping_statistics(fixture.analyzer,0,0,0);
    fixture.frame.features=std::span<const float>{linear};fixture.frame.t_pub=-1;
    assert(!fixture.analyzer.process(fixture.frame));assert_clipping_statistics(fixture.analyzer,0,0,0);
    assert(fixture.analyzer.output().history_frames==1);
    fixture.frame.t_pub=fixture.frame.t_obs;linear[0]=0;fixture.step(1);
    fixture.frame.features=fixture.features;fixture.step(2);
    assert(fixture.analyzer.output().reset==r::Reset::Profile && fixture.analyzer.output().history_frames==1);
    fixture.frame.features=std::span<const float>{linear};allocations=0;observe_allocations=true;
    for(unsigned i=3;i<803;++i){linear.fill(i%30==0?.75f:0);fixture.step(i);}
    observe_allocations=false;assert(allocations==0);assert_clipping_statistics(fixture.analyzer,0,0,.75f);
}
void clipping_statistics_follow_prepared_instance() {
    Fixture fixture;std::array<float,64> linear{};linear[2]=2;fixture.frame.features=std::span<const float>{linear};fixture.step(0);
    fixture.analyzer.reset();assert_clipping_statistics(fixture.analyzer,1,1,2);
    linear.fill(0);fixture.step(10);fixture.frame.profile=1;fixture.step(11);
    assert(fixture.analyzer.output().reset==r::Reset::Profile);
    assert_clipping_statistics(fixture.analyzer,1,1,2);
    fixture.step(20);assert(fixture.analyzer.output().reset==r::Reset::Gap);
    fixture.frame.epoch=1;fixture.step(21);assert(fixture.analyzer.output().reset==r::Reset::Epoch);
    assert_clipping_statistics(fixture.analyzer,1,1,2);
    r::Analyzer copy;assert_clipping_statistics(copy,0,0,0);assert(copy.prepare());
    assert(copy.copy_state_from(fixture.analyzer));assert_clipping_statistics(copy,1,1,2);
    r::Config bad;bad.acf_window=513;assert(!fixture.analyzer.prepare(bad));
    assert_clipping_statistics(fixture.analyzer,1,1,2);
    assert(fixture.analyzer.prepare());assert_clipping_statistics(fixture.analyzer,0,0,0);
}

void observer_geometry_is_prepared_atomically() {
    Fixture fixture;fixture.step(0);const auto history=fixture.analyzer.output().history_frames;
    r::Config bad;bad.onset_channel_count=1;bad.onset_channels[0]={60,8};assert(!fixture.analyzer.prepare(bad));
    assert(fixture.analyzer.output().history_frames==history);
    std::array<std::uint8_t,96> other{};fixture.frame.features=other;
    assert(!fixture.analyzer.process(fixture.frame));assert(fixture.analyzer.output().history_frames==history);
    r::Config wide;wide.feature_count=96;assert(fixture.analyzer.prepare(wide));assert(fixture.analyzer.process(fixture.frame));
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
    bounded_processing();invalid_is_atomic();explicit_empty_and_saturation();packing();normalized_amplitude_packing();
    spectral_peaks_preserve_time_without_instrument_claims();observer_geometry_is_prepared_atomically();bounded_clock_span();
    float_input_retains_weak_attacks_and_headroom();float_invalid_is_atomic_and_format_change_resets();
    clipping_statistics_follow_prepared_instance();
    std::cout<<"rhythm workspace bytes="<<r::Analyzer::workspace_bytes()<<'\n';
}
