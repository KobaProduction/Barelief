#include "common_support.h"
#include "common_array.h"
#include <dsp_kernels/log_compression.h>
#include <dsp_kernels/vector_normalization.h>
#include <dsp_kernels/convolution.h>
#include <dsp_kernels/correlation.h>
#include <dsp_kernels/fourier_transform.h>
#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>

namespace dsp_graph_adapters::detail {
using namespace signal_contracts;using namespace dsp_kernels;
namespace {
bool buffers(std::size_t count,std::size_t copies,std::size_t& bytes) noexcept {
    return prepared_storage::mul_bytes(count,copies*sizeof(float),bytes);
}
bool signed_lag(const dataflow_runtime::NumericOperationNodeView& n,std::ptrdiff_t& value) noexcept {
    const auto* p=parameter(n,"first_lag");if(!p){value=0;return true;}
    const auto* literal=std::get_if<NumericLiteral>(p);if(!literal)return false;
    return std::visit([&](auto v){if constexpr(std::is_integral_v<decltype(v)>){if(!std::in_range<std::ptrdiff_t>(v))return false;value=std::ptrdiff_t(v);return true;}else return false;},*literal);
}
struct LogConfig {float multiply=1,add=1;std::size_t base=0;bool signed_input=false;CommonArrayGeometry geometry;};
bool log_config(const dataflow_runtime::NumericOperationNodeView& n,LogConfig& c) noexcept {
    return number(n,"multiply",1,c.multiply) && c.multiply>=0 && number(n,"add",1,c.add) && c.add>=0 && integer(n,"base",0,c.base) && c.base<=2 && flag(n,"signed_input",false,c.signed_input) && common_array_geometry(n,0,c.geometry);
}
class Log final:public CommonKernel<Log> {
public:
    bool prepare(const dataflow_runtime::NumericOperationNodeView& n){if(!log_config(n,config_))return false;input_.resize(config_.geometry.width());output_.resize(input_.size());return true;}
    std::size_t workspace_bytes() const noexcept override {return (input_.capacity()+output_.capacity())*sizeof(float);}
private:
    bool run(std::span<const NumericOperationInput> in,std::span<NumericOperationOutput> out) noexcept override {
        out[0].value.actual=in[0].value.actual;
        const auto width=in[0].value.actual[config_.geometry.axis];
        auto values=std::span(input_).first(width),result=std::span(output_).first(width);
        for(std::size_t row=0;row<common_array_rows(in[0].value,config_.geometry);++row){
            if(!common_array_read(in[0].value,row,values,config_.geometry) ||
               !logarithmic_compression(values,result,config_.multiply,config_.add,LogarithmBase(config_.base),config_.signed_input) ||
               !common_array_write(out[0].value,row,result,config_.geometry))return false;
        }
        return true;
    }
    LogConfig config_;std::vector<float> input_,output_;
};
struct NormConfig {std::size_t method=1;float exponent=2,minimum_scale=std::numeric_limits<float>::min();CommonArrayGeometry geometry;};
bool norm_config(const dataflow_runtime::NumericOperationNodeView& n,NormConfig& c) noexcept {
    return integer(n,"method",1,c.method) && c.method<=6 && number(n,"exponent",2,c.exponent) && c.exponent>0 &&
        number(n,"minimum_scale",std::numeric_limits<float>::min(),c.minimum_scale) && c.minimum_scale>=0 && common_array_geometry(n,0,c.geometry,true);
}
class Normalize final:public CommonKernel<Normalize> {
public:
    bool prepare(const dataflow_runtime::NumericOperationNodeView& n){
        if(!norm_config(n,config_))return false;
        values_.resize(config_.geometry.width());return true;
    }
    std::size_t workspace_bytes() const noexcept override {return values_.capacity()*sizeof(float);}
private:
    bool run(std::span<const NumericOperationInput> in,std::span<NumericOperationOutput> out) noexcept override {
        const auto& geometry=config_.geometry;out[0].value.actual=in[0].value.actual;
        auto values=std::span(values_).first(in[0].value.actual[geometry.axis]);
        for(std::size_t row=0;row<common_array_rows(in[0].value,geometry);++row){
            if(!common_array_read(in[0].value,row,values,geometry) ||
               !normalize_vector(values,VectorNormalization(config_.method),config_.exponent,config_.minimum_scale) ||
               !common_array_write(out[0].value,row,values,geometry))return false;
        }
        return true;
    }
    NormConfig config_;std::vector<float> values_;
};
struct ConvolutionConfig {CommonArrayGeometry geometry;std::size_t crop=0,kernel=0;DType type=DType::Float32;};
bool convolution_config(const dataflow_runtime::NumericOperationNodeView& n,ConvolutionConfig& c) noexcept {
    if(!common_array_geometry(n,0,c.geometry,true,true) || !integer(n,"crop",0,c.crop) || c.crop>1)return false;
    c.type=contract(n,domain::PortDirection::Input,0)->dtype;c.kernel=array(n,"kernel").size();
    if(n.inputs.size()==1)return c.type!=DType::Float64 || reference_same_convolution_available(c.geometry.width(),c.kernel);
    const auto kernel=contract(n,domain::PortDirection::Input,1);
    if(!kernel || kernel->dtype!=c.type || kernel->shape.rank!=Rank::Vector || kernel->plane_count!=1)return false;
    c.kernel=std::size_t(kernel->shape.extents[0].maximum);
    return n.inputs.size()==2 && c.kernel && (c.type!=DType::Float64 || reference_same_convolution_available(c.geometry.width(),c.kernel));
}
template<typename Scalar> class Convolution final:public CommonKernel<Convolution<Scalar>> {
public:
    bool prepare(const dataflow_runtime::NumericOperationNodeView& n){
        if(!convolution_config(n,config_))return false;
        prepare_kernel(n);
        input_.resize(config_.geometry.width());output_.resize(same_convolution_size(input_.size(),kernel_.size(),SameConvolutionCrop(config_.crop)));return true;
    }
    std::size_t workspace_bytes() const noexcept override {return (input_.capacity()+output_.capacity()+kernel_.capacity())*sizeof(Scalar);}
private:
    void prepare_kernel(const dataflow_runtime::NumericOperationNodeView& n) {
        kernel_.resize(config_.kernel);
        if(n.inputs.size()!=1)return;
        const auto fallback=array(n,"kernel");
        std::copy(fallback.begin(),fallback.end(),kernel_.begin());
    }
    bool row(const NumericBufferView& input,MutableNumericBufferView& output,std::size_t at) noexcept {
        auto values=std::span(input_).first(input.actual[config_.geometry.axis]);
        if(!common_array_read(input,at,values,config_.geometry))return false;
        auto result=std::span(output_).first(same_convolution_size(values.size(),kernel_.size(),SameConvolutionCrop(config_.crop)));
        if(!convolve_same(std::span<const Scalar>(values),std::span<const Scalar>(kernel_),result,SameConvolutionCrop(config_.crop)))return false;
        return common_array_write(output,at,std::span<const Scalar>(result),config_.geometry);
    }
    bool load_kernel(const NumericOperationInput& input) noexcept {
        if(input.value.actual[0]!=kernel_.size())return false;
        for(std::size_t i=0;i<kernel_.size();++i)
            if(load_element(input.value,{0,i,0},kernel_[i])!=Status::ok)return false;
        return true;
    }

    bool run(std::span<const NumericOperationInput> in,std::span<NumericOperationOutput> out) noexcept override {
        if(in.size()==2 && !load_kernel(in[1]))return false;
        out[0].value.actual=in[0].value.actual;
        out[0].value.actual[config_.geometry.axis]=same_convolution_size(in[0].value.actual[config_.geometry.axis],kernel_.size(),SameConvolutionCrop(config_.crop));
        for(std::size_t at=0;at<common_array_rows(in[0].value,config_.geometry);++at)if(!row(in[0].value,out[0].value,at))return false;
        return true;
    }
    ConvolutionConfig config_;std::vector<Scalar> kernel_,input_,output_;
};
struct CorrelationConfig {std::ptrdiff_t first=0;std::size_t lags=1,normalization=0;};
bool correlation_config(const dataflow_runtime::NumericOperationNodeView& n,std::size_t count,CorrelationConfig& c) noexcept {
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
bool paired_arrays(const dataflow_runtime::NumericOperationNodeView& n,CommonArrayGeometry& geometry) noexcept {
    CommonArrayGeometry right;
    return n.inputs.size()==2 && common_array_geometry(n,0,geometry) && common_array_geometry(n,1,right) &&
        geometry.rank==right.rank && geometry.rows==right.rows && geometry.columns==right.columns;
}
CommonArrayGeometry output_width(CommonArrayGeometry geometry,std::size_t width) noexcept {
    if(geometry.axis)geometry.columns=width;else geometry.rows=width;
    return geometry;
}
bool array_output(const dataflow_runtime::NumericOperationNodeView& n,domain::PortId port,CommonArrayGeometry geometry) noexcept {
    return common_output(n,port,geometry.rank,geometry.rows,geometry.columns);
}
class Correlation final:public CommonKernel<Correlation> {
public:
    bool prepare(const dataflow_runtime::NumericOperationNodeView& n){
        if(!paired_arrays(n,geometry_) || !correlation_config(n,geometry_.width(),config_))return false;
        left_.resize(geometry_.width());right_.resize(left_.size());output_.resize(config_.lags);return true;
    }
    std::size_t workspace_bytes() const noexcept override {return (left_.capacity()+right_.capacity()+output_.capacity())*sizeof(float);}
private:
    bool row(std::span<const NumericOperationInput> in,NumericOperationOutput& out,std::size_t row) noexcept {
        auto left=std::span(left_).first(in[0].value.actual[geometry_.axis]);
        auto right=std::span(right_).first(left.size());
        return common_array_read(in[0].value,row,left,geometry_) && common_array_read(in[1].value,row,right,geometry_) &&
            linear_crosscorrelation(left,right,config_.first,output_,CorrelationNormalization(config_.normalization)) &&
            common_array_write(out.value,row,output_,geometry_);
    }
    bool run(std::span<const NumericOperationInput> in,std::span<NumericOperationOutput> out) noexcept override {
        if(in[0].value.actual!=in[1].value.actual)return false;
        out[0].value.actual=in[0].value.actual;out[0].value.actual[geometry_.axis]=config_.lags;
        for(std::size_t at=0;at<common_array_rows(in[0].value,geometry_);++at)if(!row(in,out[0],at))return false;
        return true;
    }
    CommonArrayGeometry geometry_;CorrelationConfig config_;std::vector<float> left_,right_,output_;
};
class Inverse final:public CommonKernel<Inverse> {
public:
    bool prepare(const dataflow_runtime::NumericOperationNodeView& n){
        if(!paired_arrays(n,geometry_) || !integer(n,"fft_size",512,size_) || size_!=geometry_.width() || !kernel_.prepare(size_))return false;
        real_.resize(size_);imaginary_.resize(size_);output_.resize(size_*2);return true;
    }
    std::size_t workspace_bytes() const noexcept override {return (real_.capacity()+imaginary_.capacity()+output_.capacity())*sizeof(float)+kernel_.workspace_bytes();}
private:
    bool row(std::span<const NumericOperationInput> in,std::span<NumericOperationOutput> out,std::size_t at) noexcept {
        if(!common_array_read(in[0].value,at,real_,geometry_) || !common_array_read(in[1].value,at,imaginary_,geometry_) ||
           !kernel_.process(real_,imaginary_,output_,true))return false;
        for(std::size_t i=0;i<size_;++i){real_[i]=output_[2*i];imaginary_[i]=output_[2*i+1];}
        return common_array_write(out[0].value,at,real_,geometry_) && common_array_write(out[1].value,at,imaginary_,geometry_);
    }
    bool run(std::span<const NumericOperationInput> in,std::span<NumericOperationOutput> out) noexcept override {
        if(in[0].value.actual!=in[1].value.actual || in[0].value.actual[geometry_.axis]!=size_)return false;
        out[0].value.actual=out[1].value.actual=in[0].value.actual;
        for(std::size_t at=0;at<common_array_rows(in[0].value,geometry_);++at)if(!row(in,out,at))return false;
        return true;
    }
    CommonArrayGeometry geometry_;std::size_t size_=0;PreparedFourierTransform kernel_;std::vector<float> real_,imaginary_,output_;
};
struct ClampConfig {CommonArrayGeometry geometry;float minimum=0,maximum=std::numeric_limits<float>::max();};
bool clamp_config(const dataflow_runtime::NumericOperationNodeView& node,ClampConfig& config) noexcept {
    return common_array_geometry(node,0,config.geometry) && number(node,"minimum",0,config.minimum) &&
        number(node,"maximum",std::numeric_limits<float>::max(),config.maximum) && config.minimum<=config.maximum;
}
class Clamp final:public CommonKernel<Clamp> {
public:
    bool prepare(const dataflow_runtime::NumericOperationNodeView& node){return clamp_config(node,config_);}
    std::size_t workspace_bytes() const noexcept override {return 0;}
private:
    bool run(std::span<const NumericOperationInput> in,std::span<NumericOperationOutput> out) noexcept override {
        out[0].value.actual=in[0].value.actual;
        for(std::size_t row=0;row<in[0].value.actual[0];++row)
            for(std::size_t column=0;column<in[0].value.actual[1];++column){
                float value=0;
                if(load_element(in[0].value,{0,row,column},value)!=Status::ok ||
                   store_element(out[0].value,{0,row,column},std::clamp(value,config_.minimum,config_.maximum))!=Status::ok)return false;
            }
        return true;
    }
    ClampConfig config_;
};
class QuantileMask final:public CommonKernel<QuantileMask> {
public:
    bool prepare(const dataflow_runtime::NumericOperationNodeView& node){
        std::size_t count=0;
        if(!common_vector(node,0,count) || !number(node,"fraction",.6f,fraction_) || fraction_<0 || fraction_>=1)return false;
        values_.resize(count);sorted_.resize(count);return true;
    }
    std::size_t workspace_bytes() const noexcept override {return (values_.capacity()+sorted_.capacity())*sizeof(float);}
private:
    bool run(std::span<const NumericOperationInput> in,std::span<NumericOperationOutput> out) noexcept override {
        std::span<const float> values;if(!common_values(in[0],values_,values))return false;
        if(values.empty())return write_values(out[0],{});
        std::copy(values.begin(),values.end(),sorted_.begin());
        std::sort(sorted_.begin(),sorted_.begin()+std::ptrdiff_t(values.size()));
        const float threshold=sorted_[std::size_t(std::floor(float(values.size())*fraction_))];
        auto mask=std::span(values_).first(values.size());
        for(float& value:mask)value=float(value>=threshold);
        return write_values(out[0],mask);
    }
    float fraction_=.6f;std::vector<float> values_,sorted_;
};
domain::ProcessingSupport quantile_support(const dataflow_runtime::NumericOperationNodeView& node,
    const dataflow_runtime::NumericOperationProfile& profile) noexcept {
    std::size_t count=0,bytes=0;float fraction=0;domain::ProcessingSupport support;
    if(!common_vector(node,0,count) || !number(node,"fraction",.6f,fraction) || fraction<0 || fraction>=1 ||
       !buffers(count,2,bytes) || !common_output(node,0,Rank::Vector,count))return {};
    common_resources(node,profile,sizeof(QuantileMask),bytes,support);return support;
}
domain::ProcessingSupport log_support(const dataflow_runtime::NumericOperationNodeView& n,const dataflow_runtime::NumericOperationProfile& p) noexcept {
    LogConfig c;std::size_t bytes=0;domain::ProcessingSupport support;
    if(!log_config(n,c) || !buffers(c.geometry.width(),2,bytes) || !array_output(n,0,c.geometry))return {};
    common_resources(n,p,sizeof(Log),bytes,support);return support;
}
domain::ProcessingSupport clamp_support(const dataflow_runtime::NumericOperationNodeView& n,const dataflow_runtime::NumericOperationProfile& p) noexcept {
    ClampConfig config;domain::ProcessingSupport support;
    if(!clamp_config(n,config) || !array_output(n,0,config.geometry))return {};
    common_resources(n,p,sizeof(Clamp),0,support);return support;
}
domain::ProcessingSupport normalization_support(const dataflow_runtime::NumericOperationNodeView& n,const dataflow_runtime::NumericOperationProfile& p) noexcept {
    NormConfig c;std::size_t bytes=0;domain::ProcessingSupport support;
    if(!norm_config(n,c) || !buffers(c.geometry.width(),1,bytes) ||
        !common_output(n,0,c.geometry.rank,c.geometry.rows,c.geometry.columns))return {};
    common_resources(n,p,sizeof(Normalize),bytes,support);return support;
}
domain::ProcessingSupport correlation_support(const dataflow_runtime::NumericOperationNodeView& n,const dataflow_runtime::NumericOperationProfile& p,CommonArrayGeometry geometry) noexcept {
    const auto count=geometry.width();CorrelationConfig config;std::size_t bytes=0;domain::ProcessingSupport support;
    if(!correlation_config(n,count,config) || !buffers(count,2,bytes) ||
       !prepared_storage::add_array(bytes,config.lags,sizeof(float)) || !array_output(n,0,output_width(geometry,config.lags)))return {};
    common_resources(n,p,sizeof(Correlation),bytes,support);return support;
}
domain::ProcessingSupport inverse_support(const dataflow_runtime::NumericOperationNodeView& n,const dataflow_runtime::NumericOperationProfile& p,CommonArrayGeometry geometry) noexcept {
    std::size_t fft=0,bytes=0;domain::ProcessingSupport support;
    if(!integer(n,"fft_size",512,fft) || fft!=geometry.width() || !PreparedFourierTransform::storage_bytes(fft,bytes) ||
       !prepared_storage::add_array(bytes,fft,4*sizeof(float)) || !array_output(n,0,geometry) || !array_output(n,1,geometry))return {};
    common_resources(n,p,sizeof(Inverse),bytes,support);return support;
}

domain::ProcessingSupport element_support(const dataflow_runtime::NumericOperationNodeView& n,const dataflow_runtime::NumericOperationProfile& p) noexcept {
    if(n.kind=="LogCompression")return log_support(n,p);
    if(n.kind=="Clamp")return clamp_support(n,p);
    if(n.kind=="VectorNormalize")return normalization_support(n,p);
    return {};
}
domain::ProcessingSupport convolution_support(const dataflow_runtime::NumericOperationNodeView& n,const dataflow_runtime::NumericOperationProfile& p) noexcept {
    ConvolutionConfig c;std::size_t bytes=0;domain::ProcessingSupport support;
    if(!convolution_config(n,c))return {};
    auto geometry=c.geometry;
    const auto count=geometry.width(),output=same_convolution_size(count,c.kernel,SameConvolutionCrop(c.crop));
    if(geometry.axis)geometry.columns=output;else geometry.rows=output;
    if(!prepared_storage::add_array(bytes,count,common_array_element_bytes(c.type)) ||
       !prepared_storage::add_array(bytes,c.kernel,common_array_element_bytes(c.type)) ||
       !prepared_storage::add_array(bytes,output,common_array_element_bytes(c.type)) ||
       !common_output(n,0,geometry.rank,geometry.rows,geometry.columns,c.type))return {};
    const auto object=c.type==DType::Float64?sizeof(Convolution<double>):sizeof(Convolution<float>);
    common_resources(n,p,object,bytes,support);return support;
}
domain::ProcessingSupport pair_support(const dataflow_runtime::NumericOperationNodeView& n,const dataflow_runtime::NumericOperationProfile& p) noexcept {
    CommonArrayGeometry geometry;if(!paired_arrays(n,geometry))return {};
    if(n.kind=="DirectCorrelation")return correlation_support(n,p,geometry);
    return inverse_support(n,p,geometry);
}

template<class T> PreparedCommon make(const dataflow_runtime::NumericOperationNodeView& n){auto result=std::make_unique<T>();return result->prepare(n)?std::move(result):nullptr;}
PreparedCommon prepare_value_filters(const dataflow_runtime::NumericOperationNodeView& n) {
    if(n.kind=="SameConvolution")return contract(n,domain::PortDirection::Input,0)->dtype==DType::Float64?make<Convolution<double>>(n):make<Convolution<float>>(n);
    if(n.kind=="DirectCorrelation")return make<Correlation>(n);
    if(n.kind=="InverseFourier")return make<Inverse>(n);
    return {};
}

}
namespace {
PreparedCommon prepare_element_values(const dataflow_runtime::NumericOperationNodeView& n) {
    if(n.kind=="LogCompression")return make<Log>(n);
    if(n.kind=="VectorNormalize")return make<Normalize>(n);
    if(n.kind=="Clamp")return make<Clamp>(n);
    if(n.kind=="QuantileMask")return make<QuantileMask>(n);
    return prepare_value_filters(n);
}
}

PreparedCommon prepare_common_values(const dataflow_runtime::NumericOperationNodeView& n){
    if(n.kind=="TemporalFrameSequence")return prepare_temporal_frame_sequence(n);
    if(n.kind=="FeatureAggregate")return prepare_feature_aggregate(n);
    return prepare_element_values(n);
}
domain::ProcessingSupport assess_common_values(const dataflow_runtime::NumericOperationNodeView& n,const dataflow_runtime::NumericOperationProfile& p) noexcept {
    if(n.kind=="TemporalFrameSequence")return assess_temporal_frame_sequence(n,p);
    if(n.kind=="FeatureAggregate")return assess_feature_aggregate(n,p);
    if(n.kind=="DirectCorrelation" || n.kind=="InverseFourier")return pair_support(n,p);
    if(n.kind=="QuantileMask")return quantile_support(n,p);
    if(n.kind=="SameConvolution")return convolution_support(n,p);
    return element_support(n,p);
}
}
