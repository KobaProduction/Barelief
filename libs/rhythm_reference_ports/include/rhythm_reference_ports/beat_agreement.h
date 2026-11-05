// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include <cstddef>
#include <limits>
#include <span>
#include <vector>
namespace rhythm_reference_ports {
struct BeatSequenceView {std::span<const float> seconds;};
struct BeatAgreementSelection {
    std::size_t selected=std::numeric_limits<std::size_t>::max();
    float confidence=0;
};
// Source port of Essentia TempoTapMaxAgreement. Selects an ORIGINAL candidate
// sequence by pairwise phase-error information gain. Confidence is not a
// calibrated probability. Only comparison discards the first5seconds; the
// selected output index refers to the caller's complete untrimmed sequence.
// The pinned source's int-returning log2(40) is 5. Its information-gain score
// subtracts phase entropy from that integer maximum; it is not clamped.
class PreparedBeatAgreement {
public:
    // Dynamic owned/cold-preparation upper bound; excludes this object.
    static bool storage_bytes(std::size_t maximum_methods,std::size_t maximum_beats,float minimum_time,std::size_t& bytes) noexcept;
    std::size_t workspace_bytes() const noexcept;
    bool prepare(std::size_t maximum_methods,std::size_t maximum_beats,float minimum_time=5);
    bool process(std::span<const BeatSequenceView>,BeatAgreementSelection&) noexcept;
    std::span<const float> method_scores() const noexcept {return std::span(scores_).first(methods_);}
    std::span<const float> pairwise_information_gain() const noexcept {return std::span(pairwise_).first(methods_*methods_);}
private:
    bool fill_histogram(std::span<const float>,std::span<const float>) noexcept;
    BeatAgreementSelection select_method() noexcept;
    bool validate(std::span<const BeatSequenceView>) const noexcept;
    std::span<const float> trim(std::span<const float>) const noexcept;
    float entropy(std::span<const float> reference,std::span<const float> candidate) noexcept;
    float error(std::span<const float> reference,float time) const noexcept;
    std::size_t closest(std::span<const float>,float time) const noexcept;
    bool pairs(std::span<const BeatSequenceView>) noexcept;
    float method_score(std::size_t method) const noexcept;
    float information_gain(std::span<const float>,std::span<const float>) noexcept;
    void boundaries();
    std::size_t maximum_methods_=0,maximum_beats_=0,methods_=0;
    float minimum_time_=5;
    std::vector<float> boundaries_,histogram_,scores_,pairwise_;
};
}
