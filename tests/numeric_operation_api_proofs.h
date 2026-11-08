#pragma once
#include <dataflow_runtime/numeric_operation.h>
#include <dataflow_runtime/runtime.h>
#include <signal_contracts/data_frame.h>
#include <concepts>
#include <type_traits>
#include <utility>

namespace operation_api_proofs {
using namespace dataflow_runtime;
using namespace signal_contracts;

template<class T> concept HasFrameMetadata =
    requires(T value){value.stamp;} || requires(T value){value.clock;} ||
    requires(T value){value.coordinates[0];} || requires(T value){value.coordinates.axes;} ||
    requires(T value){value.contract->clock;} || requires(T value){value.quantity;} ||
    requires(T value){value.unit;} || requires(T value){value.semantic;} ||
    requires(T value){value.history_element;} || requires(T value){value.channel_order;} ||
    requires(T value){value.shape.axes;} || requires(T value){value.axes;} ||
    requires(T value){value.sample_rate;} ||
    requires(T value){value.timestamp_ticks;} || requires(T value){value.stream_id;} ||
    requires(T value){value.source_sequence;} || requires(T value){value.frame;};

template<class T> concept HasMetadataGetter =
    requires(T value){value.stamp();} || requires(T value){value.clock();} ||
    requires(T value){value.coordinates();} || requires(T value){value.metadata();} ||
    requires(T value){value.contract();} || requires(T value){value.frame();} ||
    requires(T value){value.sample_rate();} || requires(T value){value.get();};

template<class T> concept HasRuntimeProvider =
    requires(T value){value.operation_factory;} || requires(T value){value.fourier_backend;} ||
    requires(T value){value.resampler_backend;} || requires(T value){value.runtime;} ||
    requires(T value){value.context;} || requires(T value){value.callback;};

template<class T> concept HasPayloadGetter =
    requires(T value){value.value;} || requires(T value){value.value();} ||
    requires(T value){value.storage;} || requires(T value){value.data();} ||
    requires(T value){value.view();} || requires(T value){value.read(0);};

template<class T> concept HasCoordinateStorage =
    requires(T value){value.axes;} || requires(T value){value.writable_coordinates;} ||
    requires(T value){value.coordinates.axes;} || requires(T value){value.coordinates[0];};

template<class T> concept ExposesProvenanceInternals =
    requires(T value){value.identity_;} || requires(T value){value.authority_;} ||
    requires(T value){value.clock_;} || requires(T value){value.invocation_;} ||
    requires(T value){value.lease_;} || requires(T value){value.generation_;};

template<class T> concept AcceptsFrameView = requires(T value,const NumericContract* contract){
    value.contract=contract;
};

template<class T> concept ExposesFullNode =
    requires(T value){requires std::same_as<decltype(value.node.inputs[0].contract.view()),DataContract>;} ||
    requires(T value){requires std::same_as<decltype(value.inputs[0].contract.view()),DataContract>;};

static_assert(HasFrameMetadata<NumericContract>);
static_assert(HasFrameMetadata<NumericView>);
static_assert(HasFrameMetadata<DataFrameView>);
static_assert(HasRuntimeProvider<NumericRuntimeProfile>);
static_assert(ExposesFullNode<domain::ConfiguredProcessingNode>);

static_assert(!HasFrameMetadata<NumericBufferContract>);
static_assert(!HasFrameMetadata<NumericBufferShape>);
static_assert(!HasFrameMetadata<NumericBufferView>);
static_assert(!HasFrameMetadata<MutableNumericBufferView>);
static_assert(!HasMetadataGetter<NumericBufferContract>);
static_assert(!HasMetadataGetter<NumericBufferView>);
static_assert(!HasMetadataGetter<MutableNumericBufferView>);
static_assert(!HasFrameMetadata<NumericOperationInput>);
static_assert(!HasFrameMetadata<NumericOperationOutput>);
static_assert(!HasMetadataGetter<NumericOperationInput>);
static_assert(!HasMetadataGetter<NumericOperationOutput>);
static_assert(!HasCoordinateStorage<NumericOperationOutput>);
static_assert(!HasCoordinateStorage<NumericOperationCoordinateWriter>);
static_assert(!HasFrameMetadata<NumericOperationOrigin>);
static_assert(!HasMetadataGetter<NumericOperationOrigin>);
static_assert(!HasPayloadGetter<NumericOperationOrigin>);
static_assert(!HasFrameMetadata<NumericOperationCoordinateWriter>);
static_assert(!HasMetadataGetter<NumericOperationCoordinateWriter>);
static_assert(!HasPayloadGetter<NumericOperationCoordinateWriter>);
static_assert(!HasRuntimeProvider<NumericOperationProfile>);
static_assert(!HasFrameMetadata<NumericOperationProfile>);
static_assert(!HasMetadataGetter<NumericOperationProfile>);
static_assert(!HasMetadataGetter<NumericOperationResources>);
static_assert(!HasFrameMetadata<NumericOperationPort>);
static_assert(!HasFrameMetadata<NumericOperationNode>);
static_assert(!HasFrameMetadata<NumericOperationNodeView>);
static_assert(!HasMetadataGetter<NumericOperationNode>);
static_assert(!HasMetadataGetter<NumericOperationNodeView>);
static_assert(!HasMetadataGetter<NumericOperationPorts>);
static_assert(!ExposesProvenanceInternals<NumericOperationOrigin>);
static_assert(!ExposesProvenanceInternals<NumericOperationCoordinateWriter>);
static_assert(!ExposesFullNode<NumericOperationNode>);
static_assert(!ExposesFullNode<NumericOperationNodeView>);
static_assert(!ExposesFullNode<NumericOperationDescriptor>);
static_assert(!HasRuntimeProvider<NumericOperationNodeView>);
static_assert(!HasRuntimeProvider<NumericOperationDescriptor>);
static_assert(!HasPayloadGetter<NumericOperationPorts>);
static_assert(!AcceptsFrameView<NumericBufferView>);
static_assert(!AcceptsFrameView<MutableNumericBufferView>);

static_assert(std::same_as<decltype(NumericOperationDescriptor::node),NumericOperationNode>);
static_assert(std::same_as<decltype(NumericOperationPort::contract),NumericBufferContract>);
static_assert(std::same_as<decltype(NumericOperationInput::value),NumericBufferView>);
static_assert(std::same_as<decltype(NumericOperationOutput::value),MutableNumericBufferView>);
static_assert(std::same_as<decltype(NumericOperationOutput::coordinates),NumericOperationCoordinateWriter>);
static_assert(std::same_as<decltype(std::declval<MutableNumericBufferView>().view()),NumericBufferView>);
static_assert(std::same_as<decltype(std::declval<NumericBufferContract>().view()),NumericBufferContract>);
static_assert(std::same_as<decltype(std::declval<NumericOperationPorts>()[0]),NumericOperationPort>);
static_assert(std::same_as<decltype(NumericOperationDescriptor::immutable_resources),
    std::shared_ptr<const NumericOperationResources>>);

static_assert(!std::is_convertible_v<NumericBufferContract,NumericContract>);
static_assert(!std::is_convertible_v<NumericContract,NumericBufferContract>);
static_assert(!std::is_convertible_v<NumericBufferView,NumericView>);
static_assert(!std::is_convertible_v<NumericView,NumericBufferView>);
static_assert(!std::is_convertible_v<MutableNumericBufferView,MutableNumericView>);
static_assert(!std::is_convertible_v<MutableNumericView,MutableNumericBufferView>);
static_assert(!std::is_convertible_v<NumericOperationNodeView,domain::ResolvedProcessingNode>);
static_assert(!std::is_convertible_v<domain::ResolvedProcessingNode,NumericOperationNodeView>);
static_assert(!std::is_convertible_v<NumericOperationNode,domain::ConfiguredProcessingNode>);
static_assert(!std::is_convertible_v<domain::ConfiguredProcessingNode,NumericOperationNode>);
static_assert(!std::is_convertible_v<NumericOperationProfile,NumericRuntimeProfile>);
static_assert(!std::is_convertible_v<NumericRuntimeProfile,NumericOperationProfile>);
static_assert(!std::is_convertible_v<NumericOperationOrigin,FrameStamp>);
static_assert(!std::is_convertible_v<FrameStamp,NumericOperationOrigin>);
static_assert(!std::is_convertible_v<NumericOperationOrigin,const void*>);
static_assert(!std::is_convertible_v<NumericOperationCoordinateWriter,CoordinateView>);
static_assert(!std::is_constructible_v<NumericBufferContract,NumericContract>);
static_assert(!std::is_constructible_v<NumericBufferView,NumericView>);
static_assert(!std::is_constructible_v<MutableNumericBufferView,MutableNumericView>);
static_assert(!std::is_constructible_v<NumericOperationNodeView,domain::ResolvedProcessingNode>);
static_assert(!std::is_constructible_v<NumericOperationNode,domain::ConfiguredProcessingNode>);
static_assert(!std::is_constructible_v<NumericOperationProfile,NumericRuntimeProfile>);
static_assert(!std::is_constructible_v<NumericOperationOrigin,FrameStamp>);
static_assert(!std::is_constructible_v<NumericOperationInput,domain::PortId,NumericView,FrameStamp,Status>);

using Describe = Status (NumericOperationFactory::*)(const NumericOperationNodeView&,
    std::shared_ptr<const NumericOperationDescriptor>&) const;
using Assess = domain::ProcessingSupport (NumericOperationFactory::*)(const NumericOperationNodeView&,
    const NumericOperationProfile&) const noexcept;
using Prepare = Status (NumericOperationFactory::*)(std::shared_ptr<const NumericOperationDescriptor>,
    const NumericOperationProfile&,std::unique_ptr<PreparedNumericOperation>&) const;
using Process = Status (PreparedNumericOperation::*)(std::span<const NumericOperationInput>,
    std::span<NumericOperationOutput>,NumericOperationControl) noexcept;
static_assert(std::same_as<decltype(&NumericOperationFactory::describe),Describe>);
static_assert(std::same_as<decltype(&NumericOperationFactory::assess),Assess>);
static_assert(std::same_as<decltype(&NumericOperationFactory::prepare),Prepare>);
static_assert(std::same_as<decltype(&PreparedNumericOperation::process),Process>);
static_assert(!std::is_invocable_v<Describe,const NumericOperationFactory&,
    const domain::ResolvedProcessingNode&,std::shared_ptr<const NumericOperationDescriptor>&>);
static_assert(!std::is_invocable_v<Assess,const NumericOperationFactory&,
    const NumericOperationNodeView&,const NumericRuntimeProfile&>);
static_assert(!std::is_invocable_v<Prepare,const NumericOperationFactory&,
    std::shared_ptr<const NumericOperationDescriptor>,const NumericRuntimeProfile&,
    std::unique_ptr<PreparedNumericOperation>&>);
} // namespace operation_api_proofs
