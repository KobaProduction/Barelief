#include <dsp_kernels/temporal_window.h>
#include <dsp_kernels/prepared_storage.h>
#include <dsp_kernels/detail/float_publication.h>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <utility>

namespace dsp_kernels {
bool PreparedTemporalWindow::frame_count(TemporalWindowConfig config,std::size_t input,std::size_t& out) noexcept {
    std::size_t bytes=0;if(!storage_bytes(config,{},bytes))return false;
    PreparedTemporalWindow geometry;geometry.config_=config;out=geometry.geometry_frames(input);return true;
}
namespace {
bool valid_window(TemporalWindowConfig config,std::span<const float> coefficients) noexcept {
    if(unsigned(config.precision)>unsigned(NumericalPrecision::DesktopReference))return false;
    if(!config.length || !config.hop || config.length>std::size_t(std::numeric_limits<std::ptrdiff_t>::max())/2)return false;
    if(config.tail<TemporalTail::Drop || config.tail>TemporalTail::OriginInside)return false;
    if((config.center && config.tail==TemporalTail::CoverEnd) || (!config.center && config.tail==TemporalTail::CenterCoverEnd))return false;
    if(!coefficients.empty())return coefficients.size()==config.length &&
        std::all_of(coefficients.begin(),coefficients.end(),[](float value){return std::isfinite(value);});
    return std::find(window_function_names.begin(),window_function_names.end(),window_function_name(config.function))!=window_function_names.end();
}

void prepare_coefficients(std::span<float> window,TemporalWindowConfig config,std::span<const float> coefficients) {
    if(!coefficients.empty())std::copy(coefficients.begin(),coefficients.end(),window.begin());
    else for(std::size_t i=0;i<config.length;++i)
        window[i]=window_coefficient(i,config.length,config.function,config.periodic);
}
std::size_t reflected_index(std::ptrdiff_t at,std::size_t size,bool symmetric) noexcept {
    if(size==1)return 0;
    const auto period=2*std::uint64_t(size-std::size_t(!symmetric));
    const auto magnitude=at<0?std::uint64_t(-(at+1))+1:std::uint64_t(at);
    const auto index=at<0?(period-magnitude%period)%period:magnitude%period;
    return std::size_t(index<size?index:period-index-std::size_t(symmetric));
}
}
bool PreparedTemporalWindow::storage_bytes(TemporalWindowConfig config,std::span<const float> coefficients,std::size_t& bytes) noexcept {
    std::size_t required=0;
    if(!valid_window(config,coefficients) || !prepared_storage::mul_bytes(config.length,sizeof(float),required))return false;
    if(config.precision==NumericalPrecision::DesktopReference && !prepared_storage::add_array(required,config.length,sizeof(double)))return false;
    bytes=required;return true;
}
bool PreparedTemporalWindow::prepare(TemporalWindowConfig config,std::span<const float> coefficients,std::span<const double> reference) {
    std::size_t bound=0;
    if(!storage_bytes(config,coefficients,bound))return false;
    PreparedTemporalWindow next;
    next.config_=config;
    next.window_.resize(config.length);
    next.explicit_window_=!coefficients.empty() || !reference.empty();
    prepare_coefficients(next.window_,config,coefficients);
    if(!next.configure_reference(coefficients,reference))return false;
    if(next.workspace_bytes()>bound)return false;
    *this=std::move(next);
    return true;
}
bool PreparedTemporalWindow::configure_reference(std::span<const float> coefficients,std::span<const double> reference) {
    if(config_.precision!=NumericalPrecision::DesktopReference)return reference.empty();
    if(!reference.empty() && (reference.size()!=window_.size() || !detail::finite_float_range(reference)))return false;
    reference_window_.resize(window_.size());
    for(std::size_t i=0;i<window_.size();++i){
        if(!reference.empty())reference_window_[i]=reference[i];
        else if(!coefficients.empty())reference_window_[i]=coefficients[i];
        else reference_window_[i]=window_coefficient_value<double>(i,window_.size(),config_.function,config_.periodic);
        window_[i]=float(reference_window_[i]);
    }
    return true;
}
bool PreparedTemporalWindow::set_coefficients(std::span<const float> values) noexcept {
    if(window_.empty() || values.size()!=window_.size() ||
        !std::all_of(values.begin(),values.end(),[](float x){return std::isfinite(x);}))return false;
    if(values.data()!=window_.data())std::copy(values.begin(),values.end(),window_.begin());
    if(!reference_window_.empty())std::copy(values.begin(),values.end(),reference_window_.begin());
    explicit_window_=true;return true;
}
std::size_t PreparedTemporalWindow::center_offset() const noexcept {
    return config_.length/2+std::size_t(config_.odd_center_ceil && config_.length%2);
}
std::size_t PreparedTemporalWindow::center_cover_frames(std::size_t count) const noexcept {
    const auto offset=center_offset(),terminal=count+offset-config_.length/2;
    const auto cover=1+terminal/config_.hop+std::size_t(terminal%config_.hop!=0);
    return std::min(cover,1+(count+offset-1)/config_.hop);
}
std::size_t PreparedTemporalWindow::frames(std::size_t count) const noexcept {
    return window_.empty()?0:geometry_frames(count);
}
std::size_t PreparedTemporalWindow::geometry_frames(std::size_t count) const noexcept {
    if(count>std::size_t(std::numeric_limits<std::ptrdiff_t>::max())-config_.length)return 0;
    if(!count)return config_.empty_centered_frame && config_.center && config_.tail==TemporalTail::Drop && config_.length%2==0?1:0;
    if(config_.tail==TemporalTail::OriginInside)return 1+(count-1)/config_.hop;
    if(config_.tail==TemporalTail::CenterCoverEnd)return center_cover_frames(count);
    if(config_.tail==TemporalTail::CoverEnd){
        const auto cover=count<=config_.length?1:2+(count-config_.length-1)/config_.hop;
        return std::min(cover,1+(count-1)/config_.hop);
    }
    const auto padded=count+(config_.center?center_offset()+config_.length/2:0);
    return padded<config_.length?0:1+(padded-config_.length)/config_.hop;
}
float PreparedTemporalWindow::sample(std::span<const float> input,std::ptrdiff_t at,TemporalPadding padding) const noexcept {
    if(at>=0 && std::size_t(at)<input.size())return input[std::size_t(at)];
    if(padding==TemporalPadding::Constant || !config_.center || input.empty())return 0;
    if(padding==TemporalPadding::Edge)return at<0?input.front():input.back();
    if(padding==TemporalPadding::Reflect || padding==TemporalPadding::Symmetric)
        return input[reflected_index(at,input.size(),padding==TemporalPadding::Symmetric)];
    const auto half=std::ptrdiff_t(config_.length/2);if(!half)return 0;
    if(at<0)return input.front()*float(at+half)/float(half);
    return input.back()*float(std::ptrdiff_t(input.size())+half-1-at)/float(half);
}
bool PreparedTemporalWindow::read(std::span<const float> input,std::size_t frame,std::span<float> output,TemporalPadding padding) const noexcept {
    if(frame>=frames(input.size()) || output.size()!=config_.length)return false;
    const auto start=std::ptrdiff_t(frame*config_.hop)-(config_.center?std::ptrdiff_t(center_offset()):0);
    return read_at(input,start,output,padding);
}
bool PreparedTemporalWindow::read_at(std::span<const float> input,std::ptrdiff_t first,std::span<float> output,TemporalPadding padding) const noexcept {
    const auto limit=std::numeric_limits<std::ptrdiff_t>::max();
    if(window_.empty() || output.size()!=config_.length || input.size()>std::size_t(limit)-config_.length ||
        first>limit-std::ptrdiff_t(config_.length) || unsigned(padding)>unsigned(TemporalPadding::Symmetric))return false;
    for(std::size_t i=0;i<output.size();++i)output[i]=sample(input,first+std::ptrdiff_t(i),padding)*window_[i];
    return std::all_of(output.begin(),output.end(),[](float value){return std::isfinite(value);});
}
bool PreparedTemporalWindow::read_double(std::span<const float> input,std::size_t frame,std::span<double> output,TemporalPadding padding) const noexcept {
    if(frame>=frames(input.size()))return false;
    const auto start=std::ptrdiff_t(frame*config_.hop)-(config_.center?std::ptrdiff_t(center_offset()):0);
    return read_double_at(input,start,output,padding);
}
bool PreparedTemporalWindow::read_double_at(std::span<const float> input,std::ptrdiff_t first,std::span<double> output,TemporalPadding padding) const noexcept {
    const auto limit=std::numeric_limits<std::ptrdiff_t>::max();
    if(reference_window_.empty() || output.size()!=config_.length || input.size()>std::size_t(limit)-config_.length ||
       first>limit-std::ptrdiff_t(config_.length) || unsigned(padding)>unsigned(TemporalPadding::Symmetric))return false;
    for(std::size_t i=0;i<output.size();++i)output[i]=double(sample(input,first+std::ptrdiff_t(i),padding))*reference_window_[i];
    return std::all_of(output.begin(),output.end(),[](double x){return std::isfinite(x);});
}
}
