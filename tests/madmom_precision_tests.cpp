#include <dsp_kernels/comb_filter_bank.h>
#include <dsp_kernels/convolution.h>
#include <dsp_kernels/histogram_tempo_peaks.h>
#include <dsp_kernels/interval_histogram.h>
#include <dsp_kernels/interval_sequence_search.h>
#include <dsp_kernels/local_interval_beat_search.h>
#include <dsp_kernels/axis_math.h>
#include <dsp_kernels/window_math.h>
#include <algorithm>
#include <bit>
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
void portable_default_unchanged() {
    IntervalHistogramConfig histogram;histogram.minimum_interval=2;histogram.maximum_interval=3;histogram.alpha=.5f;
    PreparedIntervalHistogram plan;const std::array<float,4> input{1,0,0,0};
    check(plan.prepare(input.size(),histogram) && plan.process_offline(input),"default Float32 histogram");
    check(plan.bins().size()==2 && plan.bins()[0]==1.5f && plan.bins()[1]==1.5f && plan.reference_bins().empty(),"Float32 recurrence and default storage preserved");

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

void axis_math_controls() {
    const std::array<float,3> rounded{1,1,1};const std::array<double,3> precise{1,1+1e-10,1};
    std::size_t index=9,count=0;std::array<std::uint64_t,3> indices{};
    check(first_maximum_index(std::span<const float>(rounded),index) && index==0,"first tied maximum index");
    check(ordered_wrapped_peaks(std::span<const double>(precise),std::span(indices),count) && count==1 && indices[0]==1,"Double peak topology preserved");
    check(ordered_wrapped_peaks(std::span<const float>(rounded),std::span(indices),count) && count==0,"strict plateau has no peak");
    const std::array<float,3> input{1e8f,1,-1e8f};std::array<float,3> sums{};
    check(sequential_axis_sum<float>(std::span<const float>(input),std::span(sums),true) && sums[2]==0,"Float32 sequential cumulative sum");
}
// Independent NumPy 1.26.4 native sum controls, including eight-lane,
// 128-value block and recursive split/tail boundaries.
void pairwise_float_controls() {
    std::array<float,257> input;input.fill(1.f);input[0]=1e8f;std::array<float,1> output{};
    constexpr std::array<std::size_t,9> lengths{0,1,7,8,16,127,128,129,257};
    constexpr std::array<std::uint32_t,9> bits{0,0x4cbebc20,0x4cbebc20,0x4cbebc20,0x4cbebc21,0x4cbebc2e,0x4cbebc2e,0x4cbebc2f,0x4cbebc3e};
    const auto before=allocations;
    for(std::size_t i=0;i<lengths.size();++i){
        const auto values=std::span<const float>(input).first(lengths[i]);
        check(axis_sum<float>(values,std::span(output),false,AxisSumAccumulation::Pairwise) && std::bit_cast<std::uint32_t>(output[0])==bits[i],"Float32 pairwise matches independent native bits");
        check(axis_sum<float>(values,std::span(output),false) && output[0]==(lengths[i]?1e8f:0.f),"default Float32 sequential sum is unchanged");
    }
    check(allocations==before,"pairwise Float32 reduction allocates nothing");
}
void pairwise_double_controls() {
    std::array<double,257> input;input.fill(1.);input[0]=1e16;std::array<double,1> output{};
    constexpr std::array<std::size_t,9> lengths{0,1,7,8,16,127,128,129,257};
    constexpr std::array<std::uint64_t,9> bits{0,0x4341c37937e08000ull,0x4341c37937e08000ull,0x4341c37937e08003ull,0x4341c37937e08007ull,0x4341c37937e08036ull,0x4341c37937e08038ull,0x4341c37937e0803cull,0x4341c37937e08078ull};
    const auto before=allocations;
    for(std::size_t i=0;i<lengths.size();++i){
        const auto values=std::span<const double>(input).first(lengths[i]);
        check(axis_sum<double>(values,std::span(output),false,AxisSumAccumulation::Pairwise) && std::bit_cast<std::uint64_t>(output[0])==bits[i],"Double pairwise matches independent native bits");
        check(axis_sum<double>(values,std::span(output),false) && output[0]==(lengths[i]?1e16:0.),"default Double sequential sum is unchanged");
    }
    check(allocations==before,"pairwise Double reduction allocates nothing");
}
void pairwise_rejects_invalid_inputs() {
    const std::array<double,3> nonfinite{1,std::numeric_limits<double>::infinity(),1};std::array<double,1> output{23};
    check(!axis_sum<double>(std::span<const double>(nonfinite),std::span(output),false,AxisSumAccumulation::Pairwise) && output[0]==23,"pairwise rejects nonfinite inputs without publication");
    const std::array<double,2> values{1,2},overflow{std::numeric_limits<double>::max(),std::numeric_limits<double>::max()};
    check(!axis_sum<double>(std::span<const double>(values),std::span(output),true,AxisSumAccumulation::Pairwise),"pairwise cumulative mode is invalid");
    check(!axis_sum<double>(std::span<const double>(values),std::span(output),false,AxisSumAccumulation(2)),"unknown accumulation mode is invalid");
    check(!axis_sum<double>(std::span<const double>(values),std::span(output).first(0),false,AxisSumAccumulation::Pairwise),"pairwise output must contain one reduction slot");
    check(!axis_sum<double>(std::span<const double>(overflow),std::span(output),false,AxisSumAccumulation::Pairwise) && output[0]==23,"pairwise overflow cannot publish a partial sum");
}
void pairwise_preserves_strict_peak() {
    std::array<double,16> input;input.fill(1.);input[0]=1e16;input.back()=0;
    std::array<double,3> pairwise{},sequential{};
    check(axis_sum<double>(std::span<const double>(input),std::span(pairwise).first(1),false,AxisSumAccumulation::Pairwise),"reduce neighboring histogram bin");
    check(axis_sum<double>(std::span<const double>(input),std::span(sequential).first(1),false),"reduce sequential neighbor");
    input.back()=1;
    check(axis_sum<double>(std::span<const double>(input),std::span(pairwise).subspan(1,1),false,AxisSumAccumulation::Pairwise),"reduce central histogram bin");
    check(axis_sum<double>(std::span<const double>(input),std::span(sequential).subspan(1,1),false),"reduce sequential center");
    pairwise[2]=pairwise[0];sequential[2]=sequential[0];
    std::array<std::uint64_t,3> indices{};std::size_t count=0;
    check(pairwise[0]==1e16+12 && pairwise[1]==1e16+14,"pairwise bins match independent native source sums");
    check(ordered_wrapped_peaks(std::span<const double>(pairwise),std::span(indices),count) && count==1 && indices[0]==1,"pairwise reduction retains the strict native histogram peak");
    check(ordered_wrapped_peaks(std::span<const double>(sequential),std::span(indices),count) && count==0,"sequential rounding produces a distinct plateau");
}
struct LocalOracle {
    PreparedIntervalHistogram histogram;std::array<float,3> kernel{};std::array<float,7> scratch{},axis{};
    static bool estimate(void* context,std::span<const float> values,std::size_t& interval) noexcept {
        auto& self=*static_cast<LocalOracle*>(context);float coordinate=0;
        if(!self.histogram.process_offline(values) || !dominant_histogram_coordinate(self.histogram.bins(),self.axis,self.kernel,self.scratch,coordinate))return false;
        interval=std::size_t(coordinate);return true;
    }
};
std::array<float,32> local_input() {
    std::array<float,32> input{};
    for(std::size_t i=0;i<input.size();++i){input[i]=.5f;for(std::size_t tone=1;tone<=5;++tone)input[i]+=.04f*std::sin(float(i*tone)*.31f);}
    return input;
}
void local_prefix_rows(std::span<const float> input,LocalOracle& oracle,std::span<std::size_t> intervals) {
    const std::array<std::size_t,7> delays{2,3,4,5,6,7,8};const std::array<float,1> alpha{.79f};
    PreparedCombFilterBank comb;check(comb.prepare(delays,alpha),"prepare shared comb response");
    TemporalWindowConfig config;config.length=64;config.hop=1;config.center=true;config.function=WindowFunction::Rectangular;
    PreparedTemporalWindow window;check(window.prepare(config),"prepare explicit centered contexts");
    std::array<float,7> response{},sum{};std::array<float,64> context{};
    for(std::size_t time=0;time<input.size()+31;++time){
        const float value=time<input.size()?input[time]:0;
        check(comb.process({&value,1},response),"shared comb response");
        const auto maximum=*std::max_element(response.begin(),response.end());
        for(std::size_t i=0;i<sum.size();++i)sum[i]+=response[i]==maximum?response[i]:0;
        if(time<31)continue;
        const auto prediction=time-31;check(window.read(input,prediction,context),"original centered context");
        check(oracle.histogram.process_offline(context) && std::equal(sum.begin(),sum.end(),oracle.histogram.bins().begin()),"prefix histogram equals each independently evaluated context");
        float coordinate=0;check(dominant_histogram_coordinate(sum,oracle.axis,oracle.kernel,oracle.scratch,coordinate),"explicit histogram smoothing and first argmax");
        intervals[prediction]=std::size_t(coordinate);
    }
}
void local_prefix_equivalence() {
    const auto input=local_input();IntervalHistogramConfig config;config.minimum_interval=2;config.maximum_interval=8;
    LocalOracle oracle;check(oracle.histogram.prepare(64,config) && symmetric_hamming_window(oracle.kernel),"prepare independent interval oracle");
    for(std::size_t i=0;i<oracle.axis.size();++i)oracle.axis[i]=float(i+2);
    std::array<std::size_t,32> intervals{};local_prefix_rows(input,oracle,intervals);
    PreparedLocalIntervalBeatSearch original,explicit_path;
    check(original.prepare(32,32,2,8) && explicit_path.prepare(32,32,2,8),"prepare independent local decoders");
    const auto before=allocations;
    check(original.process(input,IntervalEstimationPort{&oracle,LocalOracle::estimate}) && explicit_path.process(input,intervals),"adaptive local and explicit interval path");
    check(std::equal(original.positions().begin(),original.positions().end(),explicit_path.positions().begin(),explicit_path.positions().end()),"explicit prediction intervals preserve local beat path");
    check(allocations==before,"explicit local decoder has no hot allocations");
    check(!explicit_path.process(input,std::span(intervals).first(31)) && explicit_path.positions().empty(),"mismatched prediction interval count rejected");
    const auto short_input=std::span<const float>(input).first(5);std::array<std::size_t,5> short_intervals{};
    local_prefix_rows(short_input,oracle,short_intervals);
    check(original.process(short_input,IntervalEstimationPort{&oracle,LocalOracle::estimate}) && explicit_path.process(short_input,short_intervals),"short warmup uses one interval per actual prediction");
    check(std::equal(original.positions().begin(),original.positions().end(),explicit_path.positions().begin(),explicit_path.positions().end()),"short prefix construction preserves local beat path");

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
    portable_default_unchanged();axis_math_controls();local_prefix_equivalence();
    pairwise_float_controls();pairwise_double_controls();pairwise_rejects_invalid_inputs();pairwise_preserves_strict_peak();
    decoder_defined_outcomes();search_defined_outcomes();
    std::puts("madmom precision PASS");
}
