// SPDX-License-Identifier: AGPL-3.0-or-later
#include "interval_operations.h"
#include "operation_io.h"
#include "parameter_values.h"
#include <algorithm>
#include <cmath>

namespace rhythm_graph_adapters::detail {
using namespace rhythm_reference_ports;
using namespace dataflow_runtime;
using namespace signal_contracts;
namespace {
bool harmonics_requirements(const domain::ResolvedProcessingNode& node,std::size_t maximum,std::size_t& bytes) noexcept {
    HarmonicTempoConfig config;
    return scalar_parameter(node,"reference_bpm",60,config.reference_bpm) &&
        scalar_parameter(node,"minimum_common_bpm",20,config.minimum_common_bpm) &&
        scalar_parameter(node,"tolerance_percent",5,config.tolerance_percent) &&
        PreparedHarmonicTempoCandidates::required_workspace_bytes(maximum,config,bytes) &&
        node.outputs.size()==1 && output_shape(node,0,Rank::Vector,maximum,1,true);
}
bool sections_requirements(const domain::ResolvedProcessingNode& node,std::size_t maximum,std::size_t& bytes) noexcept {
    TempoChangeSectionConfig config;
    return scalar_parameter(node,"tolerance",.08f,config.tolerance) &&
        scalar_parameter(node,"long_region_seconds",20,config.long_region_seconds) &&
        scalar_parameter(node,"short_region_seconds",4,config.short_region_seconds) &&
        PreparedTempoChangeSections::required_workspace_bytes(maximum,config,bytes) && node.outputs.size()==2 &&
        output_shape(node,0,Rank::Vector,maximum,1,true) && output_shape(node,1,Rank::Vector,maximum,1,true);
}
bool fluctuation_requirements(const domain::ResolvedProcessingNode& node,std::size_t& bytes) noexcept {
    const auto scales=array_parameter(node,"scales");
    if(scales.empty() || !std::is_sorted(scales.begin(),scales.end()))return false;
    for(float value:scales)if(!std::isfinite(value) || value<2 || value>=float(INT32_MAX) || value!=std::floor(value))return false;
    return PreparedDetrendedFluctuation::required_workspace_bytes(scales.size(),bytes) &&
        node.outputs.size()==4 && output_shape(node,0,Rank::Scalar,1) && output_shape(node,1,Rank::Vector,scales.size()) &&
        output_shape(node,2,Rank::Vector,scales.size()-1) && output_shape(node,3,Rank::Scalar,1);
}
bool kernel_requirements(const domain::ResolvedProcessingNode& node,std::size_t maximum,std::size_t& bytes) noexcept {
    bytes=0;
    if(node.kind=="HarmonicTempoCandidates")return harmonics_requirements(node,maximum,bytes);
    if(node.kind=="TempoChangeSections")return sections_requirements(node,maximum,bytes);
    if(node.kind=="DetrendedFluctuation")return fluctuation_requirements(node,bytes);
    return node.kind=="BeatIntervalHistogram" && node.outputs.size()==2 &&
        output_shape(node,0,Rank::Vector,250) && output_shape(node,1,Rank::Matrix,2,3);
}
bool admitted_ports(const domain::ResolvedProcessingNode& node,const FrameLimits& limits) noexcept {
    for(const auto& input:node.inputs)if(validate_contract(input.contract.view(),limits)!=Status::ok)return false;
    for(const auto& output:node.outputs){
        if(validate_contract(output.contract.view(),limits)!=Status::ok)return false;
        const auto data=output.contract.view();const auto* value=std::get_if<NumericContract>(&data);
        if(!value || !index_output_contract(*value))return false;
    }
    return true;
}
}
domain::ProcessingSupport assess_interval_operation(const domain::ResolvedProcessingNode& node,const NumericRuntimeProfile& profile) noexcept {
    domain::ProcessingSupport support;std::size_t maximum=0;
    if(node.inputs.size()!=1 || !vector_capacity(node,domain::PortDirection::Input,0,maximum) || !maximum ||
       !admitted_ports(node,profile.admission.frame) || !kernel_requirements(node,maximum,support.resources.scratch_bytes))return support;
    if(!numeric_operation_descriptor_required_bytes(node,profile.admission,support.resources.plan_bytes) ||
       !numeric_operation_add_bytes(support.resources.plan_bytes,sizeof(IntervalOperation)))return {};
    support.resources.stack_bytes=dsp_kernels::numeric_reference_stack_bytes;support.supported=true;return support;
}
}
