#include <rhythm_reference_ports/beat_agreement.h>
#include <array>
#include <cstdio>
#include <cstdlib>
namespace {
void check(bool value,const char* message) {
    if(!value){std::fprintf(stderr,"agreement source regression: %s\n",message);std::exit(1);}
}
void check_pairwise_scores(std::span<const float> pairs) {
    for(std::size_t row=0;row<5;++row)for(std::size_t column=0;column<5;++column)
        check(pairs[row*5+column]==(row==column?0:5),"source pairwise score is not mathematically reinterpreted");
}

void identical_source_candidates() {
    rhythm_reference_ports::PreparedBeatAgreement agreement;
    check(agreement.prepare(5,4),"prepare source candidate geometry");
    const std::array<float,4> ticks{6,6.5f,7,7.5f};
    std::array<rhythm_reference_ports::BeatSequenceView,5> candidates;
    for(auto& candidate:candidates)candidate.seconds=ticks;
    rhythm_reference_ports::BeatAgreementSelection selected;
    check(agreement.process(candidates,selected),"compare identical candidates after source trimming");
    // Actual pinned TempoTapMaxAgreement: essentia::log2(int(40)) returns 5.
    // The native same-candidate PCM fixture also reports exactly 5, not 5.32.
    check(selected.selected==0 && selected.confidence==5,"retain source integer-log confidence and first selected method");
    for(float score:agreement.method_scores())check(score==5,"source mean information gain");
    check_pairwise_scores(agreement.pairwise_information_gain());
}
void negative_source_phase_wrap() {
    rhythm_reference_ports::PreparedBeatAgreement agreement;
    check(agreement.prepare(5,4),"prepare negative phase source fixture");
    const std::array<float,3> reference{6,7,8};
    const std::array<float,4> early{5,6,7,8};
    const std::array<rhythm_reference_ports::BeatSequenceView,5> candidates{{{reference},{early},{reference},{reference},{reference}}};
    rhythm_reference_ports::BeatAgreementSelection selected;
    check(agreement.process(candidates,selected),"compare an early tick before the reference interval");
    // Actual pinned TempoTapMaxAgreement: essentia::fmod(-.5,1)-.5 == 0.
    // Genuine original probe records confidence bits 0x40a00000 for these inputs.
    check(selected.selected==0 && selected.confidence==5,"negative full-period phase wraps to zero entropy");
    for(float score:agreement.method_scores())check(score==5,"preserve source agreement for negative phase wrapping");
}
}
int main(){identical_source_candidates();negative_source_phase_wrap();std::puts("Essentia original MaxAgreement confidence PASS");}
