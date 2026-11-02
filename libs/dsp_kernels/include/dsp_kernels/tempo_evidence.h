#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace dsp_kernels::rhythm {
struct TempoEvidenceConfig {
    unsigned frame_rate_fps=60;
    std::size_t short_fft=512,long_fft=4096;
    float min_bpm=40,max_bpm=240;
    float min_observation_s=4,mature_observation_s=8;
};
struct PeriodicityCurve {
    std::span<const float> fourier,autocorrelation;
    std::size_t filled=0;
};
struct TempoEvidenceCandidate {
    float bpm=0,score=0;
    float fourier_short=0,fourier_long=0,acf_short=0,acf_long=0;
    std::uint8_t origin_curves=0;
};
struct TempoEvidenceOutput {
    std::array<TempoEvidenceCandidate,8> candidates{};
    std::size_t count=0,proposals=0,truncated_proposals=0;
    float maturity=0;
    bool harmonic_ambiguity=false;
};
// Stateless evidence extraction from explicit external histories. No FFT,
// history retention, labels or decision about a confirmed beat grid. Components
// are correlated measurements of one source; score is not a probability.
class PreparedTempoEvidence {
public:
    bool prepare(const TempoEvidenceConfig& = {}) noexcept;
    bool process(const PeriodicityCurve& short_curve,const PeriodicityCurve& long_curve) noexcept;
    const TempoEvidenceOutput& output() const noexcept {return output_;}
    std::size_t workspace_bytes() const noexcept {return sizeof(*this);}
private:
    struct Proposal {float bpm=0;std::uint8_t origin=0;};
    TempoEvidenceConfig config_{};
    TempoEvidenceOutput output_{};
    std::array<Proposal,288> proposals_{};
    std::size_t proposal_count_=0;
    bool prepared_=false;
    void propose(float,std::uint8_t) noexcept;
    void collect(const PeriodicityCurve&,std::size_t,std::uint8_t) noexcept;
    void rank(const PeriodicityCurve&,const PeriodicityCurve&) noexcept;
};
}
