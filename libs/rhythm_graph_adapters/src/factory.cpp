// SPDX-License-Identifier: AGPL-3.0-or-later
#include <rhythm_graph_adapters/factory.h>
#include "adapter_operation.h"
#include "metadata_policy.h"
#include "parameter_values.h"
#include "spectral_operation_plan.h"
#include "descriptor_operations.h"
#include "event_operations.h"
#include "buffered_operations.h"
#include "contrast_operations.h"
#include "pcm_stage_operations.h"
#include "batch_operation_plan.h"
#include "tracking_operations.h"
#include "loudness_operations.h"
#include "sequence_source_operations.h"
#include "btrack_operations.h"
#include "essentia_operations.h"

namespace rhythm_graph_adapters {
namespace {
using namespace dataflow_runtime;
using namespace signal_contracts;
NumericOperationPolicy interval_policy(std::size_t outputs) {
    NumericOperationPolicy policy;policy.mode=NumericOperationMode::IndependentBatch;
    policy.match=NumericInputMatch::IndependentBatch;policy.arithmetic_failure=NumericOperationFailure::ResetState;
    policy.inputs.reserve(1);policy.inputs.push_back({0,NumericInputUse::Frame,true,ValueDomain::FiniteOnly});
    policy.output_values.reserve(outputs);
    for(std::size_t i=0;i<outputs;++i)policy.output_values.push_back(ValueDomain::FiniteOnly);
    return policy;
}
bool within_bound(const domain::ProcessingResources& actual,const domain::ProcessingResources& bound) noexcept {
    return actual.plan_bytes<=bound.plan_bytes && actual.payload_bytes<=bound.payload_bytes &&
        actual.scratch_bytes<=bound.scratch_bytes && actual.stack_bytes<=bound.stack_bytes;
}
bool feature_kind(std::string_view kind) noexcept {
    return detail::spectral_operation_kind(kind) || detail::descriptor_operation_kind(kind) ||
        detail::event_operation_kind(kind) || detail::buffered_operation_kind(kind) || detail::contrast_operation_kind(kind);
}
bool sequence_kind(std::string_view kind) noexcept {
    return detail::pcm_stage_operation_kind(kind) || detail::batch_operation_kind(kind) || detail::tracking_operation_kind(kind) ||
        detail::loudness_operation_kind(kind) || detail::sequence_source_kind(kind);
}
bool source_operation_kind(std::string_view kind) noexcept {
    return detail::btrack_operation_kind(kind) || detail::essentia_operation_kind(kind) ||
        detail::interval_operation_kind(kind) || feature_kind(kind) || sequence_kind(kind);
}
NumericOperationPolicy feature_policy(const dataflow_runtime::NumericOperationNodeView& node) {
    if(detail::spectral_operation_kind(node.kind))return detail::spectral_operation_policy(node);
    if(detail::descriptor_operation_kind(node.kind))return detail::descriptor_operation_policy(node);
    if(detail::event_operation_kind(node.kind))return detail::event_operation_policy(node);
    if(detail::buffered_operation_kind(node.kind))return detail::buffered_operation_policy(node);
    return detail::contrast_operation_policy(node);
}
NumericOperationPolicy batch_policy(const dataflow_runtime::NumericOperationNodeView& node) {
    if(detail::loudness_operation_kind(node.kind)){
        detail::LoudnessPlan plan;if(!detail::loudness_operation_plan(node,plan))return {};
        return detail::loudness_operation_policy(node,plan);
    }
    detail::BatchPlan plan;
    if(!detail::batch_operation_plan(node,plan) || !detail::batch_operation_shapes(node,plan))return {};
    return detail::batch_operation_policy(node,plan);
}
NumericOperationPolicy sequence_policy(const dataflow_runtime::NumericOperationNodeView& node) {
    if(detail::pcm_stage_operation_kind(node.kind))return detail::pcm_stage_policy(node);
    if(detail::tracking_operation_kind(node.kind))return detail::tracking_operation_policy(node);
    if(detail::sequence_source_kind(node.kind))return detail::sequence_source_policy(node);
    return batch_policy(node);
}
NumericOperationPolicy source_policy(const dataflow_runtime::NumericOperationNodeView& node) {
    if(detail::btrack_operation_kind(node.kind))return detail::btrack_policy(node);
    if(detail::essentia_operation_kind(node.kind))return detail::essentia_operation_policy(node);
    if(detail::interval_operation_kind(node.kind))return interval_policy(node.outputs.size());
    return feature_kind(node.kind)?feature_policy(node):sequence_policy(node);
}
std::unique_ptr<detail::AdapterOperation> prepare_feature(const dataflow_runtime::NumericOperationNode& node) {
    if(detail::spectral_operation_kind(node.kind))return detail::prepare_spectral_operation(node);
    if(detail::descriptor_operation_kind(node.kind))return detail::prepare_descriptor_operation(node);
    if(detail::event_operation_kind(node.kind))return detail::prepare_event_operation(node);
    if(detail::buffered_operation_kind(node.kind))return detail::prepare_buffered_operation(node);
    return detail::prepare_contrast_operation(node);
}
std::unique_ptr<detail::AdapterOperation> prepare_sequence(const dataflow_runtime::NumericOperationNode& node) {
    if(detail::pcm_stage_operation_kind(node.kind))return detail::prepare_pcm_stage(node);
    if(detail::tracking_operation_kind(node.kind))return detail::prepare_tracking_operation(node);
    if(detail::loudness_operation_kind(node.kind))return detail::prepare_loudness_operation(node);
    if(detail::sequence_source_kind(node.kind))return detail::prepare_sequence_source(node);
    return detail::prepare_batch_operation(node);
}
std::unique_ptr<detail::AdapterOperation> prepare_source_operation(const dataflow_runtime::NumericOperationNode& node,
    const std::shared_ptr<const dsp_kernels::ResamplerBackendFactory>& provider) {
    if(detail::btrack_operation_kind(node.kind))return detail::prepare_btrack_operation(node,provider);
    if(detail::essentia_operation_kind(node.kind))return detail::prepare_essentia_operation(node);
    if(detail::interval_operation_kind(node.kind))return detail::prepare_interval_operation(node);
    return feature_kind(node.kind)?prepare_feature(node):prepare_sequence(node);
}
domain::ProcessingSupport assess_feature(const dataflow_runtime::NumericOperationNodeView& node,const NumericOperationProfile& profile) noexcept {
    if(detail::spectral_operation_kind(node.kind))return detail::assess_spectral_operation(node,profile);
    if(detail::descriptor_operation_kind(node.kind))return detail::assess_descriptor_operation(node,profile);
    if(detail::event_operation_kind(node.kind))return detail::assess_event_operation(node,profile);
    if(detail::buffered_operation_kind(node.kind))return detail::assess_buffered_operation(node,profile);
    return detail::assess_contrast_operation(node,profile);
}
domain::ProcessingSupport assess_sequence(const dataflow_runtime::NumericOperationNodeView& node,const NumericOperationProfile& profile) noexcept {
    if(detail::pcm_stage_operation_kind(node.kind))return detail::assess_pcm_stage(node,profile);
    if(detail::batch_operation_kind(node.kind))return detail::assess_batch_operation(node,profile);
    if(detail::tracking_operation_kind(node.kind))return detail::assess_tracking_operation(node,profile);
    if(detail::loudness_operation_kind(node.kind))return detail::assess_loudness_operation(node,profile);
    return detail::assess_sequence_source(node,profile);
}
Status publish_prepared(std::unique_ptr<detail::AdapterOperation> candidate,std::shared_ptr<const NumericOperationDescriptor> descriptor,
    const NumericOperationProfile& profile,const domain::ProcessingResources& bound,std::unique_ptr<PreparedNumericOperation>& output) {
    if(!candidate)return Status::invalid_argument;
    candidate->bind(std::move(descriptor),profile.admission);domain::ProcessingResources actual;
    if(!candidate->owned_resources(actual) || !within_bound(actual,bound))return Status::exhausted;
    output=std::move(candidate);return Status::ok;
}
class RhythmOperationFactory final:public NumericOperationFactory {
public:
    explicit RhythmOperationFactory(RhythmOperationFactoryOptions options):options_(std::move(options)){}
    Status describe(const dataflow_runtime::NumericOperationNodeView& node,std::shared_ptr<const NumericOperationDescriptor>& out) const override {
        if(!source_operation_kind(node.kind))return options_.fallback?options_.fallback->describe(node,out):Status::unavailable;
        auto policy=source_policy(node);detail::add_metadata_policy(node,policy);
        return own_numeric_operation_descriptor(node,std::move(policy),out,this);
    }
    domain::ProcessingSupport assess(const dataflow_runtime::NumericOperationNodeView& node,const NumericOperationProfile& profile) const noexcept override {
        if(detail::btrack_operation_kind(node.kind))return detail::assess_btrack_operation(node,profile,options_.window_resampler.get());
        if(detail::essentia_operation_kind(node.kind))return detail::assess_essentia_operation(node,profile);
        if(detail::interval_operation_kind(node.kind))return detail::assess_interval_operation(node,profile);
        if(feature_kind(node.kind))return assess_feature(node,profile);
        if(sequence_kind(node.kind))return assess_sequence(node,profile);
        return options_.fallback?options_.fallback->assess(node,profile):domain::ProcessingSupport{};
    }
    Status prepare(std::shared_ptr<const NumericOperationDescriptor> descriptor,const NumericOperationProfile& profile,
        std::unique_ptr<PreparedNumericOperation>& output) const override {
        if(!descriptor)return Status::invalid_argument;
        if(descriptor->factory_identity!=this)return options_.fallback?options_.fallback->prepare(std::move(descriptor),profile,output):Status::unavailable;
        const auto support=assess(detail::definition_view(descriptor->node),profile);
        if(!support.supported)return Status::incompatible_contract;
        auto candidate=prepare_source_operation(descriptor->node,options_.window_resampler);
        return publish_prepared(std::move(candidate),std::move(descriptor),profile,support.resources,output);
    }
private:
    RhythmOperationFactoryOptions options_;
};
}
std::shared_ptr<const NumericOperationFactory> make_rhythm_operation_factory(RhythmOperationFactoryOptions options) {
    return std::make_shared<RhythmOperationFactory>(std::move(options));
}
}
