#include <dsp_kernels/onset_fusion.h>
#include <array>
#include <cassert>
#include <cstdlib>
#include <limits>
#include <new>

namespace {bool count_allocations=false;unsigned allocations=0;}
void* operator new(std::size_t size){if(count_allocations)++allocations;if(auto* p=std::malloc(size?size:1))return p;throw std::bad_alloc{};}
void operator delete(void* p) noexcept {std::free(p);}
void operator delete(void* p,std::size_t) noexcept {std::free(p);}
namespace {
using namespace dsp_kernels;
OnsetObservation peak(double observed,double ready,double strength=.8) {
    OnsetObservation value;value.peak=OnsetPeak{observed,ready,strength,.2};return value;
}
void waits_for_pending_peak_and_keeps_readiness() {
    PreparedOnsetFusion fusion;assert(fusion.prepare(2));
    std::array<OnsetObservation,2> values{peak(.1,.12,.5),{}};values[1].pending_t_obs=.115;
    assert(fusion.process(values,.12,.125) && fusion.output().empty());
    values={OnsetObservation{},peak(.115,.128,.9)};
    assert(fusion.process(values,.128,.13) && fusion.output().empty());
    values={};assert(fusion.process(values,.14,.145));
    assert(fusion.output().size()==1);const auto& event=fusion.output()[0];
    assert(event.channels==3 && event.peak.t_obs==.1 && event.peak.t_ready==.145 && event.peak.strength==.9f);
    assert(fusion.process(values,.2,.2) && fusion.output().empty());
}
void late_confirmation_remains_chronological() {
    PreparedOnsetFusion fusion;assert(fusion.prepare(2));
    std::array<OnsetObservation,2> values{OnsetObservation{},peak(.14,.16)};values[0].pending_t_obs=.1;
    assert(fusion.process(values,.16,.16) && fusion.output().empty());
    values[1]=peak(.24,.26);assert(fusion.process(values,.26,.26) && fusion.output().empty());
    values={peak(.1,.3),OnsetObservation{}};assert(fusion.process(values,.3,.31));
    const auto result=fusion.output();assert(result.size()==3);
    assert(result[0].peak.t_obs==.1 && result[1].peak.t_obs==.14 && result[2].peak.t_obs==.24);
    assert(result[0].peak.t_ready==.31 && result[1].peak.t_ready==.31 && result[2].peak.t_ready==.31);
}
void same_channel_repetition_is_not_a_duplicate() {
    PreparedOnsetFusion fusion;assert(fusion.prepare(2));
    std::array<OnsetObservation,2> values{peak(.1,.11),{}};values[1].pending_t_obs=.09;
    assert(fusion.process(values,.11,.11));
    values[0]=peak(.12,.13);assert(fusion.process(values,.13,.13));
    values={OnsetObservation{},peak(.09,.2)};assert(fusion.process(values,.2,.2));
    assert(fusion.output().size()==2);
    assert(fusion.output()[0].channels==3 && fusion.output()[0].peak.t_obs==.09);
    assert(fusion.output()[1].channels==1 && fusion.output()[1].peak.t_obs==.12);
}
void one_channel_is_immediate_and_invalid_input_is_atomic() {
    PreparedOnsetFusion fusion;assert(fusion.prepare(1));
    std::array values{peak(.1,.12)};assert(fusion.process(values,.12,.125));
    assert(fusion.output().size()==1 && fusion.output()[0].peak.t_ready==.125);
    assert(!fusion.prepare(9) && fusion.output().size()==1);
    values[0]=peak(.11,.14);assert(!fusion.process(values,.14,.14));
    assert(fusion.output().size()==1 && fusion.output()[0].peak.t_obs==.1);
    values[0]=peak(.2,.22);values[0].peak->strength=std::numeric_limits<double>::quiet_NaN();
    assert(!fusion.process(values,.22,.22) && fusion.output().size()==1);
    values[0]=peak(.2,.22);assert(fusion.process(values,.22,.23) && fusion.output().size()==1);
    fusion.reset();assert(fusion.output().empty());
}
void bounded_overflow_is_explicit_and_allocation_free() {
    PreparedOnsetFusion fusion;assert(fusion.prepare(2));
    std::array<OnsetObservation,2> values{};values[0].pending_t_obs=0;
    allocations=0;count_allocations=true;
    for(unsigned i=0;i<=max_pending_onsets;++i){
        const double observed=(i+1)*.1;values[1]=peak(observed-.05,observed);
        assert(fusion.process(values,observed,observed));
        assert(fusion.output().empty());
    }
    count_allocations=false;assert(allocations==0);
    assert(fusion.dropped()==max_pending_onsets+1);
    fusion.reset();assert(!fusion.dropped());
}
}
int main(){waits_for_pending_peak_and_keeps_readiness();late_confirmation_remains_chronological();same_channel_repetition_is_not_a_duplicate();one_channel_is_immediate_and_invalid_input_is_atomic();bounded_overflow_is_explicit_and_allocation_free();}
