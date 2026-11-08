#include "spectral_sequence.h"
#include <algorithm>
#include <cmath>

namespace dsp_graph_adapters::detail {
using namespace dsp_kernels;
using namespace signal_contracts;
namespace {
bool signed_setting(const dataflow_runtime::NumericOperationNodeView& n,std::string_view key,std::ptrdiff_t& output) noexcept {
    float value=0;if(!number(n,key,0,value) || value!=std::trunc(value) || value<-65536 || value>65536)return false;
    output=std::ptrdiff_t(value);return true;
}
bool novelty_settings(const dataflow_runtime::NumericOperationNodeView& n,SpectralNoveltyConfig& c) noexcept {
    std::size_t method=2;
    if(!integer(n,"method",2,method) || method>11 || !integer(n,"lag",1,c.lag) ||
       !integer(n,"frequency_maximum",3,c.frequency_maximum) || !integer(n,"temporal_maximum",3,c.temporal_maximum) ||
       !signed_setting(n,"temporal_origin",c.temporal_origin) || !integer(n,"alignment_radius",1,c.alignment_radius))return false;
    c.method=SpectralNoveltyMethod(method);
    return number(n,"epsilon",2.220446049250313e-16f,c.epsilon) && flag(n,"supplied_differences",false,c.supplied_differences) &&
        flag(n,"positive_alignment",false,c.positive_alignment);
}
}
bool spectral_supports(const dataflow_runtime::NumericOperationNodeView& n,std::size_t features,std::size_t bins) noexcept {
    const auto first=array(n,"support_first"),count=array(n,"support_count");
    if(first.empty() && count.empty())return true;
    if(first.size()!=features || count.size()!=features)return false;
    for(std::size_t i=0;i<features;++i)
        if(!std::isfinite(first[i]) || !std::isfinite(count[i]) || first[i]<0 || count[i]<1 ||
           first[i]!=std::trunc(first[i]) || count[i]!=std::trunc(count[i]) || double(first[i])+count[i]>double(bins))return false;
    return true;
}
std::vector<FilterBandSupport> own_spectral_supports(const dataflow_runtime::NumericOperationNodeView& n) {
    const auto first=array(n,"support_first"),count=array(n,"support_count");std::vector<FilterBandSupport> result;result.reserve(first.size());
    for(std::size_t i=0;i<first.size();++i)result.push_back({std::size_t(first[i]),std::size_t(count[i])});
    return result;
}
namespace {
bool phase_geometry(const dataflow_runtime::NumericOperationNodeView& node,SpectralSequencePlan& plan) noexcept {
    plan.bins=plan.features;std::size_t frames=0;
    if(contract(node,domain::PortDirection::Input,1) && (!input_geometry(node,1,Rank::Matrix,frames,plan.bins) || frames!=plan.frames))return false;
    return !spectral_novelty_uses_phase(plan.novelty.method) || contract(node,domain::PortDirection::Input,1).has_value();
}
bool matrix_novelty_plan(const dataflow_runtime::NumericOperationNodeView& node,SpectralSequencePlan& plan) noexcept {
    if(!input_geometry(node,0,Rank::Matrix,plan.frames,plan.features) || !phase_geometry(node,plan))return false;
    const auto supports=array(node,"support_first").size();
    if(plan.novelty.method==SpectralNoveltyMethod::PhaseMaskedDifference && plan.features!=plan.bins && supports!=plan.features)return false;
    return spectral_supports(node,plan.features,plan.bins) &&
        PreparedSpectralNovelty::storage_bytes(plan.frames,plan.features,plan.bins,plan.novelty,supports,plan.kernel_bytes);
}
}
bool spectral_sequence_plan(const dataflow_runtime::NumericOperationNodeView& node,SpectralSequencePlan& plan) noexcept {
    plan={};if(node.kind!="SpectralNovelty")return false;
    if(!novelty_settings(node,plan.novelty))return false;
    if(!matrix_novelty_plan(node,plan))return false;
    plan.columns=plan.novelty.method==SpectralNoveltyMethod::AlignedDifference?plan.features:1;
    return true;
}
SpectralOutputShape spectral_output_shape(const SpectralSequencePlan& p,domain::PortId port) noexcept {
    if(!port)return {Rank::Matrix,std::max(std::size_t{1},p.frames),p.columns};
    const auto mapped=port;
    if(mapped==4)return {Rank::Scalar,1,1,DType::Int64};
    if(mapped==3)return {Rank::Vector,2*p.novelty.alignment_radius+1};
    return {Rank::Vector,mapped==2?p.bins:p.features};
}
bool spectral_sequence_outputs(const dataflow_runtime::NumericOperationNodeView& n,const SpectralSequencePlan& p) noexcept {
    for(domain::PortId i=0;i<n.outputs.size();++i){const auto shape=spectral_output_shape(p,i);
        if(!output_geometry(n,i,shape.rank,shape.rows,shape.columns,shape.dtype))return false;
    }
    return n.outputs.size()==5u;
}
}
