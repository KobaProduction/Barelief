#include "common_support.h"
#include "common_array.h"
#include <domain/numeric/atomic_operation_ports.h>
#include <dsp_kernels/curve_interpolation.h>
#include <dsp_kernels/prepared_resampler.h>
#include <dsp_kernels/comb_filter_bank.h>
#include <dsp_kernels/value_histogram.h>
#include <dsp_kernels/peaks.h>
#include <algorithm>
#include <cmath>
#include <type_traits>

namespace dsp_graph_adapters::detail {
using namespace signal_contracts;using namespace dsp_kernels;
namespace comb_ports=domain::atomic_ports::comb;
namespace {
bool interpolation_config(const dataflow_runtime::NumericOperationNodeView& n,CurveInterpolationConfig& c) noexcept {
    std::size_t method=0;
    if(!integer(n,"method",0,method) || method>8 || !integer(n,"spline_order",1,c.spline_order) ||
       !number(n,"fill_below",0,c.fill_below) || !number(n,"fill_above",0,c.fill_above) ||
       !flag(n,"extrapolate",false,c.extrapolate) || !flag(n,"bounds_error",false,c.bounds_error) ||
       !flag(n,"keep_infinite_coordinates",false,c.keep_infinite_coordinates) || !flag(n,"source_linear",false,c.source_linear))return false;
    c.kind=CurveInterpolation(method);return true;
}
bool valid_coordinates(std::span<const float> values) noexcept {
    if(values.empty())return false;
    for(std::size_t i=0;i<values.size();++i)if(std::isfinite(values[i]))
        for(std::size_t j=0;j<i;++j)if(values[i]==values[j])return false;
    return true;
}
class Interpolation final:public CommonKernel<Interpolation> {
public:
    bool prepare(const dataflow_runtime::NumericOperationNodeView& n){
        std::size_t count=0,queries=0;CurveInterpolationConfig c;const auto coordinates=array(n,"coordinates");
        if(!common_vector(n,0,count) || !common_vector(n,1,queries) || coordinates.size()!=count || !interpolation_config(n,c) || !valid_coordinates(coordinates) || !kernel_.prepare(coordinates,c))return false;
        values_.resize(count);queries_.resize(queries);result_.resize(queries);return true;
    }
    std::size_t workspace_bytes() const noexcept override {return kernel_.workspace_bytes()+(values_.capacity()+queries_.capacity()+result_.capacity())*sizeof(float);}
private:
    bool run(std::span<const NumericOperationInput> in,std::span<NumericOperationOutput> out) noexcept override {
        std::span<const float> values,queries;
        if(!common_values(in[0],values_,values) || !common_values(in[1],queries_,queries) || values.size()!=values_.size() || !kernel_.set_values(values))return false;
        auto result=std::span(result_).first(queries.size());
        for(std::size_t k=0;k<queries.size();++k)if(!kernel_.sample(queries[k],result[k]))return false;
        return write_values(out[0],result);
    }
    PreparedCurveInterpolation kernel_;std::vector<float> values_,queries_,result_;
};
bool expansion_config(const dataflow_runtime::NumericOperationNodeView& node,ResamplingWindowConfig& config) noexcept {
    std::size_t factor=0;
    if(!common_vector(node,0,config.input_samples) || !integer(node,"factor",2,factor) || factor<1 || factor>4 ||
       !endpoint_expansion_size(config.input_samples,unsigned(factor),config.output_samples))return false;
    config.channels=1;config.mode=ResamplingWindowMode::EndpointLinear;config.expansion_factor=unsigned(factor);return true;
}
class LinearExpansion final:public CommonKernel<LinearExpansion> {
public:
    bool prepare(const dataflow_runtime::NumericOperationNodeView& node){
        ResamplingWindowConfig config;if(!expansion_config(node,config) || !kernel_.prepare_window(config))return false;
        input_.resize(config.input_samples);output_.resize(config.output_samples);return true;
    }
    std::size_t workspace_bytes() const noexcept override {
        return kernel_.resources().owned_bytes+(input_.capacity()+output_.capacity())*sizeof(float);
    }
private:
    bool run(std::span<const NumericOperationInput> in,std::span<NumericOperationOutput> out) noexcept override {
        std::span<const float> input;if(!common_values(in[0],input_,input))return false;
        if(input.empty())return write_values(out[0],{});
        const auto count=kernel_.window_output_samples(input.size());std::size_t written=0;
        auto output=std::span(output_).first(count);
        return kernel_.process_window_prefix(input,input.size(),input.size(),output,count,written) && written==count && write_values(out[0],output);
    }
    PreparedResampler kernel_;std::vector<float> input_,output_;
};
domain::ProcessingSupport expansion_support(const dataflow_runtime::NumericOperationNodeView& node,
    const dataflow_runtime::NumericOperationProfile& profile) noexcept {
    ResamplingWindowConfig config;ResamplerResources resources;domain::ProcessingSupport support;
    if(!expansion_config(node,config) || !PreparedResampler::window_resource_requirements(config,nullptr,resources))return {};
    auto bytes=resources.owned_bytes;
    if(!prepared_storage::add_array(bytes,config.input_samples,sizeof(float)) ||
       !prepared_storage::add_array(bytes,config.output_samples,sizeof(float)) || !common_output(node,0,Rank::Vector,config.output_samples))return {};
    common_resources(node,profile,sizeof(LinearExpansion),bytes,support);return support;
}
bool comb_config(const dataflow_runtime::NumericOperationNodeView& n,std::size_t& maximum,CombDirection& direction) noexcept {
    const auto delays=array(n,"delays"),alpha=array(n,"alpha");std::size_t mode=1;maximum=0;
    if(delays.empty() || (alpha.size()!=1 && alpha.size()!=delays.size()) || !integer(n,"direction",1,mode) || mode>1)return false;
    for(float delay:delays){if(!std::isfinite(delay) || delay<1 || delay>=float(SIZE_MAX) || std::floor(delay)!=delay)return false;maximum=std::max(maximum,std::size_t(delay));}
    if(!std::all_of(alpha.begin(),alpha.end(),[](float a){return std::isfinite(a);}))return false;
    direction=CombDirection(mode);return true;
}
bool comb_input(const dataflow_runtime::NumericOperationNodeView& n,CommonArrayGeometry& geometry) noexcept {
    return common_array_geometry(n,comb_ports::values,geometry,false,true) && geometry.rank==Rank::Vector;
}
template<typename Scalar> constexpr NumericalPrecision comb_precision() noexcept {
    return std::is_same_v<Scalar,double>?NumericalPrecision::DesktopReference:NumericalPrecision::PortableFloat32;
}
template<typename Scalar> class Comb final:public CommonKernel<Comb<Scalar>> {
public:
    bool prepare(const dataflow_runtime::NumericOperationNodeView& n){
        CommonArrayGeometry geometry;std::size_t maximum=0;CombDirection direction;
        if(!comb_input(n,geometry) || !comb_config(n,maximum,direction) || !flag(n,"reset_per_call",false,reset_))return false;
        const auto values=array(n,"delays");std::vector<std::size_t> delays;delays.reserve(values.size());for(float value:values)delays.push_back(std::size_t(value));
        if(!kernel_.prepare(delays,array(n,"alpha"),direction,1,comb_precision<Scalar>()))return false;
        row_.resize(delays.size());return true;
    }
    std::size_t workspace_bytes() const noexcept override {return kernel_.workspace_bytes()+row_.capacity()*sizeof(Scalar);}
private:
    void reset_kernel() noexcept override {kernel_.reset();}
    bool sample(const NumericBufferView& input,MutableNumericBufferView& output,std::size_t time) noexcept {
        Scalar value{};
        if(load_element(input,{0,time,0},value)!=Status::ok || !kernel_.process({&value,1},row_))return false;
        for(std::size_t band=0;band<row_.size();++band)if(store_element(output,{0,time,band},row_[band])!=Status::ok)return false;
        return true;
    }
    bool run(std::span<const NumericOperationInput> in,std::span<NumericOperationOutput> out) noexcept override {
        if(reset_)kernel_.reset();
        if(!set_shape(out[0],in[0].value.actual[0],kernel_.filters()))return false;
        for(std::size_t time=0;time<in[0].value.actual[0];++time)if(!sample(in[0].value,out[0].value,time))return false;
        return write_values(out[comb_ports::delays],array(definition(this->descriptor_->node),"delays"));
    }
    PreparedCombFilterBank kernel_;std::vector<Scalar> row_;bool reset_=false;
};
struct HistogramConfig {std::size_t bins=16,mode=0;float step=1;};
bool histogram_config(const dataflow_runtime::NumericOperationNodeView& n,HistogramConfig& c) noexcept {
    return integer(n,"bins",16,c.bins) && c.bins>=2 && integer(n,"method",0,c.mode) && c.mode<=1 && number(n,"step",1,c.step) && c.step>0;
}
class Histogram final:public CommonKernel<Histogram> {
public:
    bool prepare(const dataflow_runtime::NumericOperationNodeView& n){std::size_t count=0;if(!common_vector(n,0,count) || !histogram_config(n,config_))return false;input_.resize(count);counts_.resize(config_.bins);centers_.resize(config_.bins);return true;}
    std::size_t workspace_bytes() const noexcept override {return (input_.capacity()+centers_.capacity())*sizeof(float)+counts_.capacity()*sizeof(std::uint64_t);}
private:
    bool run(std::span<const NumericOperationInput> in,std::span<NumericOperationOutput> out) noexcept override {
        std::span<const float> input;if(!common_values(in[0],input_,input) || input.empty())return false;
        if(config_.mode){
            if(!rounded_bin_counts(input,counts_,config_.step))return false;
            for(std::size_t i=0;i<centers_.size();++i)centers_[i]=float(i)*config_.step;
        }else if(!uniform_value_histogram(input,counts_,centers_))return false;
        return write_samples(out[0],counts_) && write_values(out[1],centers_);
    }
    HistogramConfig config_;std::vector<float> input_,centers_;std::vector<std::uint64_t> counts_;
};
class Peaks final:public CommonKernel<Peaks> {
public:
    bool prepare(const dataflow_runtime::NumericOperationNodeView& n){
        std::size_t count=0;if(!common_vector(n,0,count) || !number(n,"threshold",0,threshold_) || !flag(n,"endpoints",false,endpoints_) || !number(n,"origin",0,origin_) || !number(n,"step",1,step_) || step_<=0)return false;
        input_.resize(count);positions_.resize(count);heights_.resize(count);offsets_.resize(count);return true;
    }
    std::size_t workspace_bytes() const noexcept override {return (input_.capacity()+positions_.capacity()+heights_.capacity()+offsets_.capacity())*sizeof(float);}
private:
    bool run(std::span<const NumericOperationInput> in,std::span<NumericOperationOutput> out) noexcept override {
        std::span<const float> input;std::size_t count=0;if(!common_values(in[0],input_,input))return false;
        auto offsets=std::span(offsets_).first(input.size());
        if(!interpolated_local_peaks(input,threshold_,endpoints_,positions_,heights_,offsets,count))return false;
        for(std::size_t i=0;i<count;++i)positions_[i]=origin_+positions_[i]*step_;
        return write_values(out[0],std::span(positions_).first(count)) && write_values(out[1],std::span(heights_).first(count)) && write_values(out[2],offsets);
    }
    float threshold_=0,origin_=0,step_=1;bool endpoints_=false;std::vector<float> input_,positions_,heights_,offsets_;
};
domain::ProcessingSupport interpolation_support(const dataflow_runtime::NumericOperationNodeView& n,const dataflow_runtime::NumericOperationProfile& p) noexcept {
    std::size_t count=0,queries=0,bytes=0;CurveInterpolationConfig c;domain::ProcessingSupport s;
    const auto coordinates=array(n,"coordinates");
    if(!common_vector(n,0,count) || !common_vector(n,1,queries) || coordinates.size()!=count || !valid_coordinates(coordinates) || !interpolation_config(n,c) ||
       !PreparedCurveInterpolation::storage_bytes(count,c,bytes) || !prepared_storage::add_array(bytes,count,sizeof(float)) ||
       !prepared_storage::add_array(bytes,queries,2*sizeof(float)) || !common_output(n,0,Rank::Vector,queries))return {};
    common_resources(n,p,sizeof(Interpolation),bytes,s);return s;
}
domain::ProcessingSupport comb_support(const dataflow_runtime::NumericOperationNodeView& n,const dataflow_runtime::NumericOperationProfile& p) noexcept {
    CommonArrayGeometry geometry;std::size_t maximum=0,bytes=0;CombDirection direction;domain::ProcessingSupport support;
    if(!comb_input(n,geometry) || !comb_config(n,maximum,direction))return {};
    const auto type=contract(n,domain::PortDirection::Input,0)->dtype;const auto filters=array(n,"delays").size();
    const auto precision=type==DType::Float64?NumericalPrecision::DesktopReference:NumericalPrecision::PortableFloat32;
    if(!PreparedCombFilterBank::storage_bytes(filters,maximum,direction,1,precision,bytes) ||
       !prepared_storage::add_array(bytes,filters,common_array_element_bytes(type)) ||
       !common_output(n,0,Rank::Matrix,geometry.rows,filters,type) || !common_output(n,1,Rank::Vector,filters))return {};
    const auto object=type==DType::Float64?sizeof(Comb<double>):sizeof(Comb<float>);
    common_resources(n,p,object,bytes,support);return support;
}

domain::ProcessingSupport histogram_support(const dataflow_runtime::NumericOperationNodeView& n,const dataflow_runtime::NumericOperationProfile& p) noexcept {
    std::size_t count=0,bytes=0;HistogramConfig c;domain::ProcessingSupport s;
    if(!common_vector(n,0,count) || !histogram_config(n,c) || !prepared_storage::add_array(bytes,count,sizeof(float)) ||
       !prepared_storage::add_array(bytes,c.bins,sizeof(float)+sizeof(std::uint64_t)) || !common_output(n,0,Rank::Vector,c.bins,1,DType::UInt64) || !common_output(n,1,Rank::Vector,c.bins))return {};
    common_resources(n,p,sizeof(Histogram),bytes,s);return s;
}
domain::ProcessingSupport peaks_support(const dataflow_runtime::NumericOperationNodeView& n,const dataflow_runtime::NumericOperationProfile& p) noexcept {
    std::size_t count=0,bytes=0;float threshold=0,origin=0,step=1;bool endpoints=false;domain::ProcessingSupport s;
    if(!common_vector(n,0,count) || !number(n,"threshold",0,threshold) || !number(n,"origin",0,origin) || !number(n,"step",1,step) || step<=0 || !flag(n,"endpoints",false,endpoints) ||
       !prepared_storage::add_array(bytes,count,4*sizeof(float)) || !common_output(n,0,Rank::Vector,count) || !common_output(n,1,Rank::Vector,count) || !common_output(n,2,Rank::Vector,count))return {};
    common_resources(n,p,sizeof(Peaks),bytes,s);return s;
}
template<class T> PreparedCommon make(const dataflow_runtime::NumericOperationNodeView& n){auto result=std::make_unique<T>();return result->prepare(n)?std::move(result):nullptr;}
}
bool linear_expansion_actual(const dataflow_runtime::NumericOperationNodeView& node,std::size_t count,std::size_t& result) noexcept {
    std::size_t factor=0;return integer(node,"factor",2,factor) && endpoint_expansion_size(count,factor,result);
}
PreparedCommon prepare_common_sequences(const dataflow_runtime::NumericOperationNodeView& n){
    if(n.kind=="LinearExpansion")return make<LinearExpansion>(n);
    if(n.kind=="CurveInterpolation")return make<Interpolation>(n);
    if(n.kind=="CombFilterBank")return contract(n,domain::PortDirection::Input,0)->dtype==DType::Float64?make<Comb<double>>(n):make<Comb<float>>(n);
    if(n.kind=="ValueHistogram")return make<Histogram>(n);
    if(n.kind=="InterpolatedPeaks")return make<Peaks>(n);
    return {};
}
domain::ProcessingSupport assess_common_sequences(const dataflow_runtime::NumericOperationNodeView& n,const dataflow_runtime::NumericOperationProfile& p) noexcept {
    if(n.kind=="LinearExpansion")return expansion_support(n,p);
    if(n.kind=="CurveInterpolation")return interpolation_support(n,p);
    if(n.kind=="CombFilterBank")return comb_support(n,p);
    if(n.kind=="ValueHistogram")return histogram_support(n,p);
    return peaks_support(n,p);
}
}
