#include <dsp_kernels/spectral_autocorrelation.h>
#include <dsp_kernels/detail/source_spectral_math.h>
#include <dsp_kernels/detail/float_publication.h>
#include <algorithm>
#include <cmath>

namespace dsp_kernels {
bool PreparedSpectralAutocorrelation::configure_reference() {
    if(!reference_fft_.prepare(config_.fft_size))return false;
    const auto bins=config_.fft_size/2+1;
    reference_input_.resize(config_.fft_size);reference_complex_.resize(2*bins);reference_spectrum_.resize(2*bins);
    reference_result_.resize(config_.fft_size);reference_values_.resize(samples_);return true;
}
bool PreparedSpectralAutocorrelation::forward_reference(std::span<const double> input) noexcept {
    std::fill(reference_input_.begin(),reference_input_.end(),0.);
    std::copy(input.begin(),input.end(),reference_input_.begin());
    return reference_fft_.forward(reference_input_,reference_complex_);
}
bool PreparedSpectralAutocorrelation::compress_reference() noexcept {
    for(std::size_t k=0;k<reference_complex_.size()/2;++k){
        const double value=detail::spectral_power(reference_complex_[2*k],reference_complex_[2*k+1],double(config_.spectrum_divisor),double(config_.spectral_exponent));
        if(!std::isfinite(value))return false;
        reference_spectrum_[2*k]=value;reference_spectrum_[2*k+1]=0.;
    }
    return reference_fft_.inverse(reference_spectrum_,reference_result_);
}
bool PreparedSpectralAutocorrelation::collect_reference(std::size_t samples,std::size_t lags) noexcept {
    for(std::size_t lag=0;lag<lags;++lag){
        double value=reference_result_[lag];
        if(config_.inverse_output==AutocorrelationInverseOutput::ComplexMagnitude)value=std::abs(value);
        value*=double(config_.inverse_scale);
        if(config_.normalization==CorrelationNormalization::Unbiased)value/=double(samples-lag);
        if(config_.normalization==CorrelationNormalization::Biased)value/=double(samples);
        if(!std::isfinite(value))return false;
        reference_values_[lag]=value;
    }
    return true;
}
void PreparedSpectralAutocorrelation::publish_reference_spectrum() noexcept {
    for(std::size_t k=0;k<reference_spectrum_.size()/2;++k){
        spectrum_[k]=float(reference_spectrum_[2*k]);
        if(k && 2*k!=config_.fft_size)spectrum_[config_.fft_size-k]=spectrum_[k];
    }
    for(std::size_t k=0;k<reference_result_.size();++k){
        result_[2*k]=float(reference_result_[k]);result_[2*k+1]=0;
    }
}
bool PreparedSpectralAutocorrelation::publish_reference(std::size_t lags) noexcept {
    if(!detail::finite_float_range(reference_input_) || !detail::finite_float_range(reference_complex_) ||
       !detail::finite_float_range(reference_spectrum_) || !detail::finite_float_range(reference_result_) ||
       !detail::finite_float_range(std::span<const double>(reference_values_).first(lags)))return false;
    std::transform(reference_input_.begin(),reference_input_.end(),input_.begin(),[](double x){return float(x);});
    std::transform(reference_complex_.begin(),reference_complex_.end(),transformed_.begin(),[](double x){return float(x);});
    publish_reference_spectrum();
    std::transform(reference_values_.begin(),reference_values_.begin()+std::ptrdiff_t(lags),values_.begin(),[](double x){return float(x);});
    return true;
}
bool PreparedSpectralAutocorrelation::process_double(std::span<const double> input,std::span<double> output) noexcept {
    ready_=false;
    if(config_.precision!=NumericalPrecision::DesktopReference || input.empty() || input.size()>samples_ || output.size()>input.size())return false;
    if(!forward_reference(input) || !compress_reference() || !collect_reference(input.size(),output.size()))return false;
    if(!publish_reference(output.size()))return false;
    std::copy_n(reference_values_.begin(),output.size(),output.begin());
    active_samples_=input.size();active_lags_=output.size();ready_=true;return true;
}
bool PreparedSpectralAutocorrelation::process_reference(std::span<const float> input,std::span<float> output) noexcept {
    std::transform(input.begin(),input.end(),reference_values_.begin(),[](float x){return double(x);});
    const auto promoted=std::span(reference_values_).first(input.size());
    if(!process_double(promoted,std::span(reference_values_).first(output.size())))return false;
    std::copy_n(values_.begin(),output.size(),output.begin());return true;
}
}
