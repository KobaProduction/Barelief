// Adapted from madmom ml/gmm.py at
// CPJKU/madmom@27f032e8947204902c675e5e341a3faf5dc86dae.
// Gaussian density code originates in sklearn.mixture.GMM,
// incorporated through sklearn 0650d5502e01e6b4245ce99729fc8e7a71aacff3.
// Authors: Ron Weiss, Fabian Pedregosa, Bertrand Thirion.
// BSD-style license: packaging/licenses/madmom.txt; sklearn notice shipped too.
#include <dsp_kernels/prepared_storage.h>
#include <dsp_kernels/gaussian_mixture.h>
#include <dsp_kernels/matrix_factorization.h>
#include <dsp_kernels/sequence_statistics.h>
#include <algorithm>
#include <cmath>
#include <limits>
#include <numbers>
#include <utility>

namespace dsp_kernels {
namespace {
constexpr float log_two_pi=1.8378770664093453f;
bool geometry(const GaussianMixtureParameters& p) noexcept {
    if(!p.features || !p.components || p.components>std::numeric_limits<std::size_t>::max()/p.features ||
       p.features>std::numeric_limits<std::size_t>::max()/p.features ||
       unsigned(p.covariance)>unsigned(GaussianCovariance::Full))return false;
    if(p.weights.size()!=p.components || p.means.size()!=p.features*p.components)return false;
    const auto diagonal=p.features*p.components,square=p.features*p.features;
    if(p.covariance==GaussianCovariance::Diagonal)return p.covariances.size()==diagonal;
    if(p.covariance==GaussianCovariance::Spherical)return p.covariances.size()==p.components || p.covariances.size()==diagonal;
    if(p.covariance==GaussianCovariance::Tied)return p.covariances.size()==square;
    return p.components<=std::numeric_limits<std::size_t>::max()/square && p.covariances.size()==p.components*square;
}
bool finite(std::span<const float> values) noexcept {
    return std::all_of(values.begin(),values.end(),[](float x){return std::isfinite(x);});
}
}
bool PreparedGaussianMixture::diagonal(GaussianMixtureParameters p) {
    inverse_variance_.resize(p.means.size());mean_over_variance_.resize(p.means.size());
    for(std::size_t component=0;component<p.components;++component){
        float log_determinant=0,mean_square=0;
        for(std::size_t feature=0;feature<features_;++feature){
            const auto at=component*features_+feature;
            const float variance=p.covariances[p.covariances.size()==p.components?component:at],mean=means_[at];
            if(variance<=0)return false;
            inverse_variance_[at]=1.f/variance;mean_over_variance_[at]=mean/variance;
            log_determinant+=std::log(variance);mean_square+=(mean*mean)/variance;
        }
        constants_[component]=float(features_)*log_two_pi+log_determinant+mean_square;
        if(!std::isfinite(constants_[component]))return false;
    }
    return finite(inverse_variance_) && finite(mean_over_variance_);
}
bool PreparedGaussianMixture::covariance(std::span<const float> input,std::span<float> output,float retry) {
    return cholesky_lower(input,features_,output) || cholesky_lower(input,features_,output,retry);
}
bool PreparedGaussianMixture::full(GaussianMixtureParameters p,float retry) {
    const auto square=features_*features_,matrices=p.covariance==GaussianCovariance::Tied?1:p.components;
    lower_.resize(matrices*square);
    for(std::size_t component=0;component<matrices;++component)
        if(!covariance(p.covariances.subspan(component*square,square),std::span(lower_).subspan(component*square,square),retry))return false;
    for(std::size_t component=0;component<p.components;++component){
        const auto matrix=p.covariance==GaussianCovariance::Tied?0:component;float sum=0;
        for(std::size_t feature=0;feature<features_;++feature)sum+=std::log(lower_[matrix*square+feature*features_+feature]);
        constants_[component]=float(features_)*log_two_pi+2.f*sum;
        if(!std::isfinite(constants_[component]))return false;
    }
    return true;
}
bool PreparedGaussianMixture::configure(GaussianMixtureParameters p,float retry) {
    if(!geometry(p) || !finite(p.means) || !finite(p.covariances) || !finite(p.weights) ||
       !std::all_of(p.weights.begin(),p.weights.end(),[](float x){return x>=0;}) ||
       std::none_of(p.weights.begin(),p.weights.end(),[](float x){return x>0;}) ||
       !std::isfinite(retry) || retry<0)return false;
    features_=p.features;covariance_=p.covariance;means_.assign(p.means.begin(),p.means.end());
    log_weights_.resize(p.components);constants_.resize(p.components);
    std::transform(p.weights.begin(),p.weights.end(),log_weights_.begin(),[](float w){return std::log(w);});
    deviation_.resize(features_);solution_.resize(features_);log_components_.resize(p.components);responsibilities_.resize(p.components);
    return (covariance_==GaussianCovariance::Diagonal || covariance_==GaussianCovariance::Spherical)?diagonal(p):full(p,retry);
}
bool PreparedGaussianMixture::prepare(GaussianMixtureParameters parameters,float retry) {
    std::size_t required=0;if(!storage_bytes(parameters,retry,required))return false;
    PreparedGaussianMixture next;
    if(!next.configure(parameters,retry))return false;
    if(next.workspace_bytes()>required)return false;
    *this=std::move(next);return true;
}
float PreparedGaussianMixture::diagonal_density(std::span<const float> sample,std::size_t component) const noexcept {
    float mean_dot=0,square_dot=0;
    for(std::size_t feature=0;feature<features_;++feature){
        const auto at=component*features_+feature;
        mean_dot+=sample[feature]*mean_over_variance_[at];
        square_dot+=(sample[feature]*sample[feature])*inverse_variance_[at];
    }
    return -.5f*(constants_[component]-2.f*mean_dot+square_dot);
}
bool PreparedGaussianMixture::full_density(std::span<const float> sample,std::size_t component,float& output) noexcept {
    const auto matrix=covariance_==GaussianCovariance::Tied?0:component,square=features_*features_;
    for(std::size_t feature=0;feature<features_;++feature)deviation_[feature]=sample[feature]-means_[component*features_+feature];
    if(!lower_triangular_solve(std::span<const float>(lower_).subspan(matrix*square,square),deviation_,solution_))return false;
    float sum=0;
    for(const float value:solution_)sum+=value*value;
    output=-.5f*(sum+constants_[component]);return std::isfinite(output);
}
bool PreparedGaussianMixture::densities(std::span<const float> sample) noexcept {
    const bool diagonal=covariance_==GaussianCovariance::Diagonal || covariance_==GaussianCovariance::Spherical;
    for(std::size_t component=0;component<components();++component){
        float value=0;
        if(diagonal)value=diagonal_density(sample,component);
        else if(!full_density(sample,component,value))return false;
        if(!std::isfinite(value))return false;
        log_components_[component]=value+log_weights_[component];
    }
    return true;
}
bool PreparedGaussianMixture::posterior(float probability) noexcept {
    for(std::size_t component=0;component<components();++component){
        responsibilities_[component]=std::exp(log_components_[component]-probability);
        if(!std::isfinite(responsibilities_[component]))return false;
    }
    return true;
}
bool PreparedGaussianMixture::process(std::span<const float> sample,float& output,std::span<float> responsibilities) noexcept {
    ready_=false;
    if(!features_ || sample.size()!=features_ || !finite(sample) ||
       (!responsibilities.empty() && responsibilities.size()!=components()) || !densities(sample))return false;
    float value=0;
    if(!log_sum_exp_float32(log_components_,value) || !posterior(value))return false;
    if(!responsibilities.empty())std::copy(responsibilities_.begin(),responsibilities_.end(),responsibilities.begin());
    output=value;ready_=true;return true;
}
std::span<const float> PreparedGaussianMixture::component_log_probabilities() const noexcept {
    return ready_?std::span<const float>(log_components_):std::span<const float>{};
}
std::span<const float> PreparedGaussianMixture::responsibilities() const noexcept {
    return ready_?std::span<const float>(responsibilities_):std::span<const float>{};
}
}


namespace dsp_kernels {
bool PreparedGaussianMixture::storage_bytes(GaussianMixtureParameters p,float retry,std::size_t& out) noexcept {
    if(!geometry(p) || !finite(p.means) || !finite(p.weights) || !finite(p.covariances) || !std::isfinite(retry) || retry<0 ||
       !std::all_of(p.weights.begin(),p.weights.end(),[](float x){return x>=0;}) ||
       std::none_of(p.weights.begin(),p.weights.end(),[](float x){return x>0;}))return false;
    std::size_t bytes=0;
    if(!prepared_storage::add_array(bytes,p.means.size(),sizeof(float)) ||
       !prepared_storage::add_array(bytes,p.components,4*sizeof(float)) || !prepared_storage::add_array(bytes,p.features,2*sizeof(float)))return false;
    const bool diagonal=p.covariance==GaussianCovariance::Diagonal || p.covariance==GaussianCovariance::Spherical;
    if(!prepared_storage::add_array(bytes,diagonal?p.means.size():p.covariances.size(),diagonal?2*sizeof(float):sizeof(float)))return false;
    out=bytes;return true;
}
std::size_t PreparedGaussianMixture::workspace_bytes() const noexcept {
    return (means_.capacity()+log_weights_.capacity()+constants_.capacity()+inverse_variance_.capacity()+mean_over_variance_.capacity()+lower_.capacity()+
        deviation_.capacity()+solution_.capacity()+log_components_.capacity()+responsibilities_.capacity())*sizeof(float);
}
}
