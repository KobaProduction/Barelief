#pragma once
#include <span>
#include <vector>

namespace dsp_kernels {
enum class GaussianCovariance {Diagonal,Spherical,Tied,Full};
struct GaussianMixtureParameters {
    std::size_t features=0,components=0;
    GaussianCovariance covariance=GaussianCovariance::Full;
    std::span<const float> weights,means,covariances;
};
// Owning cold model data; parameters() borrows only for prepare().
struct GaussianMixtureData {
    std::size_t features=0,components=0;
    GaussianCovariance covariance=GaussianCovariance::Full;
    std::vector<float> weights,means,covariances;
    GaussianMixtureParameters parameters() const noexcept {return {features,components,covariance,weights,means,covariances};}
};
// Classical GMM score/score_samples (madmom/sklearn source). No fitting/inference
// network/pickle/I/O. Cold prepare copies explicit numerical parameters.
// Means [component][feature]; covariances: diagonal [component][feature],
// spherical [component] or [component][feature], tied [feature][feature],
// full [component][feature][feature]. Source spherical 1D indexing defect is
// repaired; a supplied per-feature spherical row keeps all its source values.
// Diagonal density keeps source expanded x^2/mu^2/dot expression, not a
// rearranged squared-distance formula. Full/tied use shared Cholesky/solve and
// source ONE retry with +1e-7 diagonal. Weights are not silently normalized.
// Log-sum-exp and responsibilities Float32; no hot allocation.
class PreparedGaussianMixture {
public:
    static bool storage_bytes(GaussianMixtureParameters,float retry,std::size_t&) noexcept;
    std::size_t workspace_bytes() const noexcept;
    bool prepare(GaussianMixtureParameters,float full_diagonal_retry=1e-7f);
    bool process(std::span<const float> sample,float& log_probability,
                 std::span<float> responsibilities={}) noexcept;
    std::size_t features() const noexcept {return features_;}
    std::size_t components() const noexcept {return log_weights_.size();}
    std::span<const float> component_log_probabilities() const noexcept;
    std::span<const float> responsibilities() const noexcept;
private:
    bool configure(GaussianMixtureParameters,float);
    bool diagonal(GaussianMixtureParameters);
    bool covariance(std::span<const float>,std::span<float>,float);
    bool full(GaussianMixtureParameters,float);
    float diagonal_density(std::span<const float>,std::size_t) const noexcept;
    bool full_density(std::span<const float>,std::size_t,float&) noexcept;
    bool densities(std::span<const float>) noexcept;
    bool posterior(float) noexcept;
    std::size_t features_=0;
    GaussianCovariance covariance_=GaussianCovariance::Full;
    bool ready_=false;
    std::vector<float> means_,log_weights_,constants_,inverse_variance_,mean_over_variance_,lower_;
    std::vector<float> deviation_,solution_,log_components_,responsibilities_;
};
}
