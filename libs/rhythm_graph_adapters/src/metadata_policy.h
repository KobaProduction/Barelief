// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include <dataflow_runtime/numeric_operation.h>

namespace rhythm_graph_adapters::detail {
void metadata_clock(dataflow_runtime::NumericOperationPolicy&,domain::PortDirection,domain::PortId,
    dataflow_runtime::NumericOperationClockKind,double rate=0,bool allow_unknown=true);
void metadata_index_axes(dataflow_runtime::NumericOperationPolicy&,domain::PortId);
void metadata_index_outputs(const dataflow_runtime::NumericOperationNodeView&,dataflow_runtime::NumericOperationPolicy&,bool clocked);
void metadata_match_input(dataflow_runtime::NumericOperationPolicy&,domain::PortId,std::size_t axes=2,bool compare_tags=false);
dataflow_runtime::NumericOperationAxisRule metadata_uniform(domain::PortId,std::size_t,signal_contracts::NumericUnit,double);
void add_metadata_policy(const dataflow_runtime::NumericOperationNodeView&,dataflow_runtime::NumericOperationPolicy&);
void feature_metadata_policy(const dataflow_runtime::NumericOperationNodeView&,dataflow_runtime::NumericOperationPolicy&);
void batch_metadata_policy(const dataflow_runtime::NumericOperationNodeView&,dataflow_runtime::NumericOperationPolicy&);
void tracking_metadata_policy(const dataflow_runtime::NumericOperationNodeView&,dataflow_runtime::NumericOperationPolicy&);
void loudness_metadata_policy(const dataflow_runtime::NumericOperationNodeView&,dataflow_runtime::NumericOperationPolicy&);
void pcm_metadata_policy(const dataflow_runtime::NumericOperationNodeView&,dataflow_runtime::NumericOperationPolicy&);
void btrack_metadata_policy(const dataflow_runtime::NumericOperationNodeView&,dataflow_runtime::NumericOperationPolicy&);
void essentia_metadata_policy(const dataflow_runtime::NumericOperationNodeView&,dataflow_runtime::NumericOperationPolicy&);
}
