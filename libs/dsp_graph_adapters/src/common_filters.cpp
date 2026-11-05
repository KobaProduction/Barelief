#include "common_support.h"
#include <dsp_kernels/mel_filter_bank.h>
#include <dsp_kernels/recursive_filter.h>
#include <algorithm>
#include <cmath>

namespace dsp_graph_adapters::detail {
using namespace signal_contracts;using namespace dsp_kernels;
namespace {
constexpr float identity[]{1};
std::span<const float> coefficients(const domain::ResolvedProcessingNode& n,std::string_view key) noexcept {
    const auto value=array(n,key);return parameter(n,key)?value:std::span<const float>(identity);
}
struct RecursiveConfig {RecursiveFilterOptions options;SubnormalStatePolicy cleanup;};
bool recursive_config(const domain::ResolvedProcessingNode& n,RecursiveConfig& c) noexcept {
    std::size_t form=0,cleanup=1;
    if(!integer(n,"form",0,form) || form>1 || !integer(n,"subnormal",1,cleanup) || cleanup>2 || !number(n,"input_zero_threshold",0,c.options.input_zero_threshold) || c.options.input_zero_threshold<0)return false;
    c.options.form=RecursiveFilterForm(form);c.cleanup=SubnormalStatePolicy(cleanup);
    const auto a=coefficients(n,"denominator"),b=coefficients(n,"numerator");
    if(a.empty() || b.empty() || !std::isfinite(a[0]) || a[0]==0)return false;
    const auto finite=[divisor=a[0]](float v){return std::isfinite(v/divisor);};
    return std::all_of(a.begin(),a.end(),finite) && std::all_of(b.begin(),b.end(),finite);
}
class Recursive final:public CommonKernel<Recursive> {
public:
    bool prepare(const domain::ResolvedProcessingNode& n){
        std::size_t count=0;RecursiveConfig c;
        if(!common_vector(n,0,count) || !recursive_config(n,c) || !kernel_.prepare(coefficients(n,"numerator"),coefficients(n,"denominator"),c.cleanup,c.options))return false;
        input_.resize(count);output_.resize(count);return true;
    }
    std::size_t workspace_bytes() const noexcept override {return kernel_.storage_bytes()+(input_.capacity()+output_.capacity())*sizeof(float);}
private:
    void reset_kernel() noexcept override {kernel_.reset();}
    bool run(std::span<const NumericOperationInput> in,std::span<NumericOperationOutput> out) noexcept override {
        std::span<const float> input;if(!common_values(in[0],input_,input))return false;
        auto result=std::span(output_).first(input.size());
        return kernel_.process(input,result) && write_values(out[0],result) && write_values(out[1],kernel_.state());
    }
    PreparedRecursiveFilter kernel_;std::vector<float> input_,output_;
};
struct ProjectionConfig {std::size_t rows=1,bins=0,bands=0;Rank rank=Rank::Vector;FilterProjectionOptions options;};
bool projection_options(const domain::ResolvedProcessingNode& n,FilterProjectionOptions& options) noexcept {
    const auto mode=parameter(n,"accumulation")?text(n,"accumulation"):std::string_view{"separate"};
    if(mode!="separate" && mode!="sequential_fma")return false;
    options.accumulation=mode=="sequential_fma"?FilterProjectionAccumulation::SequentialFma:FilterProjectionAccumulation::Separate;
    return flag(n,"compensated",true,options.compensated) && flag(n,"square_input",false,options.square_input) && valid_filter_projection_options(options);
}
bool projection_config(const domain::ResolvedProcessingNode& n,ProjectionConfig& c) noexcept {
    const auto input=contract(n,domain::PortDirection::Input,0);
    if(!input || input->dtype!=DType::Float32 || input->channel_order.size()!=1 || input->representation.encoding!=StorageEncoding::Native || input->representation.valid_bits || input->representation.layout!=ChannelLayout::Planar)return false;
    c.rank=input->shape.rank;
    if(c.rank!=Rank::Vector && c.rank!=Rank::Matrix)return false;
    const auto axis=c.rank==Rank::Matrix?1u:0u;
    if(input->shape.extents[axis].maximum>SIZE_MAX || input->shape.extents[0].maximum>SIZE_MAX)return false;
    c.bins=std::size_t(input->shape.extents[axis].maximum);c.rows=c.rank==Rank::Matrix?std::size_t(input->shape.extents[0].maximum):1;
    const auto weights=array(n,"weights"),centers=array(n,"centers_hz");
    if(!c.bins || weights.empty() || weights.size()%c.bins)return false;
    c.bands=weights.size()/c.bins;
    if(!centers.empty() && centers.size()!=c.bands)return false;
    return projection_options(n,c.options) &&
        std::all_of(weights.begin(),weights.end(),[](float v){return std::isfinite(v);}) &&
        std::all_of(centers.begin(),centers.end(),[](float v){return std::isfinite(v) && v>=0;});
}
class Projection final:public CommonKernel<Projection> {
public:
    bool prepare(const domain::ResolvedProcessingNode& n){
        if(!projection_config(n,config_))return false;
        const auto centers=array(n,"centers_hz");std::vector<double> coordinates(centers.begin(),centers.end());
        if(!kernel_.prepare_matrix(config_.bins,config_.bands,array(n,"weights"),coordinates,config_.options))return false;
        input_.resize(config_.bins);output_.resize(config_.bands);return true;
    }
    std::size_t workspace_bytes() const noexcept override {return kernel_.workspace_bytes()+(input_.capacity()+output_.capacity())*sizeof(float);}
private:
    bool project_rows(const NumericOperationInput& input,NumericOperationOutput& target) noexcept {
        const auto rows=config_.rank==Rank::Matrix?input.value.actual[0]:1;
        if((config_.rank==Rank::Matrix?input.value.actual[1]:input.value.actual[0])!=config_.bins || !set_shape(target,config_.rank==Rank::Matrix?rows:config_.bands,config_.rank==Rank::Matrix?config_.bands:1))return false;
        for(std::size_t row=0;row<rows;++row){
            for(std::size_t bin=0;bin<config_.bins;++bin)if(load_element(input.value,{0,config_.rank==Rank::Matrix?row:bin,config_.rank==Rank::Matrix?bin:0},input_[bin])!=Status::ok)return false;
            if(!kernel_.process(input_,output_))return false;
            for(std::size_t band=0;band<config_.bands;++band)if(store_element(target.value,{0,config_.rank==Rank::Matrix?row:band,config_.rank==Rank::Matrix?band:0},output_[band])!=Status::ok)return false;
        }
        return true;
    }
    bool run(std::span<const NumericOperationInput> in,std::span<NumericOperationOutput> out) noexcept override {
        if(!project_rows(in[0],out[0]) || !set_shape(out[1],config_.bands,config_.bins))return false;
        const auto axis=config_.rank==Rank::Matrix?1u:0u;
        if(out[0].value.contract->coordinates[axis].mode==CoordinateMode::ExplicitFloat64LE){
            for(std::size_t band=0;band<kernel_.centers_hz().size();++band)
                if(dataflow_runtime::numeric_operation_coordinate(out[0],axis,band,kernel_.centers_hz()[band])!=Status::ok)return false;
        }
        const auto weights=kernel_.weights();
        for(std::size_t band=0;band<config_.bands;++band)for(std::size_t bin=0;bin<config_.bins;++bin)
            if(store_element(out[1].value,{0,band,bin},weights[band*config_.bins+bin])!=Status::ok)return false;
        const auto centers=kernel_.centers_hz();
        if(!set_shape(out[2],centers.size()))return false;
        for(std::size_t band=0;band<centers.size();++band)if(store_element(out[2].value,{0,band,0},float(centers[band]))!=Status::ok)return false;
        return true;
    }
    ProjectionConfig config_;MelFilterBank kernel_;std::vector<float> input_,output_;
};
domain::ProcessingSupport recursive_support(const domain::ResolvedProcessingNode& n,const dataflow_runtime::NumericRuntimeProfile& p) noexcept {
    std::size_t count=0,bytes=0;RecursiveConfig c;domain::ProcessingSupport support;
    const auto a=coefficients(n,"denominator"),b=coefficients(n,"numerator");
    const auto states=std::max(a.size(),b.size());
    if(!common_vector(n,0,count) || !recursive_config(n,c) || !PreparedRecursiveFilter::storage_bytes(b.size(),a.size(),c.options,bytes) ||
       !prepared_storage::add_array(bytes,count,2*sizeof(float)) || !common_output(n,0,Rank::Vector,count) ||
       !common_output(n,1,Rank::Vector,states*(c.options.form==RecursiveFilterForm::DirectI?2:1)))return {};
    common_resources(n,p,sizeof(Recursive),bytes,support);return support;
}
domain::ProcessingSupport projection_support(const domain::ResolvedProcessingNode& n,const dataflow_runtime::NumericRuntimeProfile& p) noexcept {
    ProjectionConfig c;std::size_t bytes=0;domain::ProcessingSupport support;
    if(!projection_config(n,c) || !MelFilterBank::matrix_storage_bytes(c.bins,c.bands,bytes) ||
       !prepared_storage::add_array(bytes,c.bins,sizeof(float)) || !prepared_storage::add_array(bytes,c.bands,sizeof(float)+sizeof(double)) ||
       !common_output(n,0,c.rank,c.rank==Rank::Matrix?c.rows:c.bands,c.rank==Rank::Matrix?c.bands:1) ||
       !common_output(n,1,Rank::Matrix,c.bands,c.bins) || !common_output(n,2,Rank::Vector,c.bands))return {};
    common_resources(n,p,sizeof(Projection),bytes,support);return support;
}
}
PreparedCommon prepare_common_filters(const domain::ResolvedProcessingNode& n){
    if(n.kind=="RecursiveFilter"){auto value=std::make_unique<Recursive>();return value->prepare(n)?std::move(value):nullptr;}
    if(n.kind=="FilterMatrixProjection"){auto value=std::make_unique<Projection>();return value->prepare(n)?std::move(value):nullptr;}
    return {};
}
domain::ProcessingSupport assess_common_filters(const domain::ResolvedProcessingNode& n,const dataflow_runtime::NumericRuntimeProfile& p) noexcept {
    return n.kind=="RecursiveFilter"?recursive_support(n,p):projection_support(n,p);
}
}
