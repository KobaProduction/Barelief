#include <dsp_kernels/spectral_autocorrelation.h>
#include <dsp_kernels/prepared_storage.h>
#include <dsp_kernels/detail/source_spectral_math.h>
#include <algorithm>
#include <bit>
#include <cmath>
#include <limits>
#include <utility>

namespace dsp_kernels {
namespace {
std::size_t power_two(std::size_t minimum) noexcept {
    std::size_t size=1;
    while(size<minimum){
        if(size>std::numeric_limits<std::size_t>::max()/2)return 0;
        size*=2;
    }
    return size;
}
std::size_t smooth_candidate(std::size_t minimum,std::size_t factor) noexcept {
    const auto multiplier=power_two(minimum/factor+std::size_t(minimum%factor!=0));
    return multiplier && factor<=std::numeric_limits<std::size_t>::max()/multiplier?factor*multiplier:0;
}
// Independent enumeration of the real-transform 2/3/5-smooth length rule.
// Pinned reference: scipy/pocketfft@9367142748fcc9696a1c9e5a99b76ed9897c9daa,
// pocketfft_hdronly.h good_size_real. Enumeration runs only during preparation.
std::size_t smooth_length(std::size_t minimum) noexcept {
    auto best=power_two(minimum);
    for(std::size_t fifth=1;fifth<=best;){
        for(auto third=fifth;third<=best;){
            const auto candidate=smooth_candidate(minimum,third);
            if(candidate && candidate<best)best=candidate;
            if(third>best/3)break;
            third*=3;
        }
        if(fifth>best/5)break;
        fifth*=5;
    }
    return best;
}
bool valid_parameters(const SpectralAutocorrelationConfig& config) noexcept {
    return std::isfinite(config.spectrum_divisor) && config.spectrum_divisor>0 &&
        std::isfinite(config.spectral_exponent) && config.spectral_exponent>0 &&
        std::isfinite(config.inverse_scale) && config.inverse_scale>0 &&
        config.normalization>=CorrelationNormalization::Raw && config.normalization<=CorrelationNormalization::Biased &&
        unsigned(config.inverse_output)<=unsigned(AutocorrelationInverseOutput::ComplexMagnitude) &&
        unsigned(config.transform)<=unsigned(AutocorrelationTransform::Real) &&
        unsigned(config.precision)<=unsigned(NumericalPrecision::DesktopReference) &&
        (config.precision!=NumericalPrecision::DesktopReference || config.transform!=AutocorrelationTransform::Complex) &&
        unsigned(config.length)<=unsigned(AutocorrelationFftLength::Smooth235);
}
bool native_real(const SpectralAutocorrelationConfig& config) noexcept {
    return config.transform!=AutocorrelationTransform::Complex && config.fft_size>=8 &&
        config.fft_size<=max_fft_samples && !(config.fft_size&(config.fft_size-1));
}
bool backend_storage(std::size_t size,const FourierBackendFactory& factory,std::size_t& bytes) noexcept {
    FourierBackendResources forward,inverse;
    if(!PreparedFourierBackend::query({size,FourierBackendTransform::RealForward},factory,forward) ||
       !PreparedFourierBackend::query({size,FourierBackendTransform::RealInverse},factory,inverse))return false;
    return prepared_storage::add_bytes(bytes,forward.cold_peak_bytes) && prepared_storage::add_bytes(bytes,inverse.cold_peak_bytes) &&
        prepared_storage::add_array(bytes,2*size+2,sizeof(float));
}
bool primary_transform_storage(const SpectralAutocorrelationConfig& config,const FourierBackendFactory* factory,std::size_t& bytes) noexcept {
    const bool reference=config.precision==NumericalPrecision::DesktopReference;
    if(reference && factory)return false;
    if(factory && config.transform!=AutocorrelationTransform::Complex)return backend_storage(config.fft_size,*factory,bytes);
    return reference?detail::PreparedReferenceFourier::storage_bytes(config.fft_size,bytes):
        PreparedFourierTransform::storage_bytes(config.fft_size,bytes,factory);
}
bool auxiliary_transform_storage(std::size_t samples,const SpectralAutocorrelationConfig& config,const FourierBackendFactory* factory,std::size_t& bytes) noexcept {
    const bool reference=config.precision==NumericalPrecision::DesktopReference;
    if(reference && (!prepared_storage::add_array(bytes,config.fft_size,4*sizeof(double)) ||
       !prepared_storage::add_array(bytes,samples+4,sizeof(double))))return false;
    std::size_t nested=0;
    if(!reference && !factory && native_real(config)){
        if(!PreparedRfft::storage_bytes(config.fft_size,false,nested) || !prepared_storage::add_bytes(bytes,nested))return false;
    }
    return true;
}
bool variable_plan_storage(std::size_t maximum,SpectralAutocorrelationConfig config,const FourierBackendFactory* factory,std::size_t& bytes) noexcept {
    std::size_t nested=0;
    for(std::size_t capacity=1;;capacity=capacity>maximum/2?maximum:capacity*2){
        if(!PreparedSpectralAutocorrelation::storage_bytes(capacity,config,nested,factory) || !prepared_storage::add_bytes(bytes,nested))return false;
        if(capacity==maximum)break;
    }
    return true;
}
}
bool autocorrelation_fft_size(std::size_t samples,const SpectralAutocorrelationConfig& config,std::size_t& output) noexcept {
    if(!samples || samples>std::numeric_limits<std::size_t>::max()/4 || !valid_parameters(config))return false;
    const auto minimum=2*samples-1;
    if(config.fft_size){
        if(config.fft_size<minimum || config.fft_size>std::numeric_limits<std::size_t>::max()/4)return false;
        output=config.fft_size;return true;
    }
    const auto size=config.length==AutocorrelationFftLength::PowerOfTwo?power_two(2*samples):smooth_length(minimum);
    if(!size)return false;
    output=size;return true;
}
bool PreparedSpectralAutocorrelation::prepare_transforms(const SpectralAutocorrelationConfig& config,const FourierBackendFactory* factory) {
    const bool reference=config.precision==NumericalPrecision::DesktopReference;
    if(reference && factory)return false;
    const bool external_real=factory && config.transform!=AutocorrelationTransform::Complex;
    if(!reference && !external_real && !complex_fft_.prepare(config.fft_size,factory))return false;
    use_real_=!reference && !factory && native_real(config);
    hermitian_=reference || external_real || use_real_ || config.transform==AutocorrelationTransform::Real;
    if(use_real_ && !real_fft_.prepare(config.fft_size))return false;
    return true;
}
bool PreparedSpectralAutocorrelation::configure(std::size_t samples,SpectralAutocorrelationConfig config,const FourierBackendFactory* factory) {
    if(!autocorrelation_fft_size(samples,config,config.fft_size) || !prepare_transforms(config,factory))return false;
    config_=config;samples_=samples;input_.resize(config.fft_size);
    transformed_.resize(2*config.fft_size);spectrum_.resize(config.fft_size);result_.resize(2*config.fft_size);values_.resize(samples);
    if(factory && config.transform!=AutocorrelationTransform::Complex && !configure_backend(*factory))return false;
    return config.precision!=NumericalPrecision::DesktopReference || configure_reference();
}
bool PreparedSpectralAutocorrelation::configure_backend(const FourierBackendFactory& factory) {
    if(!backend_forward_.prepare({config_.fft_size,FourierBackendTransform::RealForward},factory) ||
       !backend_inverse_.prepare({config_.fft_size,FourierBackendTransform::RealInverse},factory))return false;
    backend_spectrum_.resize(config_.fft_size+2);backend_result_.resize(config_.fft_size);return true;
}
bool PreparedSpectralAutocorrelation::prepare(std::size_t samples,SpectralAutocorrelationConfig config,const FourierBackendFactory* factory) {
    ready_=false;
    std::size_t bound=0;if(!storage_bytes(samples,config,bound,factory))return false;
    PreparedSpectralAutocorrelation next;
    if(!next.configure(samples,config,factory))return false;
    if(next.workspace_bytes()>bound)return false;
    *this=std::move(next);return true;
}
bool PreparedSpectralAutocorrelation::storage_bytes(std::size_t samples,SpectralAutocorrelationConfig config,std::size_t& bytes,const FourierBackendFactory* factory) noexcept {
    std::size_t required=0;
    if(!autocorrelation_fft_size(samples,config,config.fft_size))return false;
    if(!primary_transform_storage(config,factory,required) || !auxiliary_transform_storage(samples,config,factory,required))return false;
    if(!prepared_storage::add_array(required,config.fft_size,6*sizeof(float)) || !prepared_storage::add_array(required,samples,sizeof(float)))return false;
    bytes=required;return true;
}
bool PreparedSpectralAutocorrelation::forward(std::span<const float> input) noexcept {
    std::fill(input_.begin(),input_.end(),0);
    std::copy(input.begin(),input.end(),input_.begin());
    if(backend_forward_.ready())return backend_forward_.process(input_,std::span(transformed_).first(config_.fft_size+2));
    if(use_real_)return real_fft_.process(input_,std::span(transformed_).first(config_.fft_size+2));
    if(!complex_fft_.process(input_,{},transformed_))return false;
    if(hermitian_){
        transformed_[1]=0;
        if(config_.fft_size%2==0)transformed_[config_.fft_size+1]=0;
    }
    return true;
}
float PreparedSpectralAutocorrelation::compressed(float real,float imaginary) const noexcept {
    return detail::spectral_power(real,imaginary,config_.spectrum_divisor,config_.spectral_exponent);
}
bool PreparedSpectralAutocorrelation::compress() noexcept {
    const auto count=hermitian_?config_.fft_size/2+1:config_.fft_size;
    for(std::size_t i=0;i<count;++i){
        const float value=compressed(transformed_[2*i],transformed_[2*i+1]);
        if(!std::isfinite(value))return false;
        spectrum_[i]=value;
        if(hermitian_ && i && 2*i!=config_.fft_size)spectrum_[config_.fft_size-i]=value;
    }
    return true;
}
bool PreparedSpectralAutocorrelation::inverse() noexcept {
    if(backend_inverse_.ready())return inverse_backend();
    return complex_fft_.process(spectrum_,{},result_,true);
}
bool PreparedSpectralAutocorrelation::inverse_backend() noexcept {
    std::fill(backend_spectrum_.begin(),backend_spectrum_.end(),0.f);
    for(std::size_t bin=0;bin<=config_.fft_size/2;++bin)backend_spectrum_[2*bin]=spectrum_[bin];
    if(!backend_inverse_.process(backend_spectrum_,backend_result_))return false;
    std::fill(result_.begin(),result_.end(),0.f);
    for(std::size_t i=0;i<config_.fft_size;++i)result_[2*i]=backend_result_[i];
    return true;
}
bool PreparedSpectralAutocorrelation::process(std::span<const float> input,std::span<float> output) noexcept {
    ready_=false;
    if(input.size()!=samples_)return false;
    return process_prefix(input,output);
}
float PreparedSpectralAutocorrelation::inverse_value(std::size_t lag) const noexcept {
    const float real=result_[2*lag];
    return config_.inverse_output==AutocorrelationInverseOutput::ComplexMagnitude?
        std::hypot(real,result_[2*lag+1]):real;
}
bool PreparedSpectralAutocorrelation::collect(std::size_t samples,std::size_t lags) noexcept {
    for(std::size_t lag=0;lag<lags;++lag){
        float value=inverse_value(lag)*config_.inverse_scale;
        if(config_.normalization==CorrelationNormalization::Unbiased)value/=float(samples-lag);
        if(config_.normalization==CorrelationNormalization::Biased)value/=float(samples);
        if(!std::isfinite(value))return false;
        values_[lag]=value;
    }
    return true;
}
bool PreparedSpectralAutocorrelation::process_prefix(std::span<const float> input,std::span<float> output) noexcept {
    ready_=false;
    if(!samples_ || input.empty() || input.size()>samples_ || output.size()>input.size())return false;
    if(config_.precision==NumericalPrecision::DesktopReference)return process_reference(input,output);
    if(!forward(input) || !compress() || !inverse() || !collect(input.size(),output.size()))return false;
    std::copy_n(values_.begin(),output.size(),output.begin());
    active_samples_=input.size();active_lags_=output.size();ready_=true;return true;
}
SpectralAutocorrelationView PreparedSpectralAutocorrelation::view() const noexcept {
    if(!ready_)return {};
    const auto bins=hermitian_?config_.fft_size/2+1:config_.fft_size;
    return {input_,std::span(transformed_).first(2*bins),spectrum_,result_,
        std::span(values_).first(active_lags_),active_samples_,config_.fft_size};
}
std::size_t PreparedSpectralAutocorrelation::workspace_bytes() const noexcept {
    return (input_.capacity()+transformed_.capacity()+spectrum_.capacity()+result_.capacity()+values_.capacity())*sizeof(float)+
        (reference_input_.capacity()+reference_complex_.capacity()+reference_spectrum_.capacity()+reference_result_.capacity()+reference_values_.capacity())*sizeof(double)+
        real_fft_.workspace_bytes()+complex_fft_.workspace_bytes()+reference_fft_.workspace_bytes()+
        backend_forward_.workspace_bytes()+backend_inverse_.workspace_bytes()+(backend_spectrum_.capacity()+backend_result_.capacity())*sizeof(float);
}
bool PreparedVariableAutocorrelation::prepare(std::size_t maximum,CorrelationNormalization normalization,const FourierBackendFactory* factory) {
    std::size_t bound=0;if(!storage_bytes(maximum,normalization,bound,factory))return false;
    PreparedVariableAutocorrelation next;
    if(!next.prepare_plans(maximum,normalization,factory) || next.workspace_bytes()>bound)return false;
    *this=std::move(next);return true;
}
bool PreparedVariableAutocorrelation::prepare_plans(std::size_t maximum,CorrelationNormalization normalization,const FourierBackendFactory* factory) {
    maximum_samples_=maximum;
    plans_.reserve(std::bit_width(maximum-1)+1);
    SpectralAutocorrelationConfig config;config.normalization=normalization;
    for(std::size_t capacity=1;;capacity=capacity>maximum/2?maximum:capacity*2){
        PreparedSpectralAutocorrelation plan;
        if(!plan.prepare(capacity,config,factory))return false;
        plans_.push_back(std::move(plan));
        if(capacity==maximum)break;
    }
    return true;
}
bool PreparedVariableAutocorrelation::storage_bytes(std::size_t maximum,CorrelationNormalization normalization,std::size_t& bytes,const FourierBackendFactory* factory) noexcept {
    if(!maximum || maximum>std::numeric_limits<std::size_t>::max()/4)return false;
    if(normalization<CorrelationNormalization::Raw || normalization>CorrelationNormalization::Biased)return false;
    std::size_t required=0;SpectralAutocorrelationConfig config;config.normalization=normalization;
    if(!prepared_storage::add_array(required,std::bit_width(maximum-1)+1,sizeof(PreparedSpectralAutocorrelation)))return false;
    if(!variable_plan_storage(maximum,config,factory,required))return false;
    bytes=required;return true;
}
std::size_t PreparedVariableAutocorrelation::workspace_bytes() const noexcept {
    auto bytes=plans_.capacity()*sizeof(PreparedSpectralAutocorrelation);
    for(const auto& plan:plans_)bytes+=plan.workspace_bytes();
    return bytes;
}
bool PreparedVariableAutocorrelation::process(std::span<const float> input,std::span<float> output) noexcept {
    active_fft_=0;
    if(input.empty() || input.size()>maximum_samples_ || output.size()>input.size())return false;
    for(auto& plan:plans_)if(plan.samples()>=input.size()){
        if(!plan.process_prefix(input,output))return false;
        active_fft_=plan.fft_size();return true;
    }
    return false;
}
}
