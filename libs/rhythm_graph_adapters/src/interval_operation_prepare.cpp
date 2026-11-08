// SPDX-License-Identifier: AGPL-3.0-or-later
#include "interval_operations.h"
#include "operation_io.h"
#include "parameter_values.h"
#include <cmath>
#include <limits>

namespace rhythm_graph_adapters::detail {
using namespace rhythm_reference_ports;
using signal_contracts::Rank;
bool interval_operation_kind(std::string_view kind) noexcept {
    return kind=="BeatIntervalHistogram" || kind=="HarmonicTempoCandidates" || kind=="TempoChangeSections" || kind=="DetrendedFluctuation";
}
std::unique_ptr<AdapterOperation> prepare_interval_operation(const dataflow_runtime::NumericOperationNode& node) {
    if(!interval_operation_kind(node.kind))return {};
    auto operation=std::make_unique<IntervalOperation>();
    if(!operation->prepare(node))return {};
    return operation;
}
bool IntervalOperation::prepare_harmonics(const dataflow_runtime::NumericOperationNode& node) {
    HarmonicTempoConfig config;
    if(!scalar_parameter(node,"reference_bpm",60,config.reference_bpm) ||
       !scalar_parameter(node,"minimum_common_bpm",20,config.minimum_common_bpm) ||
       !scalar_parameter(node,"tolerance_percent",5,config.tolerance_percent))return false;
    kind_=IntervalOperationKind::Harmonics;
    return kernel_.emplace<PreparedHarmonicTempoCandidates>().prepare(maximum_,config);
}
bool IntervalOperation::prepare_sections(const dataflow_runtime::NumericOperationNode& node) {
    TempoChangeSectionConfig config;
    if(!scalar_parameter(node,"tolerance",.08f,config.tolerance) ||
       !scalar_parameter(node,"long_region_seconds",20,config.long_region_seconds) ||
       !scalar_parameter(node,"short_region_seconds",4,config.short_region_seconds))return false;
    kind_=IntervalOperationKind::Sections;
    return kernel_.emplace<PreparedTempoChangeSections>().prepare(maximum_,config);
}
bool IntervalOperation::prepare_fluctuation(const dataflow_runtime::NumericOperationNode& node) {
    const auto values=array_parameter(node,"scales");std::vector<std::size_t> scales;
    scales.reserve(values.size());
    for(float value:values){
        if(!std::isfinite(value) || value<2 || value>=float(INT32_MAX) || value!=std::floor(value))return false;
        scales.push_back(std::size_t(value));
    }
    kind_=IntervalOperationKind::Fluctuation;
    return kernel_.emplace<PreparedDetrendedFluctuation>().prepare(scales);
}
bool IntervalOperation::output_geometry(const dataflow_runtime::NumericOperationNode& node) const noexcept {
    if(kind_==IntervalOperationKind::Histogram)
        return node.outputs.size()==2 && output_shape(node,0,Rank::Vector,250) && output_shape(node,1,Rank::Matrix,2,3);
    if(kind_==IntervalOperationKind::Harmonics)return node.outputs.size()==1 && output_shape(node,0,Rank::Vector,maximum_,1,true);
    if(kind_==IntervalOperationKind::Sections)
        return node.outputs.size()==2 && output_shape(node,0,Rank::Vector,maximum_,1,true) && output_shape(node,1,Rank::Vector,maximum_,1,true);
    const auto count=std::get<PreparedDetrendedFluctuation>(kernel_).scales().size();
    return node.outputs.size()==4 && output_shape(node,0,Rank::Scalar,1) && output_shape(node,1,Rank::Vector,count) &&
        output_shape(node,2,Rank::Vector,count-1) && output_shape(node,3,Rank::Scalar,1);
}
bool IntervalOperation::prepare(const dataflow_runtime::NumericOperationNode& node) {
    if(node.inputs.size()!=1 || !vector_capacity(node,domain::PortDirection::Input,0,maximum_) || !maximum_)return false;
    bool prepared=node.kind=="BeatIntervalHistogram";
    if(node.kind=="HarmonicTempoCandidates")prepared=prepare_harmonics(node);
    if(node.kind=="TempoChangeSections")prepared=prepare_sections(node);
    if(node.kind=="DetrendedFluctuation")prepared=prepare_fluctuation(node);
    return prepared && output_geometry(node);
}
}
