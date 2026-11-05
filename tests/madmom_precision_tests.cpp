#include <dsp_kernels/comb_filter_bank.h>
#include <dsp_kernels/convolution.h>
#include <dsp_kernels/crf_beat_sequence.h>
#include <dsp_kernels/window_math.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <new>
namespace {
using namespace dsp_kernels;
constexpr auto reference=NumericalPrecision::DesktopReference;
std::size_t allocations=0;
void check(bool value,const char* message) {
    if(!value){std::fprintf(stderr,"FAIL madmom precision: %s\n",message);std::exit(1);}
}
void double_window_convolution() {
    std::array<double,3> window{},output{};
    const std::array<double,3> input{1,2,3};
    check(symmetric_hamming_window(window),"Double Hamming");
    check(std::abs(window[0]-.08)<1e-15 && window[1]==1 && window[0]==window[2],"source centered Hamming");
    check(convolve_same(input,window,output),"Double convolution");
    check(std::abs(output[0]-1.16)<1e-14 && std::abs(output[1]-2.32)<1e-14 && std::abs(output[2]-3.16)<1e-14,"same crop arithmetic");
    check(!convolve_same(std::span<const double>(output),window,output),"overlapping output rejected");
    const std::array<double,2> short_input{2,3};
    check(convolve_same(short_input,window,output),"kernel longer than input");
    check(std::abs(output[0]-.16)<1e-14 && std::abs(output[1]-2.24)<1e-14 && std::abs(output[2]-3.16)<1e-14,"NumPy largest same crop");
    const std::array<double,1> invalid{std::numeric_limits<double>::infinity()};
    check(!convolve_same(invalid,window,output),"nonfinite input rejected");
}
void double_peak_topology() {
    const std::array<float,5> axis{1,2,3,4,5},rounded{0,1,1,1,0};
    const std::array<double,5> precise{0,1,1+1e-10,1,0};
    PreparedHistogramTempoPeaks portable,desktop;
    check(portable.prepare(axis,100) && desktop.prepare(axis,100,false,reference),"both peak policies prepare");
    const auto before=allocations;
    check(portable.process(rounded) && desktop.process(precise),"both peak policies process");
    check(allocations==before,"peak extraction has no hot allocations");
    check(std::isnan(portable.candidates()[0].tempo),"Float32 plateau keeps no-peak sentinel");
    check(desktop.reference_candidates().size()==1 && desktop.reference_candidates()[0].tempo==2000.,"Double strict maximum retained before publication");
    check(desktop.candidates()[0].tempo==2000.f && desktop.candidates()[0].strength==1.f,"Float32 publication after extraction");
    check(!desktop.process(rounded) && desktop.candidates().empty(),"precision mismatch invalidates output");
    check(!desktop.prepare(axis,100,true,reference) && desktop.process(precise),"unsupported interpolation retains prepared plan");
    const float tiny=std::numeric_limits<float>::denorm_min();
    const std::array<float,3> tiny_axis{tiny,2*tiny,3*tiny};const std::array<double,3> peak{0,1,0};
    check(desktop.prepare(tiny_axis,100,false,reference) && !desktop.process(peak) && desktop.candidates().empty(),"Float32 publication overflow rejects atomically");
}
void double_comb_boundary() {
    constexpr std::array<std::size_t,1> delays{1};constexpr std::array<float,1> alpha{.79f};
    const std::array<double,1> impulse{1},zero{0};std::array<double,1> value{};
    PreparedCombFilterBank plan;
    check(plan.prepare(delays,alpha,CombDirection::Backward,1,reference),"Double comb preparation");
    std::size_t bytes=0;
    check(PreparedCombFilterBank::storage_bytes(delays,alpha,CombDirection::Backward,1,reference,bytes) && plan.workspace_bytes()<=bytes,"Double comb pure storage bound");
    check(plan.process(impulse,value) && value[0]==1.,"impulse begins recurrence");
    auto clone=plan;const auto before=allocations;
    check(plan.process(zero,value) && value[0]==double(alpha[0]) && value[0]!=.79,"Cython float alpha promoted to Double");
    check(plan.process(zero,value) && value[0]==double(alpha[0])*double(alpha[0]),"Double recurrence history");
    check(clone.process(zero,value) && value[0]==double(alpha[0]),"clone owns independent history");
    plan.reset();check(plan.process(zero,value) && value[0]==0,"reset clears reference state");
    check(allocations==before,"Double comb processing/reset do not allocate");
    bytes=123;
    check(!PreparedCombFilterBank::storage_bytes(2,std::numeric_limits<std::size_t>::max(),CombDirection::Backward,1,reference,bytes) && bytes==123,"failed storage query leaves output unchanged");
}
void double_histogram() {
    IntervalHistogramConfig config;config.minimum_interval=2;config.maximum_interval=3;config.alpha=.5f;config.precision=reference;
    PreparedIntervalHistogram plan;const std::array<double,4> input{1,0,0,0};
    check(plan.prepare(input.size(),config),"Double histogram prepare");
    const auto before=allocations;
    check(plan.process_offline(input),"Double histogram process");
    check(plan.reference_bins().size()==2 && plan.reference_bins()[0]==1.5 && plan.reference_bins()[1]==1.5,"every tied comb maximum retained");
    check(allocations==before,"Double histogram no hot allocations");
    check(plan.bins().empty() && plan.channel_bins(0).empty(),"Double storage has no implicit narrowed histogram");
    config.method=IntervalHistogramMethod::Autocorrelation;
    check(!plan.prepare(input.size(),config) && plan.process_offline(input),"unsupported Double ACF preserves prepared plan");
}
TempoAnalysisSequenceConfig reference_config() {
    TempoAnalysisSequenceConfig config;config.precision=reference;
    config.histogram.frame_rate=4;config.histogram.minimum_bpm=60;config.histogram.maximum_bpm=120;
    config.activation_smoothing_seconds=0;config.histogram_smoothing=0;return config;
}
void reference_composition_lifecycle() {
    auto config=reference_config();PreparedTempoAnalysisSequence plan;
    std::array<float,32> input{};input[0]=1;input[4]=1;input[8]=1;
    std::size_t bound=0;
    check(PreparedTempoAnalysisSequence::storage_bytes(input.size(),config,{},{},{},bound) && plan.prepare(input.size(),config),"reference composition pure query and preparation");
    check(plan.workspace_bytes()<=bound,"reference composition actual capacity within query");
    auto clone=plan;const auto before=allocations;
    check(plan.process(input) && clone.process(input),"independent prepared copies process");
    check(std::equal(plan.raw_histogram().begin(),plan.raw_histogram().end(),clone.raw_histogram().begin()),"cloned histogram equivalence");
    plan.reset();check(plan.candidates().empty() && plan.raw_histogram().empty() && !clone.candidates().empty(),"reset clears publication independently");
    check(plan.process(input) && allocations==before,"reference replay no hot allocations");
    config.histogram.online=true;
    check(!plan.prepare(input.size(),config) && plan.process(input),"unsupported reference online prepare atomic");
    check(!plan.process_online(input) && plan.candidates().empty(),"online call explicitly rejected");
    config=reference_config();config.interpolate=true;bound=456;
    check(!PreparedTempoAnalysisSequence::storage_bytes(input.size(),config,{},{},{},bound) && bound==456,"unsupported reference query atomic");
}
void explicit_identity_windows() {
    auto config=reference_config();config.activation_smoothing_seconds=.25f;config.histogram_smoothing=1;
    const std::array<float,1> identity{1};std::array<float,32> input{};input[0]=1;input[4]=.5f;
    std::size_t bound=123;
    check(!PreparedTempoAnalysisSequence::storage_bytes(input.size(),config,{},{},{},bound) && bound==123,"integer Hamming length1 is invalid");
    check(!PreparedTempoAnalysisSequence::storage_bytes(input.size(),config,identity,{},{},bound),"explicit activation does not override invalid histogram recipe");
    check(!PreparedTempoAnalysisSequence::storage_bytes(input.size(),config,{},identity,{},bound),"explicit histogram does not override invalid activation recipe");
    PreparedTempoAnalysisSequence plan,baseline;
    check(PreparedTempoAnalysisSequence::storage_bytes(input.size(),config,identity,identity,{},bound) &&
        plan.prepare(input.size(),config,identity,identity),"explicit one-coefficient kernels are valid");
    check(plan.workspace_bytes()<=bound && baseline.prepare(input.size(),reference_config()),"identity resources and unsmoothed baseline");
    const auto before=allocations;
    check(plan.process(input) && baseline.process(input),"explicit identity composition processes");
    check(std::equal(plan.raw_histogram().begin(),plan.raw_histogram().end(),baseline.raw_histogram().begin()) &&
        std::equal(plan.smoothed_histogram().begin(),plan.smoothed_histogram().end(),baseline.raw_histogram().begin()),"explicit identity preserves activation and histogram");
    check(allocations==before,"identity kernels need no hot allocation");
    check(!plan.prepare(input.size(),config) && plan.process(input),"integer rejection preserves prepared explicit kernels");
}
void portable_default_unchanged() {
    check(TempoAnalysisSequenceConfig{}.precision==NumericalPrecision::PortableFloat32,"portable is default policy");
    IntervalHistogramConfig histogram;histogram.minimum_interval=2;histogram.maximum_interval=3;histogram.alpha=.5f;
    PreparedIntervalHistogram plan;const std::array<float,4> input{1,0,0,0};
    check(plan.prepare(input.size(),histogram) && plan.process_offline(input),"default Float32 histogram");
    check(plan.bins().size()==2 && plan.bins()[0]==1.5f && plan.bins()[1]==1.5f && plan.reference_bins().empty(),"Float32 recurrence and default storage preserved");
    CrfBeatSequenceConfig config;config.tempo=reference_config();config.use_factors=true;
    std::size_t bytes=77;
    check(!PreparedCrfBeatSequence::storage_bytes(input.size(),config,default_beat_interval_factors,bytes) && bytes==77,"reference factors explicitly unsupported");
}
void decoder_defined_outcomes() {
    PreparedIntervalSequenceDecoder plan;std::array<float,160> input{};input.fill(.2f);
    check(plan.prepare(input.size(),24,100),"prepare decoder outcomes");const auto before=allocations;
    check(plan.process(input,24) && plan.outcome()==SequenceDecodeOutcome::Path && !plan.positions().empty(),"defined decoder path");
    input.fill(0);
    check(!plan.process(input,50) && plan.outcome()==SequenceDecodeOutcome::NoPath,"zero support is NoPath");
    check(plan.positions().empty() && std::isinf(plan.log_probability()) && plan.log_probability()<0 && !plan.error_reason(),"NoPath clears path and score");
    check(!plan.process(std::span(input).first(8),50) && plan.outcome()==SequenceDecodeOutcome::NoPath,"short valid input is NoPath");
    input[0]=std::numeric_limits<float>::quiet_NaN();
    check(!plan.process(input,50) && plan.outcome()==SequenceDecodeOutcome::Error && plan.error_reason(),"NaN is Error, not NoPath");
    input[0]=-.1f;check(!plan.process(input,50) && plan.outcome()==SequenceDecodeOutcome::Error,"negative activation is Error");
    input.fill(.2f);check(plan.process(input,24) && !plan.error_reason(),"valid decoder recovers after invalid input");
    check(!plan.process(input,0) && plan.outcome()==SequenceDecodeOutcome::Error && plan.positions().empty(),"invalid interval clears prior path");
    const std::array<float,161> oversized{};
    check(!plan.process(oversized,50) && plan.outcome()==SequenceDecodeOutcome::Error,"oversized activation is Error");
    check(allocations==before,"decoder outcomes need no hot allocation");
}
void search_defined_outcomes() {
    PreparedIntervalSequenceSearch plan;std::array<float,160> input{};
    std::fill(input.begin()+84,input.begin()+107,.5f);const std::array<std::size_t,2> intervals{75,100};
    check(plan.prepare(input.size(),50,100,2),"prepare independent candidate search");const auto before=allocations;
    check(plan.process(input,intervals) && plan.outcome()==SequenceDecodeOutcome::Path,"mixed valid and unreachable trials retain winner");
    check(plan.trial_intervals()[0]==100 && plan.trial_intervals()[1]==75,"source descending trial order");
    check(std::isfinite(plan.normalized_scores()[0]) && std::isinf(plan.normalized_scores()[1]) && plan.normalized_scores()[1]<0,"unreachable trial keeps negative infinity");
    input.fill(0);
    check(!plan.process(input,intervals) && plan.outcome()==SequenceDecodeOutcome::NoPath && plan.positions().empty(),"all unreachable trials are NoPath");
    check(plan.trial_intervals().size()==2 && std::all_of(plan.normalized_scores().begin(),plan.normalized_scores().end(),[](float x){return std::isinf(x) && x<0;}),"all-trial diagnostics are current");
    input[90]=std::numeric_limits<float>::infinity();
    check(!plan.process(input,intervals) && plan.outcome()==SequenceDecodeOutcome::Error,"invalid activations are not skipped as no-path trials");
    check(plan.positions().empty() && plan.trial_intervals().empty() && plan.normalized_scores().empty() && plan.error_reason(),"failed search exposes no stale diagnostics");
    check(allocations==before,"search outcomes allocate nothing");
}
CrfBeatSequenceConfig no_path_config() {
    CrfBeatSequenceConfig config;config.tempo.precision=reference;
    config.tempo.histogram.frame_rate=200;config.tempo.histogram.minimum_bpm=160;config.tempo.histogram.maximum_bpm=240;
    config.tempo.activation_smoothing_seconds=.09f;config.tempo.histogram_smoothing=7;return config;
}
void crf_defined_outcomes() {
    PreparedCrfBeatSequence plan;auto config=no_path_config();std::array<float,161> input{};input.fill(.2f);
    check(plan.prepare(input.size(),config),"prepare CRF outcome lifecycle");const auto before=allocations;
    check(plan.process(input) && plan.outcome()==SequenceDecodeOutcome::Path && !plan.events().empty(),"positive CRF path");
    input.fill(0);std::fill(input.begin()+92,input.begin()+98,.5f);
    check(plan.process(input) && plan.outcome()==SequenceDecodeOutcome::NoPath && plan.events().empty() && plan.positions().empty(),"no initial support is successful empty CRF");
    check(!plan.trial_intervals().empty() && std::all_of(plan.normalized_scores().begin(),plan.normalized_scores().end(),[](float x){return std::isinf(x) && x<0;}),"no-path CRF publishes honest attempted trials");
    input.fill(0);check(plan.process(input) && plan.outcome()==SequenceDecodeOutcome::NoPath,"no-tempo silence is successful empty CRF");
    check(plan.events().empty() && plan.trial_intervals().empty() && plan.normalized_scores().empty(),"no-tempo clears prior trial diagnostics");
    input[0]=std::numeric_limits<float>::quiet_NaN();
    check(!plan.process(input) && plan.outcome()==SequenceDecodeOutcome::Error && plan.error_reason(),"invalid CRF input stays Error");
    check(plan.events().empty() && plan.positions().empty() && plan.tempo_candidates().empty(),"invalid CRF clears all public outputs");
    check(plan.process({}) && plan.outcome()==SequenceDecodeOutcome::NoPath && plan.events().empty(),"empty finite activation has no path");
    check(allocations==before,"CRF outcome lifecycle allocates nothing");
}
void crf_short_capacity() {
    PreparedCrfBeatSequence plan;const auto config=no_path_config();std::size_t bytes=0;
    const std::array<float,1> short_input{.5f};
    check(PreparedCrfBeatSequence::storage_bytes(1,config,default_beat_interval_factors,bytes) && plan.prepare(1,config),"cold short CRF owns model scratch without input padding");
    check(plan.workspace_bytes()<=bytes,"short query covers actual scratch capacity");const auto before=allocations;
    check(plan.process(short_input) && plan.outcome()==SequenceDecodeOutcome::NoPath && plan.events().empty(),"short actual input remains short");
    check(allocations==before,"short CRF allocates nothing during processing");
    PreparedIntervalSequenceDecoder overflow;
    check(overflow.prepare(160,24,100,std::numeric_limits<float>::denorm_min()),"tiny sigma is declared explicitly");
    std::array<float,160> input{};input.fill(.2f);
    check(!overflow.process(input,24) && overflow.outcome()==SequenceDecodeOutcome::Error && overflow.error_reason(),"nonfinite distribution is numeric Error, not missing evidence");
}
}
void* operator new(std::size_t bytes) {++allocations;if(auto* p=std::malloc(bytes?bytes:1))return p;throw std::bad_alloc{};}
void* operator new[](std::size_t bytes) {return ::operator new(bytes);}
void operator delete(void* p) noexcept {std::free(p);}
void operator delete[](void* p) noexcept {::operator delete(p);}
void operator delete(void* p,std::size_t) noexcept {::operator delete(p);}
void operator delete[](void* p,std::size_t) noexcept {::operator delete(p);}
int main() {
    double_window_convolution();double_peak_topology();double_comb_boundary();double_histogram();
    reference_composition_lifecycle();explicit_identity_windows();portable_default_unchanged();
    decoder_defined_outcomes();search_defined_outcomes();crf_defined_outcomes();crf_short_capacity();
    std::puts("madmom precision PASS");
}
