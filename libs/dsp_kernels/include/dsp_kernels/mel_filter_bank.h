#pragma once
#include <cstddef>
#include <dsp_kernels/filter_projection.h>
#include <dsp_kernels/frequency_bins.h>
#include <cstdint>
#include <span>
#include <vector>

namespace dsp_kernels {
enum class MelScale { Htk, Slaney };
enum class MelWeighting { Frequency, Scale };
enum class MelNormalization { Slaney, None, L0, Lp, Maximum, Minimum, TriangleArea };
enum class MelConstruction {Continuous,NearestBinTriangles};
struct MelFilterBankConfig {
    double sample_rate=48000,bin_hz=48000./4096,fmin_hz=0,fmax_hz=0,norm_p=2;
    std::size_t bands=50;
    std::uint64_t first_bin=1;
    MelScale scale=MelScale::Htk;
    MelNormalization normalization=MelNormalization::None;
    MelWeighting weighting=MelWeighting::Frequency;
    // Discrete construction snaps/optionally uniques Mel boundaries, then uses
    // shared bin-index triangles. Its normalization is None or Float32 L1.
    MelConstruction construction=MelConstruction::Continuous;
    bool unique_filters=true;
};
// Linear triangular projection of supplied spectral values.
// Triangle slopes are explicit: Frequency (librosa) or Scale (warped Mel).
// Slaney normalization uses Hz area; TriangleArea uses the selected slope axis. The unit is
// preserved by default. Explicit-weight preparation can request squared input
// and plain accumulation. No FFT, logarithm, quantization or history is hidden.
// Frequencies are explicit or (bin+first_bin)*bin_hz; fmax_hz=0 selects Nyquist.
// Preparation may allocate and is atomic. Processing uses per-instance scratch,
// never allocates, and preserves the output on failure; calls are serialized.
// Projection products and compensated accumulation are Float32. The frequency
// grid and cold filter construction retain coordinate precision separately.
class MelFilterBank {
public:
    bool prepare(std::size_t bins,const MelFilterBankConfig&,std::span<const double> frequencies_hz={});
    // Uniform DC-inclusive grid, HTK triangles without area normalization.
    bool prepare(std::size_t bins,std::size_t sample_rate,std::size_t bands,std::span<const double> frequencies_hz={});
    // Generic explicit-weight preparation: band-major coefficients and real
    // triangle boundary coordinates (bands+2). Optional support restricts the
    // evaluated bins, preserving endpoint/overflow semantics of source recipes.
    bool prepare_weights(std::size_t bins,std::span<const float> weights,std::span<const double> boundaries_hz,
                         FilterProjectionOptions={},std::span<const FilterBandSupport> supports={});
    // General band matrix, with optional explicit center coordinates. Empty
    // coordinates remain unavailable; no artificial triangular edges are made.
    bool prepare_matrix(std::size_t bins,std::size_t bands,std::span<const float> weights,
                        std::span<const double> centers_hz={},FilterProjectionOptions={},
                        std::span<const FilterBandSupport> supports={});
    bool process(std::span<const float>,std::span<float>) const noexcept;
    double center_hz(std::size_t band) const noexcept {return band<centers_.size()?centers_[band]:0;}
    std::span<const float> weights() const noexcept {return weights_;}
    std::span<const double> centers_hz() const noexcept {return centers_;}
    std::size_t workspace_bytes() const noexcept {return (boundaries_.capacity()+centers_.capacity())*sizeof(double)+(weights_.capacity()+staging_.capacity())*sizeof(float)+supports_.capacity()*sizeof(FilterBandSupport);}
    // Legacy total includes sizeof(MelFilterBank); nested dynamic-only users
    // subtract that header once or use matrix_storage_bytes below.
    static bool storage_bytes(std::size_t bins,const MelFilterBankConfig&,std::size_t&) noexcept;
    // Dynamic allocation payload only; includes the common prepared boundary,
    // support, center and staging capacities, even with no supplied centers.
    static bool matrix_storage_bytes(std::size_t bins,std::size_t bands,std::size_t&) noexcept;
    static bool band_count(FrequencyGridView,const MelFilterBankConfig&,std::size_t&) noexcept;
private:
    void allocate_storage(std::size_t,const MelFilterBankConfig&);
    bool build_filters(std::span<const double>);
    bool build_binned_filters(std::span<const double>);
    bool bind_binned_filters(std::span<const std::size_t>,std::span<const double>);
    bool project_band(std::span<const float>,std::size_t,float&) const noexcept;
    void build_boundaries();
    bool valid_boundaries() const noexcept;
    void build_filter(std::size_t,std::span<const double>);
    double coordinate(double hz) const noexcept;
    void normalize_filter(std::size_t);
    double filter_norm(std::span<const float>) const noexcept;
    MelFilterBankConfig config_;
    std::size_t bins_=0;
    std::vector<double> boundaries_,centers_;
    std::vector<float> weights_;
    std::vector<FilterBandSupport> supports_;
    FilterProjectionOptions projection_;
    mutable std::vector<float> staging_;
};
}
