// SPDX-License-Identifier: AGPL-3.0-or-later
#include "operation_io.h"
#include "parameter_values.h"
#include <algorithm>
#include <bit>
#include <cmath>
#include <functional>
#include <limits>

namespace rhythm_graph_adapters::detail {
using namespace signal_contracts;
const dataflow_runtime::NumericOperationInput* input_at(std::span<const dataflow_runtime::NumericOperationInput> inputs,domain::PortId port) noexcept {
    for(const auto& input:inputs)if(input.port==port)return &input;
    return nullptr;
}
dataflow_runtime::NumericOperationOutput* output_at(std::span<dataflow_runtime::NumericOperationOutput> outputs,domain::PortId port) noexcept {
    for(auto& output:outputs)if(output.port==port)return &output;
    return nullptr;
}
bool vector_capacity(const domain::ConfiguredProcessingNode& node,domain::PortDirection direction,domain::PortId id,std::size_t& count) noexcept {
    return vector_capacity(definition_view(node),direction,id,count);
}
bool vector_capacity(const domain::ResolvedProcessingNode& node,domain::PortDirection direction,domain::PortId id,std::size_t& count) noexcept {
    const auto& ports=direction==domain::PortDirection::Input?node.inputs:node.outputs;
    for(const auto& port:ports){
        if(port.id!=id)continue;
        const auto contract=port.contract.view();const auto* numeric=std::get_if<NumericContract>(&contract);
        if(!numeric || numeric->dtype!=DType::Float32 || numeric->shape.rank!=Rank::Vector || numeric->channel_order.size()!=1)return false;
        if(direction==domain::PortDirection::Input && numeric->representation.encoding!=StorageEncoding::Native)return false;
        const auto maximum=numeric->shape.extents[0].maximum;
        if(maximum>std::numeric_limits<std::size_t>::max())return false;
        count=std::size_t(maximum);return true;
    }
    return false;
}
bool output_shape(const domain::ConfiguredProcessingNode& node,domain::PortId id,Rank rank,std::size_t rows,std::size_t columns,bool variable) noexcept {
    return output_shape(definition_view(node),id,rank,rows,columns,variable);
}
bool output_shape(const domain::ResolvedProcessingNode& node,domain::PortId id,Rank rank,std::size_t rows,std::size_t columns,bool variable) noexcept {
    for(const auto& port:node.outputs){
        if(port.id!=id)continue;
        const auto contract=port.contract.view();const auto* numeric=std::get_if<NumericContract>(&contract);
        return numeric && numeric->dtype==DType::Float32 && numeric->shape.rank==rank && numeric->channel_order.size()==1 &&
            numeric->shape.extents[0].maximum>=rows && numeric->shape.extents[1].maximum>=columns &&
            numeric->shape.extents[0].minimum<=(variable?0:rows) && numeric->shape.extents[1].minimum<=columns;
    }
    return false;
}
namespace {
bool index_coordinates(const AxisCoordinates& axis) noexcept {
    if(axis.mode==CoordinateMode::None)return axis==AxisCoordinates{};
    return axis.mode==CoordinateMode::Regular && (axis.unit==NumericUnit::Values || axis.unit==NumericUnit::Frames) &&
        axis.origin.numerator==0 && axis.origin.denominator && axis.step.numerator>0 &&
        std::uint64_t(axis.step.numerator)==axis.step.denominator;
}
Status validate_proof_values(const dataflow_runtime::NumericOperationInput& input) noexcept {
    for(std::size_t i=0;i<input.contiguous_float32.size();++i){
        float logical=0;const auto status=load_element(input.value,{0,i,0},logical);
        if(status!=Status::ok)return status;
        if(!std::isfinite(logical) || std::bit_cast<std::uint32_t>(logical)!=
            std::bit_cast<std::uint32_t>(input.contiguous_float32[i]))return Status::invalid_argument;
    }
    return Status::ok;
}
}
bool index_output_contract(const NumericContract& contract) noexcept {
    return contract.clock.domain.empty() && !contract.clock.sample_rate && contract.clock.tick_seconds==Ratio{1,1} &&
        index_coordinates(contract.coordinates[0]) && index_coordinates(contract.coordinates[1]);
}
Status validate_vector(const dataflow_runtime::NumericOperationInput& input,std::size_t maximum,
    const DataContract& expected,const FrameLimits& limits) noexcept {
    const auto* contract=input.value.contract;
    if(input.status!=Status::ok || !input.stamp.available)return Status::unavailable;
    if(!contract || contract->dtype!=DType::Float32 || contract->shape.rank!=Rank::Vector || input.value.planes.size()!=1 ||
       contract->representation.encoding!=StorageEncoding::Native || !same_physical_contract(*contract,expected) ||
       input.value.actual[0]>maximum || input.contiguous_float32.size()!=input.value.actual[0] ||
       (!input.contiguous_float32.empty() && !input.contiguous_float32.data()))return Status::invalid_argument;
    const auto status=validate_view(DataFrameView{input.stamp,input.value},limits);
    return status==Status::ok?validate_proof_values(input):status;
}
Status validate_outputs(std::span<dataflow_runtime::NumericOperationOutput> outputs,const FrameLimits& limits) noexcept {
    for(const auto& output:outputs){
        if(!output.value.contract || output.value.contract->dtype!=DType::Float32 || output.value.planes.size()!=1)return Status::invalid_argument;
        const auto status=validate_mutable(output.value,limits);if(status!=Status::ok)return status;
    }
    return Status::ok;
}
namespace {
bool overlaps(std::span<const std::byte> a,std::span<const std::byte> b) noexcept {
    if(a.empty() || b.empty())return false;
    const std::less<const std::byte*> before;
    return before(a.data(),b.data()+b.size()) && before(b.data(),a.data()+a.size());
}
}
bool disjoint_operation_buffers(std::span<const dataflow_runtime::NumericOperationInput> inputs,
    std::span<const dataflow_runtime::NumericOperationOutput> outputs) noexcept {
    for(std::size_t i=0;i<outputs.size();++i){
        for(const auto& input:inputs)if(overlaps(input.value.storage,outputs[i].value.storage))return false;
        for(std::size_t j=0;j<i;++j)if(overlaps(outputs[i].value.storage,outputs[j].value.storage))return false;
    }
    return true;
}
void invalidate_outputs(std::span<dataflow_runtime::NumericOperationOutput> outputs) noexcept {
    for(auto& output:outputs){output.status=Status::unavailable;output.stamp.available=false;}
}
FrameStamp batch_stamp(const FrameStamp& input) noexcept {
    FrameStamp stamp;stamp.sequence=input.sequence;stamp.generation=input.generation;stamp.epoch=input.epoch;
    stamp.graph_revision=input.graph_revision;stamp.source_sequence=input.source_sequence;stamp.producer_epoch=input.producer_epoch;
    stamp.discontinuity=input.discontinuity;return stamp;
}
Status write_vector(dataflow_runtime::NumericOperationOutput& output,std::span<const float> values,const FrameStamp& stamp) noexcept {
    output.value.actual={values.size(),1};
    for(std::size_t i=0;i<values.size();++i){const auto status=store_element(output.value,{0,i,0},values[i]);if(status!=Status::ok)return status;}
    output.stamp=stamp;output.status=Status::ok;return Status::ok;
}
Status write_scalar(dataflow_runtime::NumericOperationOutput& output,float value,const FrameStamp& stamp) noexcept {
    output.value.actual={1,1};const auto status=store_element(output.value,{0,0,0},value);
    if(status==Status::ok){output.stamp=stamp;output.status=Status::ok;}return status;
}
}
