// Adapted from librosa 1.0.0 feature/rhythm.py and util.normalize; ISC.
// Copyright (c) 2013--2026, librosa development team.
// Full notice: packaging/licenses/librosa.txt.
// Source: librosa/librosa@3e4ff7bf5898ac5b326e7dee56463b4b325b91cb.
#include <dsp_kernels/tempogram.h>
#include <dsp_kernels/sample_rotation.h>
#include <dsp_kernels/prepared_storage.h>
#include <dsp_kernels/sequence_statistics.h>
#include <dsp_kernels/detail/float_publication.h>
#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>

namespace dsp_kernels {
namespace {
bool native_fourier_tempogram(std::size_t size) noexcept {
    return size>=8 && size<=max_fft_samples && !(size&(size-1));
}
bool finite(std::span<const float> values) noexcept {
    return std::all_of(values.begin(),values.end(),[](float value){return std::isfinite(value);});
}
bool transform_storage(NumericalPrecision precision,std::size_t size,const FourierBackendFactory* factory,std::size_t& bytes) noexcept {
    if(precision==NumericalPrecision::DesktopReference)return !factory && detail::PreparedReferenceFourier::storage_bytes(size,bytes);
    if(factory){
        FourierBackendResources resources;
        if(!PreparedFourierBackend::query({size,FourierBackendTransform::RealForward},*factory,resources))return false;
        bytes=resources.cold_peak_bytes;return true;
    }
    return native_fourier_tempogram(size)?PreparedRfft::storage_bytes(size,false,bytes):PreparedFourierTransform::storage_bytes(size,bytes);
}
template<typename Scalar> Scalar norm(std::span<const Scalar> values,VectorNormalization mode,Scalar exponent) noexcept {
    Scalar result=mode==VectorNormalization::Minimum?std::numeric_limits<Scalar>::infinity():0;
    for(const Scalar value:values){
        const Scalar magnitude=std::abs(value);
        if(mode==VectorNormalization::Maximum)result=std::max(result,magnitude);
        else if(mode==VectorNormalization::Minimum)result=std::min(result,magnitude);
        else if(mode==VectorNormalization::L0)result+=Scalar(magnitude>0);
        else result+=std::pow(magnitude,exponent);
    }
    return mode==VectorNormalization::Lp?std::pow(result,Scalar(1)/exponent):result;
}
template<typename Scalar> bool normalization_scale(std::span<const Scalar> values,VectorNormalization mode,Scalar exponent,Scalar& scale) noexcept {
    if(mode==VectorNormalization::Lp && (!std::isfinite(exponent) || exponent<=0))return false;
    scale=norm<Scalar>(values,mode,exponent);return std::isfinite(scale);
}
template<typename Scalar> bool normalize_values(std::span<Scalar> values,VectorNormalization mode,Scalar exponent,Scalar minimum_scale=std::numeric_limits<Scalar>::min()) noexcept {
    if(!std::all_of(values.begin(),values.end(),[](Scalar x){return std::isfinite(x);}) || mode<VectorNormalization::None || mode>VectorNormalization::Lp)return false;
    if(mode==VectorNormalization::None || values.empty())return true;
    Scalar scale=0;if(!normalization_scale<Scalar>(values,mode,exponent,scale))return false;
    if(scale>0 && scale>=minimum_scale)for(auto& value:values)value/=scale;
    return true;
}
float interpolate_acf(std::span<const float> acf,float bpm,float rate) noexcept {
    if(bpm<=0 || acf.size()<2)return 0;
    const float lag=60.f*rate/bpm;
    if(lag<1 || lag>float(acf.size()-1))return 0;
    const auto low=std::size_t(std::floor(lag)),high=std::min(low+1,acf.size()-1);
    const float f0=60.f*rate/float(low),f1=60.f*rate/float(high);
    const float t=low==high?0:(bpm-f0)/(f1-f0);
    return acf[low]+t*(acf[high]-acf[low]);
}
}
namespace {
bool statistical_normalization_scale(std::span<const float> values,VectorNormalization mode,float& scale) noexcept {
    if(mode==VectorNormalization::Sum)return sum_float32(values,scale);
    if(!variance_float32(values,scale))return false;
    scale=std::sqrt(scale);return true;
}
bool normalize_statistics(std::span<float> values,VectorNormalization mode) noexcept {
    if(values.empty())return true;
    float scale=0;
    if(!statistical_normalization_scale(values,mode,scale))return false;
    if(scale!=0)for(auto& value:values)value/=scale;
    return finite(values);
}
}

bool normalize_vector(std::span<float> values,VectorNormalization mode,float exponent,float minimum_scale) noexcept {
    if(!std::isfinite(minimum_scale) || minimum_scale<0)return false;
    if(mode==VectorNormalization::Sum || mode==VectorNormalization::StandardDeviation)return normalize_statistics(values,mode);
    return normalize_values<float>(values,mode,exponent,minimum_scale);
}
bool PreparedFourierTempogram::prepare_transform(std::size_t size,const FourierBackendFactory* factory) {
    if(window_.config().precision==NumericalPrecision::DesktopReference)return !factory && prepare_reference(size);
    if(factory){
        if(!backend_.prepare({size,FourierBackendTransform::RealForward},*factory))return false;
        input_.resize(size);output_.resize(2*size);return true;
    }
    real_fft_=native_fourier_tempogram(size);
    if(real_fft_?!real_.prepare(size):!complex_.prepare(size))return false;
    input_.resize(size);output_.resize(2*size);return true;
}
bool PreparedFourierTempogram::prepare(TemporalWindowConfig config,std::span<const float> coefficients,
                                      std::size_t padding,bool zero_phase,const FourierBackendFactory* factory) {
    ready_=false;
    const auto limit=std::numeric_limits<std::size_t>::max()/4;
    if(config.length>limit || padding>limit-config.length)return false;
    return prepare_size(config,coefficients,config.length+padding,zero_phase,{},factory);
}
bool PreparedFourierTempogram::prepare_size(TemporalWindowConfig config,std::span<const float> coefficients,std::size_t size,bool zero_phase,std::span<const double> reference,const FourierBackendFactory* factory) {
    ready_=false;
    std::size_t bound=0;if(!storage_bytes(config,coefficients,size,zero_phase,bound,factory))return false;
    PreparedFourierTempogram next;
    if(!next.window_.prepare(config,coefficients,reference) || !next.prepare_transform(size,factory))return false;
    next.windowed_.resize(config.length);
    if(next.workspace_bytes()>bound)return false;
    next.zero_phase_=zero_phase;*this=std::move(next);return true;
}
bool PreparedFourierTempogram::storage_bytes(TemporalWindowConfig config,std::span<const float> coefficients,std::size_t size,bool zero_phase,std::size_t& bytes,const FourierBackendFactory* factory) noexcept {
    if(!size || size>std::numeric_limits<std::size_t>::max()/4 || (zero_phase && size<config.length-config.length/2))return false;
    std::size_t required=0,nested=0;
    if(!PreparedTemporalWindow::storage_bytes(config,coefficients,required))return false;
    if(config.precision==NumericalPrecision::DesktopReference &&
       !prepared_storage::add_array(required,size+2*(size/2+1)+config.length,sizeof(double)))return false;
    if(!transform_storage(config.precision,size,factory,nested) || !prepared_storage::add_bytes(required,nested) || !prepared_storage::add_array(required,size,3*sizeof(float)) ||
       !prepared_storage::add_array(required,config.length,sizeof(float)))return false;
    bytes=required;return true;
}
bool PreparedFourierTempogram::load(std::span<const float> values,std::size_t frame) noexcept {
    if(!reference_input_.empty())return window_.read_double(values,frame,reference_windowed_,TemporalPadding::Constant) && embed_reference();
    return window_.read(values,frame,windowed_) && embed_fourier_frame(windowed_,input_,zero_phase_);
}
bool PreparedFourierTempogram::process_frame(std::span<const float> values,std::size_t frame,std::span<float> output) noexcept {
    ready_=false;
    return output.size()==2*bins() && load(values,frame) && execute(output);
}
bool PreparedFourierTempogram::process_at(std::span<const float> values,std::ptrdiff_t first,std::span<float> output,std::ptrdiff_t rotation,TemporalPadding padding) noexcept {
    ready_=false;
    if(!reference_input_.empty())return output.size()==2*bins() &&
        window_.read_double_at(values,first,reference_windowed_,padding) && embed_reference(rotation) && execute(output);
    return output.size()==2*bins() && window_.read_at(values,first,windowed_,padding) &&
        embed_fourier_frame(windowed_,input_,zero_phase_) && (!rotation || rotate_samples(input_,input_,rotation)) && execute(output);
}
bool PreparedFourierTempogram::execute(std::span<float> output) noexcept {
    if(!reference_input_.empty())return execute_reference(output);
    const bool ok=backend_.ready()?backend_.process(input_,std::span(output_).first(2*bins())):
        real_fft_?real_.process(input_,std::span(output_).first(2*bins())):complex_.process(input_,{},output_);
    if(!ok)return false;
    std::copy_n(output_.begin(),output.size(),output.begin());ready_=true;return true;
}
FourierTempogramView PreparedFourierTempogram::view() const noexcept {
    if(!ready_)return {};
    return {windowed_,input_,std::span(output_).first(2*bins()),input_.size()};
}
std::size_t PreparedFourierTempogram::workspace_bytes() const noexcept {
    return (input_.capacity()+output_.capacity()+windowed_.capacity())*sizeof(float)+
        (reference_input_.capacity()+reference_output_.capacity()+reference_windowed_.capacity())*sizeof(double)+
        window_.workspace_bytes()+real_.workspace_bytes()+complex_.workspace_bytes()+reference_fft_.workspace_bytes()+backend_.workspace_bytes();
}
SpectralAutocorrelationConfig real_linear_autocorrelation_config() noexcept {
    SpectralAutocorrelationConfig config;
    config.transform=AutocorrelationTransform::Real;
    config.length=AutocorrelationFftLength::Smooth235;
    return config;
}
bool PreparedAutocorrelationTempogram::prepare(AutocorrelationTempogramConfig config,std::span<const float> coefficients) {
    ready_=false;
    std::size_t bound=0;if(!storage_bytes(config,coefficients,bound))return false;
    PreparedAutocorrelationTempogram next;if(!next.configure(config,coefficients) || next.workspace_bytes()>bound)return false;
    *this=std::move(next);return true;
}
bool PreparedAutocorrelationTempogram::configure(AutocorrelationTempogramConfig config,std::span<const float> coefficients) {
    if(!window_.prepare(config.window,coefficients))return false;
    config.correlation.precision=config.window.precision;
    if(!correlation_.prepare(config.window.length,config.correlation))return false;
    config.correlation=correlation_.config();config_=config;input_.resize(config.window.length);output_.resize(config.window.length);
    if(config.window.precision==NumericalPrecision::DesktopReference){reference_input_.resize(config.window.length);reference_output_.resize(config.window.length);}
    return true;
}
bool PreparedAutocorrelationTempogram::storage_bytes(AutocorrelationTempogramConfig config,std::span<const float> coefficients,std::size_t& bytes) noexcept {
    if(config.normalization<VectorNormalization::None || config.normalization>VectorNormalization::Lp || unsigned(config.padding)>unsigned(TemporalPadding::Symmetric))return false;
    if(config.normalization==VectorNormalization::Lp && (!std::isfinite(config.norm_exponent) || config.norm_exponent<=0))return false;
    std::size_t required=0,nested=0;
    config.correlation.precision=config.window.precision;
    if(!PreparedTemporalWindow::storage_bytes(config.window,coefficients,required) ||
       !PreparedSpectralAutocorrelation::storage_bytes(config.window.length,config.correlation,nested) ||
       !prepared_storage::add_bytes(required,nested) || !prepared_storage::add_array(required,config.window.length,2*sizeof(float)))return false;
    if(config.window.precision==NumericalPrecision::DesktopReference && !prepared_storage::add_array(required,config.window.length,2*sizeof(double)))return false;
    bytes=required;return true;
}
std::size_t PreparedAutocorrelationTempogram::frames(std::size_t count) const noexcept {
    const auto result=window_.frames(count);return config_.window.center?std::min(count,result):result;
}
bool PreparedAutocorrelationTempogram::process_frame(std::span<const float> values,std::size_t frame,std::span<float> output) noexcept {
    ready_=false;
    if(frame>=frames(values.size()) || output.size()!=output_.size())return false;
    if(!reference_input_.empty()){
        if(!process_reference(values,frame))return false;
    }else if(!process_portable(values,frame))return false;
    std::copy(output_.begin(),output_.end(),output.begin());ready_=true;return true;
}
bool PreparedAutocorrelationTempogram::process_portable(std::span<const float> values,std::size_t frame) noexcept {
    if(!window_.read(values,frame,input_,config_.padding))return false;
    if(!correlation_.process(input_,output_))return false;
    return normalize_vector(output_,config_.normalization,config_.norm_exponent);
}
AutocorrelationTempogramView PreparedAutocorrelationTempogram::view() const noexcept {
    if(!ready_)return {};
    const auto correlation=correlation_.view();
    return {input_,correlation.values,output_,correlation};
}
std::size_t PreparedAutocorrelationTempogram::workspace_bytes() const noexcept {
    return (input_.capacity()+output_.capacity())*sizeof(float)+(reference_input_.capacity()+reference_output_.capacity())*sizeof(double)+
        window_.workspace_bytes()+correlation_.workspace_bytes();
}
bool PreparedFourierTempogram::prepare_reference(std::size_t size) {
    if(!reference_fft_.prepare(size))return false;
    input_.resize(size);output_.resize(2*size);reference_input_.resize(size);
    reference_output_.resize(2*(size/2+1));reference_windowed_.resize(window_.config().length);return true;
}
bool PreparedFourierTempogram::embed_reference(std::ptrdiff_t rotation) noexcept {
    if(!embed_fourier_frame(reference_windowed_,reference_input_,zero_phase_) ||
       (rotation && !rotate_samples(reference_input_,reference_input_,rotation)))return false;
    if(!detail::finite_float_range(reference_windowed_) || !detail::finite_float_range(reference_input_))return false;
    std::transform(reference_windowed_.begin(),reference_windowed_.end(),windowed_.begin(),[](double x){return float(x);});
    std::transform(reference_input_.begin(),reference_input_.end(),input_.begin(),[](double x){return float(x);});return true;
}
bool PreparedFourierTempogram::execute_reference(std::span<float> output) noexcept {
    if(!reference_fft_.forward(reference_input_,reference_output_))return false;
    if(!detail::finite_float_range(reference_output_))return false;
    std::transform(reference_output_.begin(),reference_output_.end(),output_.begin(),[](double x){return float(x);});
    std::copy_n(output_.begin(),output.size(),output.begin());ready_=true;return true;
}
bool PreparedAutocorrelationTempogram::process_reference(std::span<const float> values,std::size_t frame) noexcept {
    if(!window_.read_double(values,frame,reference_input_,config_.padding) || !correlation_.process_double(reference_input_,reference_output_) ||
       !normalize_values<double>(reference_output_,config_.normalization,double(config_.norm_exponent)))return false;
    if(!detail::finite_float_range(reference_input_) || !detail::finite_float_range(reference_output_))return false;
    std::transform(reference_input_.begin(),reference_input_.end(),input_.begin(),[](double x){return float(x);});
    std::transform(reference_output_.begin(),reference_output_.end(),output_.begin(),[](double x){return float(x);});return true;
}
bool fourier_tempo_frequencies(float rate,std::size_t size,std::span<float> output) noexcept {
    if(!std::isfinite(rate) || rate<=0 || !size || output.size()!=size/2+1)return false;
    for(std::size_t i=0;i<output.size();++i)output[i]=60.f*rate*float(i)/float(size);
    return true;
}
bool autocorrelation_tempo_frequencies(float rate,std::span<float> output) noexcept {
    if(!std::isfinite(rate) || rate<=0 || output.empty())return false;
    output[0]=std::numeric_limits<float>::infinity();
    for(std::size_t i=1;i<output.size();++i)output[i]=60.f*rate/float(i);
    return true;
}
bool hybrid_tempogram_frame(std::span<const float> complex,std::span<const float> acf,float rate,std::size_t size,std::span<float> output) noexcept {
    if(!size || output.size()!=size/2+1 || complex.size()!=2*output.size() || acf.size()<2)return false;
    if(!std::isfinite(rate) || rate<=0 || !finite(complex) || !finite(acf))return false;
    for(std::size_t i=0;i<output.size();++i){
        const float a=interpolate_acf(acf,60.f*rate*float(i)/float(size),rate);
        output[i]=std::sqrt(std::hypot(complex[2*i],complex[2*i+1])*std::abs(a));
    }
    return finite(output);
}
bool hybrid_tempogram_frame(std::span<const float> bpm,std::span<const float> complex,const PreparedCurveInterpolation& acf,std::span<float> output) noexcept {
    if(complex.size()%2 || bpm.size()!=complex.size()/2 || output.size()!=bpm.size() || acf.values().empty())return false;
    for(std::size_t i=0;i<bpm.size();++i){
        float sample=0;if(!acf.sample(bpm[i],sample) || !std::isfinite(sample))return false;
        const float magnitude=std::hypot(complex[2*i],complex[2*i+1]);
        const float value=std::sqrt(std::max(0.f,magnitude*std::abs(sample)));
        if(!std::isfinite(magnitude) || !std::isfinite(value))return false;
        output[i]=value;
    }
    return true;
}

}
