#include <dsp_kernels/recursive_filter.h>
#include <algorithm>
#include <cmath>
#include <functional>
#include <limits>
#include <utility>

namespace dsp_kernels {
namespace {
bool finite_values(std::span<const float> values) noexcept {
    return std::all_of(values.begin(),values.end(),[](float value){return std::isfinite(value);});
}
bool overlapping_buffers(std::span<const float> input,std::span<float> output) noexcept {
    if(input.empty() || input.data()==output.data())return false;
    const std::less<const float*> before;
    return before(input.data(),output.data()+output.size()) && before(output.data(),input.data()+input.size());
}
}
namespace {
bool finite_normalized_coefficients(std::span<const float> b,std::span<const float> a) noexcept {
    const auto finite=[divisor=a[0]](float value){return std::isfinite(value/divisor);};
    return std::all_of(b.begin(),b.end(),finite) && std::all_of(a.begin(),a.end(),finite);
}
void store_normalized_coefficients(std::span<const float> b,std::span<const float> a,
    std::span<float> numerator,std::span<float> denominator) noexcept {
    for(std::size_t i=0;i<b.size();++i)numerator[i]=b[i]/a[0];
    for(std::size_t i=1;i<a.size();++i)denominator[i]=a[i]/a[0];
    denominator[0]=1;
}
}

bool PreparedRecursiveFilter::coefficients(std::span<const float> b,std::span<const float> a) {
    if(b.empty() || a.empty() || a[0]==0 || !finite_values(b) || !finite_values(a))return false;
    const auto count=std::max(b.size(),a.size());
    numerator_.assign(count,0);denominator_.assign(count,0);
    store_normalized_coefficients(b,a,numerator_,denominator_);
    numerator_size_=b.size();denominator_size_=a.size();active_size_=count;
    return finite_values(numerator_) && finite_values(denominator_);
}
std::size_t PreparedRecursiveFilter::state_count() const noexcept {
    return active_size_*(options_.form==RecursiveFilterForm::DirectI?2:1);
}
bool PreparedRecursiveFilter::prepare(std::span<const float> b,std::span<const float> a,SubnormalStatePolicy cleanup,RecursiveFilterOptions options) {
    std::size_t bound=0;if(!storage_bytes(b.size(),a.size(),options,bound))return false;
    if(options.form==RecursiveFilterForm::DirectI &&
       std::max(b.size(),a.size())>std::numeric_limits<std::size_t>::max()/2)return false;
    PreparedRecursiveFilter next;
    if(!next.coefficients(b,a))return false;
    next.cleanup_=cleanup;next.options_=options;
    next.state_.resize(next.state_count());next.pending_.resize(next.state_.size());
    if(next.storage_bytes()>bound)return false;
    *this=std::move(next);return true;
}
bool PreparedRecursiveFilter::reconfigure(std::span<const float> b,std::span<const float> a,bool preserve) {
    PreparedRecursiveFilter next;
    if(!next.prepare(b,a,cleanup_,options_))return false;
    if(preserve && active_size_==next.active_size_)std::copy_n(state_.begin(),state_count(),next.state_.begin());
    *this=std::move(next);return true;
}
bool PreparedRecursiveFilter::copy_coefficients(std::span<const float> b,std::span<const float> a) noexcept {
    if(b.empty() || a.empty() || a[0]==0 || std::max(a.size(),b.size())>numerator_.size())return false;
    if(!finite_normalized_coefficients(b,a))return false;
    std::fill(numerator_.begin(),numerator_.end(),0);std::fill(denominator_.begin(),denominator_.end(),0);
    store_normalized_coefficients(b,a,numerator_,denominator_);
    numerator_size_=b.size();denominator_size_=a.size();return true;
}
bool PreparedRecursiveFilter::set_coefficients(std::span<const float> b,std::span<const float> a,bool preserve) noexcept {
    const auto count=std::max(a.size(),b.size());
    if(!copy_coefficients(b,a))return false;
    if(!preserve || count!=active_size_)reset();
    active_size_=count;return true;
}
void PreparedRecursiveFilter::reset() noexcept {
    std::fill(state_.begin(),state_.end(),0);std::fill(pending_.begin(),pending_.end(),0);
}
bool PreparedRecursiveFilter::set_state(std::span<const float> values) noexcept {
    if(values.size()!=state_count() || !finite_values(values))return false;
    std::copy(values.begin(),values.end(),state_.begin());return true;
}
float PreparedRecursiveFilter::clean(float value,std::size_t index) const noexcept {
    if(cleanup_==SubnormalStatePolicy::Preserve)return value;
    if(cleanup_==SubnormalStatePolicy::FlushAfterFirst && index==0)return value;
    return std::fpclassify(value)==FP_SUBNORMAL?0:value;
}
float PreparedRecursiveFilter::next_state(std::size_t k,float input,float output) const noexcept {
    if(k>=numerator_size_)return -denominator_[k]*output+state_[k];
    if(k>=denominator_size_)return numerator_[k]*input+state_[k];
    return (numerator_[k]*input-denominator_[k]*output)+state_[k];
}
bool PreparedRecursiveFilter::advance(float input,float output) noexcept {
    for(std::size_t k=1;k<active_size_;++k){
        const float value=next_state(k,input,output);
        if(!std::isfinite(value))return false;
        pending_[k-1]=clean(value,k-1);
    }
    pending_[active_size_-1]=0;return true;
}
bool PreparedRecursiveFilter::process(float input,float& output) noexcept {
    if(!active_size_ || !std::isfinite(input))return false;
    if(std::abs(input)<options_.input_zero_threshold)input=0;
    if(options_.form==RecursiveFilterForm::DirectI)return direct(input,output);
    const float value=numerator_[0]*input+state_[0];
    if(!std::isfinite(value) || !advance(input,value))return false;
    state_.swap(pending_);output=value;return true;
}
bool PreparedRecursiveFilter::direct(float input,float& output) noexcept {
    float value=numerator_[0]*input;
    for(std::size_t k=1;k<active_size_;++k){
        value+=numerator_[k]*state_[k-1];
        value-=denominator_[k]*state_[active_size_+k-1];
    }
    if(!std::isfinite(value))return false;
    advance_direct(input,value);state_.swap(pending_);output=value;return true;
}
void PreparedRecursiveFilter::advance_direct(float input,float output) noexcept {
    pending_[0]=clean(input,0);pending_[active_size_]=clean(output,0);
    for(std::size_t k=1;k<active_size_;++k){
        pending_[k]=clean(state_[k-1],k);
        pending_[active_size_+k]=clean(state_[active_size_+k-1],k);
    }
}
bool PreparedRecursiveFilter::process(std::span<const float> input,std::span<float> output) noexcept {
    if(!active_size_ || input.size()!=output.size() || overlapping_buffers(input,output) || !finite_values(input))return false;
    for(std::size_t i=0;i<input.size();++i)if(!process(input[i],output[i]))return false;
    return true;
}
std::size_t PreparedRecursiveFilter::storage_bytes() const noexcept {
    return (numerator_.capacity()+denominator_.capacity()+state_.capacity()+pending_.capacity())*sizeof(float);
}
bool PreparedRecursiveFilter::storage_bytes(std::size_t b,std::size_t a,RecursiveFilterOptions options,std::size_t& bytes) noexcept {
    if(!b || !a || unsigned(options.form)>unsigned(RecursiveFilterForm::DirectI) ||
       !std::isfinite(options.input_zero_threshold) || options.input_zero_threshold<0)return false;
    const auto count=std::max(a,b),width=(options.form==RecursiveFilterForm::DirectI?6u:4u)*sizeof(float);
    if(count>std::numeric_limits<std::size_t>::max()/width)return false;
    bytes=count*width;return true;
}
}
