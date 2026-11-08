#include "common_array.h"
#include <dsp_kernels/mel_filter_bank.h>
#include <dsp_kernels/recursive_filter.h>
#include <algorithm>
#include <cmath>

namespace dsp_graph_adapters::detail {
using namespace signal_contracts;using namespace dsp_kernels;
namespace {
constexpr float identity[]{1};
std::span<const float> coefficients(const dataflow_runtime::NumericOperationNodeView& n,std::string_view key) noexcept {
    const auto value=array(n,key);return parameter(n,key)?value:std::span<const float>(identity);
}
struct RecursiveConfig {RecursiveFilterOptions options;SubnormalStatePolicy cleanup;std::size_t state_policy=0;};
bool recursive_config(const dataflow_runtime::NumericOperationNodeView& n,RecursiveConfig& c) noexcept {
    std::size_t form=0,cleanup=1;
    if(!integer(n,"form",0,form) || form>1 || !integer(n,"subnormal",1,cleanup) || cleanup>2 || !number(n,"input_zero_threshold",0,c.options.input_zero_threshold) || c.options.input_zero_threshold<0)return false;
    c.options.form=RecursiveFilterForm(form);c.cleanup=SubnormalStatePolicy(cleanup);
    const auto a=coefficients(n,"denominator"),b=coefficients(n,"numerator");
    if(a.empty() || b.empty() || !std::isfinite(a[0]) || a[0]==0)return false;
    const auto finite=[divisor=a[0]](float v){return std::isfinite(v/divisor);};
    return std::all_of(a.begin(),a.end(),finite) && std::all_of(b.begin(),b.end(),finite) &&
        integer(n,"state_policy",0,c.state_policy) && c.state_policy<=2;
}
std::size_t recursive_states(const dataflow_runtime::NumericOperationNodeView& n,const RecursiveConfig& config) noexcept {
    return std::max(coefficients(n,"numerator").size(),coefficients(n,"denominator").size())*
        (config.options.form==RecursiveFilterForm::DirectI?2:1);
}
bool recursive_initial_state(const dataflow_runtime::NumericOperationNodeView& n,const RecursiveConfig& config) noexcept {
    if(n.inputs.size()==1)return true;
    std::size_t count=0;
    return n.inputs.size()==2 && common_vector(n,1,count) && count==recursive_states(n,config);
}
class Recursive final:public CommonKernel<Recursive> {
public:
    bool prepare(const dataflow_runtime::NumericOperationNodeView& n){
        if(!common_array_geometry(n,0,geometry_,true) || !recursive_config(n,config_) ||
           !recursive_initial_state(n,config_) ||
           !kernel_.prepare(coefficients(n,"numerator"),coefficients(n,"denominator"),config_.cleanup,config_.options))return false;
        input_.resize(geometry_.width());output_.resize(geometry_.width());
        state_.resize(n.inputs.size()==2?recursive_states(n,config_):0);return true;
    }
    std::size_t workspace_bytes() const noexcept override {
        return kernel_.storage_bytes()+(input_.capacity()+output_.capacity()+state_.capacity())*sizeof(float);
    }
private:
    void reset_kernel() noexcept override {kernel_.reset();}
    bool initialize(std::span<const NumericOperationInput> inputs) noexcept {
        if(config_.state_policy)kernel_.reset();
        if(inputs.size()==1)return true;
        std::span<const float> state;
        return common_values(inputs[1],state_,state) && kernel_.set_state(state);
    }
    bool row(const NumericBufferView& input,MutableNumericBufferView& output,std::size_t at) noexcept {
        if(config_.state_policy==2){
            kernel_.reset();if(!state_.empty() && !kernel_.set_state(state_))return false;
        }
        auto values=std::span(input_).first(input.actual[geometry_.axis]);
        auto result=std::span(output_).first(values.size());
        return common_array_read(input,at,values,geometry_) && kernel_.process(values,result) &&
            common_array_write(output,at,result,geometry_);
    }
    bool run(std::span<const NumericOperationInput> in,std::span<NumericOperationOutput> out) noexcept override {
        if(!initialize(in))return false;
        out[0].value.actual=in[0].value.actual;
        for(std::size_t at=0;at<common_array_rows(in[0].value,geometry_);++at)
            if(!row(in[0].value,out[0].value,at))return false;
        return write_values(out[1],kernel_.state());
    }
    CommonArrayGeometry geometry_;RecursiveConfig config_;
    PreparedRecursiveFilter kernel_;std::vector<float> input_,output_,state_;
};
struct ProjectionConfig {std::size_t rows=1,bins=0,bands=0;Rank rank=Rank::Vector;FilterProjectionOptions options;};
bool projection_options(const dataflow_runtime::NumericOperationNodeView& n,FilterProjectionOptions& options) noexcept {
    const auto mode=parameter(n,"accumulation")?text(n,"accumulation"):std::string_view{"separate"};
    if(mode!="separate" && mode!="sequential_fma")return false;
    options.accumulation=mode=="sequential_fma"?FilterProjectionAccumulation::SequentialFma:FilterProjectionAccumulation::Separate;
    return flag(n,"compensated",true,options.compensated) && flag(n,"square_input",false,options.square_input) && valid_filter_projection_options(options);
}
bool projection_geometry(const dataflow_runtime::NumericOperationNodeView& n,ProjectionConfig& c) noexcept {
    const auto input=contract(n,domain::PortDirection::Input,0);
    if(!input || input->dtype!=DType::Float32 || input->plane_count!=1 || input->representation.encoding!=StorageEncoding::Native || input->representation.valid_bits || input->representation.layout!=ChannelLayout::Planar)return false;
    c.rank=input->shape.rank;if(c.rank!=Rank::Vector && c.rank!=Rank::Matrix)return false;
    const auto axis=c.rank==Rank::Matrix?1u:0u;
    if(input->shape.extents[axis].maximum>SIZE_MAX || input->shape.extents[0].maximum>SIZE_MAX)return false;
    c.bins=std::size_t(input->shape.extents[axis].maximum);c.rows=c.rank==Rank::Matrix?std::size_t(input->shape.extents[0].maximum):1;
    return true;
}
bool projection_config(const dataflow_runtime::NumericOperationNodeView& n,ProjectionConfig& c) noexcept {
    if(!projection_geometry(n,c))return false;
    const auto weights=array(n,"weights"),centers=array(n,"centers_hz");
    if(!c.bins || weights.empty() || weights.size()%c.bins)return false;
    c.bands=weights.size()/c.bins;if(!centers.empty() && centers.size()!=c.bands)return false;
    return projection_options(n,c.options) &&
        std::all_of(weights.begin(),weights.end(),[](float v){return std::isfinite(v);}) &&
        std::all_of(centers.begin(),centers.end(),[](float v){return std::isfinite(v) && v>=0;});
}
class Projection final:public CommonKernel<Projection> {
public:
    bool prepare(const dataflow_runtime::NumericOperationNodeView& n){
        if(!projection_config(n,config_))return false;
        const auto centers=array(n,"centers_hz");std::vector<double> coordinates(centers.begin(),centers.end());
        if(!kernel_.prepare_matrix(config_.bins,config_.bands,array(n,"weights"),coordinates,config_.options))return false;
        input_.resize(config_.bins);output_.resize(config_.bands);return true;
    }
    std::size_t workspace_bytes() const noexcept override {return kernel_.workspace_bytes()+(input_.capacity()+output_.capacity())*sizeof(float);}
private:
    bool read_projection_row(const NumericOperationInput& input,std::size_t row) noexcept {
        const bool matrix=config_.rank==Rank::Matrix;
        for(std::size_t bin=0;bin<config_.bins;++bin)
            if(load_element(input.value,{0,matrix?row:bin,matrix?bin:0},input_[bin])!=Status::ok)return false;
        return true;
    }
    bool write_projection_row(NumericOperationOutput& target,std::size_t row) const noexcept {
        const bool matrix=config_.rank==Rank::Matrix;
        for(std::size_t band=0;band<config_.bands;++band)
            if(store_element(target.value,{0,matrix?row:band,matrix?band:0},output_[band])!=Status::ok)return false;
        return true;
    }
    bool project_rows(const NumericOperationInput& input,NumericOperationOutput& target) noexcept {
        const auto rows=config_.rank==Rank::Matrix?input.value.actual[0]:1;
        if((config_.rank==Rank::Matrix?input.value.actual[1]:input.value.actual[0])!=config_.bins || !set_shape(target,config_.rank==Rank::Matrix?rows:config_.bands,config_.rank==Rank::Matrix?config_.bands:1))return false;
        for(std::size_t row=0;row<rows;++row)
            if(!read_projection_row(input,row) || !kernel_.process(input_,output_) || !write_projection_row(target,row))return false;
        return true;
    }
    bool publish_weights(NumericOperationOutput& target) const noexcept {
        if(!set_shape(target,config_.bands,config_.bins))return false;
        const auto weights=kernel_.weights();
        for(std::size_t band=0;band<config_.bands;++band)for(std::size_t bin=0;bin<config_.bins;++bin)
            if(store_element(target.value,{0,band,bin},weights[band*config_.bins+bin])!=Status::ok)return false;
        return true;
    }
    bool publish_centers(NumericOperationOutput& target) const noexcept {
        const auto centers=kernel_.centers_hz();if(!set_shape(target,centers.size()))return false;
        for(std::size_t band=0;band<centers.size();++band)
            if(store_element(target.value,{0,band,0},float(centers[band]))!=Status::ok)return false;
        return true;
    }
    bool run(std::span<const NumericOperationInput> in,std::span<NumericOperationOutput> out) noexcept override {
        return project_rows(in[0],out[0]) && publish_weights(out[1]) && publish_centers(out[2]);
    }
    ProjectionConfig config_;MelFilterBank kernel_;std::vector<float> input_,output_;
};
domain::ProcessingSupport recursive_support(const dataflow_runtime::NumericOperationNodeView& n,const dataflow_runtime::NumericOperationProfile& p) noexcept {
    CommonArrayGeometry geometry;RecursiveConfig config;std::size_t bytes=0;domain::ProcessingSupport support;
    if(!common_array_geometry(n,0,geometry,true) || !recursive_config(n,config) || !recursive_initial_state(n,config))return {};
    const auto a=coefficients(n,"denominator"),b=coefficients(n,"numerator");
    const auto states=recursive_states(n,config);
    if(!PreparedRecursiveFilter::storage_bytes(b.size(),a.size(),config.options,bytes) ||
       !prepared_storage::add_array(bytes,geometry.width(),2*sizeof(float)) ||
       !prepared_storage::add_array(bytes,n.inputs.size()==2?states:0,sizeof(float)) ||
       !common_output(n,0,geometry.rank,geometry.rows,geometry.columns) ||
       !common_output(n,1,Rank::Vector,states))return {};
    common_resources(n,p,sizeof(Recursive),bytes,support);return support;
}

domain::ProcessingSupport projection_support(const dataflow_runtime::NumericOperationNodeView& n,const dataflow_runtime::NumericOperationProfile& p) noexcept {
    ProjectionConfig c;std::size_t bytes=0;domain::ProcessingSupport support;
    if(!projection_config(n,c) || !MelFilterBank::matrix_storage_bytes(c.bins,c.bands,bytes) ||
       !prepared_storage::add_array(bytes,c.bins,sizeof(float)) || !prepared_storage::add_array(bytes,c.bands,sizeof(float)+sizeof(double)) ||
       !common_output(n,0,c.rank,c.rank==Rank::Matrix?c.rows:c.bands,c.rank==Rank::Matrix?c.bands:1) ||
       !common_output(n,1,Rank::Matrix,c.bands,c.bins) || !common_output(n,2,Rank::Vector,c.bands))return {};
    common_resources(n,p,sizeof(Projection),bytes,support);return support;
}
}
PreparedCommon prepare_common_filters(const dataflow_runtime::NumericOperationNodeView& n){
    if(n.kind=="RecursiveFilter"){auto value=std::make_unique<Recursive>();return value->prepare(n)?std::move(value):nullptr;}
    if(n.kind=="FilterMatrixProjection"){auto value=std::make_unique<Projection>();return value->prepare(n)?std::move(value):nullptr;}
    return {};
}
domain::ProcessingSupport assess_common_filters(const dataflow_runtime::NumericOperationNodeView& n,const dataflow_runtime::NumericOperationProfile& p) noexcept {
    return n.kind=="RecursiveFilter"?recursive_support(n,p):projection_support(n,p);
}
}
