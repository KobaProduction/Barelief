#include "common_support.h"
#include <dsp_kernels/log_compression.h>
#include <dsp_kernels/tempogram.h>
#include <dsp_kernels/convolution.h>
#include <dsp_kernels/correlation.h>
#include <dsp_kernels/fourier_transform.h>
#include <cmath>
#include <utility>

namespace dsp_graph_adapters::detail {
using namespace signal_contracts;using namespace dsp_kernels;
namespace {
bool buffers(std::size_t count,std::size_t copies,std::size_t& bytes) noexcept {
    return prepared_storage::mul_bytes(count,copies*sizeof(float),bytes);
}
bool signed_lag(const domain::ResolvedProcessingNode& n,std::ptrdiff_t& value) noexcept {
    const auto* p=parameter(n,"first_lag");if(!p){value=0;return true;}
    const auto* literal=std::get_if<NumericLiteral>(p);if(!literal)return false;
    return std::visit([&](auto v){if constexpr(std::is_integral_v<decltype(v)>){if(!std::in_range<std::ptrdiff_t>(v))return false;value=std::ptrdiff_t(v);return true;}else return false;},*literal);
}
struct LogConfig {float multiply=1,add=1;std::size_t base=0;bool signed_input=false;};
bool log_config(const domain::ResolvedProcessingNode& n,LogConfig& c) noexcept {
    return number(n,"multiply",1,c.multiply) && c.multiply>=0 && number(n,"add",1,c.add) && c.add>=0 && integer(n,"base",0,c.base) && c.base<=2 && flag(n,"signed_input",false,c.signed_input);
}
class Log final:public CommonKernel<Log> {
public:
    bool prepare(const domain::ResolvedProcessingNode& n){std::size_t count=0;if(!common_vector(n,0,count) || !log_config(n,config_))return false;input_.resize(count);output_.resize(count);return true;}
    std::size_t workspace_bytes() const noexcept override {return (input_.capacity()+output_.capacity())*sizeof(float);}
private:
    bool run(std::span<const NumericOperationInput> in,std::span<NumericOperationOutput> out) noexcept override {
        std::span<const float> values;if(!common_values(in[0],input_,values))return false;
        auto result=std::span(output_).first(values.size());
        return logarithmic_compression(values,result,config_.multiply,config_.add,LogarithmBase(config_.base),config_.signed_input) && write_values(out[0],result);
    }
    LogConfig config_;std::vector<float> input_,output_;
};
struct NormConfig {std::size_t method=1;float exponent=2;};
bool norm_config(const domain::ResolvedProcessingNode& n,NormConfig& c) noexcept {
    return integer(n,"method",1,c.method) && c.method<=4 && number(n,"exponent",2,c.exponent) && c.exponent>0;
}
class Normalize final:public CommonKernel<Normalize> {
public:
    bool prepare(const domain::ResolvedProcessingNode& n){std::size_t count=0;if(!common_vector(n,0,count) || !norm_config(n,config_))return false;values_.resize(count);return true;}
    std::size_t workspace_bytes() const noexcept override {return values_.capacity()*sizeof(float);}
private:
    bool run(std::span<const NumericOperationInput> in,std::span<NumericOperationOutput> out) noexcept override {
        std::span<const float> input;if(!common_values(in[0],values_,input))return false;
        auto values=std::span(values_).first(input.size());
        return normalize_vector(values,VectorNormalization(config_.method),config_.exponent) && write_values(out[0],values);
    }
    NormConfig config_;std::vector<float> values_;
};
class Convolution final:public CommonKernel<Convolution> {
public:
    bool prepare(const domain::ResolvedProcessingNode& n){
        std::size_t count=0;if(!common_vector(n,0,count) || !integer(n,"crop",0,crop_) || crop_>1)return false;
        const auto kernel=array(n,"kernel");kernel_.assign(kernel.begin(),kernel.end());input_.resize(count);
        output_.resize(same_convolution_size(count,kernel_.size(),SameConvolutionCrop(crop_)));return true;
    }
    std::size_t workspace_bytes() const noexcept override {return (input_.capacity()+output_.capacity()+kernel_.capacity())*sizeof(float);}
private:
    bool run(std::span<const NumericOperationInput> in,std::span<NumericOperationOutput> out) noexcept override {
        std::span<const float> input;if(!common_values(in[0],input_,input))return false;
        auto result=std::span(output_).first(same_convolution_size(input.size(),kernel_.size(),SameConvolutionCrop(crop_)));
        return convolve_same(input,kernel_,result,SameConvolutionCrop(crop_)) && write_values(out[0],result);
    }
    std::size_t crop_=0;std::vector<float> kernel_,input_,output_;
};
struct CorrelationConfig {std::ptrdiff_t first=0;std::size_t lags=1,normalization=0;};
bool correlation_config(const domain::ResolvedProcessingNode& n,std::size_t count,CorrelationConfig& c) noexcept {
    if(!count || count>std::size_t(PTRDIFF_MAX) || !signed_lag(n,c.first) || c.first< -65536 || c.first>65536 ||
       !integer(n,"lags",count,c.lags) || !c.lags || c.lags>65536 ||
       !integer(n,"normalization",0,c.normalization) || c.normalization>3)return false;
    if(c.first<= -std::ptrdiff_t(count) || c.first>=std::ptrdiff_t(count))return false;
    // The existing core checks (N-1)-first in ptrdiff_t before its loop.
    if(c.first<0 && std::size_t(-c.first)>std::size_t(PTRDIFF_MAX)-(count-1))return false;
    std::size_t available=count;
    if(c.first<0){if(!prepared_storage::add_bytes(available,std::size_t(-c.first)))return false;}
    else available-=std::size_t(c.first);
    return c.lags<=available;
}
class Correlation final:public CommonKernel<Correlation> {
public:
    bool prepare(const domain::ResolvedProcessingNode& n){
        std::size_t count=0,right=0;if(!common_vector(n,0,count) || !common_vector(n,1,right) || count!=right || !correlation_config(n,count,config_))return false;
        left_.resize(count);right_.resize(count);output_.resize(config_.lags);return true;
    }
    std::size_t workspace_bytes() const noexcept override {return (left_.capacity()+right_.capacity()+output_.capacity())*sizeof(float);}
private:
    bool run(std::span<const NumericOperationInput> in,std::span<NumericOperationOutput> out) noexcept override {
        std::span<const float> a,b;if(!common_values(in[0],left_,a) || !common_values(in[1],right_,b) || a.size()!=b.size())return false;
        return linear_crosscorrelation(a,b,config_.first,output_,CorrelationNormalization(config_.normalization)) && write_values(out[0],output_);
    }
    CorrelationConfig config_;std::vector<float> left_,right_,output_;
};
class Inverse final:public CommonKernel<Inverse> {
public:
    bool prepare(const domain::ResolvedProcessingNode& n){
        if(!integer(n,"fft_size",512,size_) || !kernel_.prepare(size_))return false;
        real_.resize(size_);imaginary_.resize(size_);output_.resize(size_*2);return true;
    }
    std::size_t workspace_bytes() const noexcept override {return (real_.capacity()+imaginary_.capacity()+output_.capacity())*sizeof(float)+kernel_.workspace_bytes();}
private:
    bool run(std::span<const NumericOperationInput> in,std::span<NumericOperationOutput> out) noexcept override {
        std::span<const float> real,imaginary;
        if(!common_values(in[0],real_,real) || !common_values(in[1],imaginary_,imaginary) || real.size()!=size_ || imaginary.size()!=size_ || !kernel_.process(real,imaginary,output_,true))return false;
        for(std::size_t i=0;i<size_;++i){real_[i]=output_[2*i];imaginary_[i]=output_[2*i+1];}
        return write_values(out[0],real_) && write_values(out[1],imaginary_);
    }
    std::size_t size_=0;PreparedFourierTransform kernel_;std::vector<float> real_,imaginary_,output_;
};
domain::ProcessingSupport element_support(const domain::ResolvedProcessingNode& n,const dataflow_runtime::NumericRuntimeProfile& p,std::size_t count) noexcept {
    std::size_t bytes=0;domain::ProcessingSupport support;
    if(n.kind=="LogCompression"){
        LogConfig c;if(!log_config(n,c) || !buffers(count,2,bytes) || !common_output(n,0,Rank::Vector,count))return {};
        common_resources(n,p,sizeof(Log),bytes,support);return support;
    }
    if(n.kind=="VectorNormalize"){
        NormConfig c;if(!norm_config(n,c) || !buffers(count,1,bytes) || !common_output(n,0,Rank::Vector,count))return {};
        common_resources(n,p,sizeof(Normalize),bytes,support);return support;
    }
    return {};
}
domain::ProcessingSupport convolution_support(const domain::ResolvedProcessingNode& n,const dataflow_runtime::NumericRuntimeProfile& p,std::size_t count) noexcept {
    std::size_t bytes=0;domain::ProcessingSupport support;
    const auto kernel=array(n,"kernel");std::size_t crop=0;
    if(!integer(n,"crop",0,crop) || crop>1)return {};
    const auto output=same_convolution_size(count,kernel.size(),SameConvolutionCrop(crop));
    if(!buffers(count,1,bytes) || !prepared_storage::add_array(bytes,kernel.size(),sizeof(float)) || !prepared_storage::add_array(bytes,output,sizeof(float)) || !common_output(n,0,Rank::Vector,std::max(std::size_t{1},output)))return {};
    common_resources(n,p,sizeof(Convolution),bytes,support);return support;
}
domain::ProcessingSupport pair_support(const domain::ResolvedProcessingNode& n,const dataflow_runtime::NumericRuntimeProfile& p) noexcept {
    std::size_t count=0,right=0,bytes=0;domain::ProcessingSupport s;
    if(!common_vector(n,0,count) || !common_vector(n,1,right) || count!=right)return {};
    if(n.kind=="DirectCorrelation"){
        CorrelationConfig c;if(!correlation_config(n,count,c) || !buffers(count,2,bytes) || !prepared_storage::add_array(bytes,c.lags,sizeof(float)) || !common_output(n,0,Rank::Vector,c.lags))return {};
        common_resources(n,p,sizeof(Correlation),bytes,s);return s;
    }
    std::size_t fft=0;
    if(!integer(n,"fft_size",512,fft) || fft!=count || !PreparedFourierTransform::storage_bytes(fft,bytes) || !prepared_storage::add_array(bytes,fft,4*sizeof(float)) ||
        !common_output(n,0,Rank::Vector,fft) || !common_output(n,1,Rank::Vector,fft))return {};
    common_resources(n,p,sizeof(Inverse),bytes,s);return s;
}
template<class T> PreparedCommon make(const domain::ResolvedProcessingNode& n){auto result=std::make_unique<T>();return result->prepare(n)?std::move(result):nullptr;}
}
PreparedCommon prepare_common_values(const domain::ResolvedProcessingNode& n){
    if(n.kind=="FeatureAggregate")return prepare_feature_aggregate(n);
    if(n.kind=="LogCompression")return make<Log>(n);
    if(n.kind=="VectorNormalize")return make<Normalize>(n);
    if(n.kind=="SameConvolution")return make<Convolution>(n);
    if(n.kind=="DirectCorrelation")return make<Correlation>(n);
    if(n.kind=="InverseFourier")return make<Inverse>(n);
    return {};
}
domain::ProcessingSupport assess_common_values(const domain::ResolvedProcessingNode& n,const dataflow_runtime::NumericRuntimeProfile& p) noexcept {
    if(n.kind=="FeatureAggregate")return assess_feature_aggregate(n,p);
    if(n.kind=="DirectCorrelation" || n.kind=="InverseFourier")return pair_support(n,p);
    std::size_t count=0;if(!common_vector(n,0,count))return {};
    return n.kind=="SameConvolution"?convolution_support(n,p,count):element_support(n,p,count);
}
}
