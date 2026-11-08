// SPDX-License-Identifier: AGPL-3.0-or-later
#include "operation_io.h"
#include "parameter_values.h"
#include <dsp_kernels/kernel_support.h>
#include <algorithm>
#include <bit>
#include <cmath>
#include <functional>
#include <limits>

namespace rhythm_graph_adapters::detail {
using namespace signal_contracts;
bool operation_input_unique(std::span<const dataflow_runtime::NumericOperationInput> inputs,std::size_t index) noexcept {
    if(index>=inputs.size())return false;
    for(std::size_t i=0;i<index;++i)if(inputs[i].port==inputs[index].port)return false;
    return true;
}
bool operation_port_unique(dataflow_runtime::NumericOperationPorts ports,std::size_t index) noexcept {
    if(index>=ports.size())return false;
    for(std::size_t i=0;i<index;++i)if(ports[i].id==ports[index].id)return false;
    return true;
}
bool operation_ports_valid(const dataflow_runtime::NumericOperationNodeView& node,const FrameLimits& limits) noexcept {
    for(const auto& port:node.inputs)if(validate_contract(port.contract,limits)!=Status::ok)return false;
    for(const auto& port:node.outputs)if(validate_contract(port.contract,limits)!=Status::ok)return false;
    return true;
}
domain::ProcessingSupport complete_operation_support(domain::ProcessingSupport support,std::size_t scratch,std::size_t extra) noexcept {
    support.resources.scratch_bytes=scratch;
    if(!dataflow_runtime::numeric_operation_add_bytes(support.resources.scratch_bytes,extra))return {};
    support.resources.stack_bytes=dsp_kernels::numeric_reference_stack_bytes;support.supported=true;return support;
}
namespace {
void frame_input_policy(dataflow_runtime::NumericOperationPolicy& policy,dataflow_runtime::NumericOperationPorts ports,domain::PortId control) {
    policy.inputs.reserve(ports.size());
    for(const auto& port:ports){
        const bool command=port.id==control;
        policy.inputs.push_back({port.id,command?dataflow_runtime::NumericInputUse::Control:dataflow_runtime::NumericInputUse::Frame,!command,ValueDomain::FiniteOnly});
        if(command)policy.control=port.id;
    }
}
}
dataflow_runtime::NumericOperationPolicy frame_operation_policy(const dataflow_runtime::NumericOperationNodeView& node,domain::PortId control,
    dataflow_runtime::NumericOperationFailure failure) {
    dataflow_runtime::NumericOperationPolicy policy;policy.mode=dataflow_runtime::NumericOperationMode::ContinuingStream;
    policy.match=dataflow_runtime::NumericInputMatch::SessionSequence;policy.session=dataflow_runtime::NumericInputSession::SharedProvenance;
    policy.arithmetic_failure=failure;frame_input_policy(policy,node.inputs,control);
    policy.output_values.assign(node.outputs.size(),ValueDomain::FiniteOnly);return policy;
}
const dataflow_runtime::NumericOperationInput* input_at(std::span<const dataflow_runtime::NumericOperationInput> inputs,domain::PortId port) noexcept {
    for(const auto& input:inputs)if(input.port==port)return &input;
    return nullptr;
}
dataflow_runtime::NumericOperationOutput* output_at(std::span<dataflow_runtime::NumericOperationOutput> outputs,domain::PortId port) noexcept {
    for(auto& output:outputs)if(output.port==port)return &output;
    return nullptr;
}
bool vector_capacity(const dataflow_runtime::NumericOperationNode& node,domain::PortDirection direction,domain::PortId id,std::size_t& count) noexcept {
    return vector_capacity(definition_view(node),direction,id,count);
}
namespace {
bool vector_port_capacity(const dataflow_runtime::NumericOperationPort& port,domain::PortDirection direction,std::size_t& count) noexcept {
    const auto& value=port.contract;
    if(value.dtype!=DType::Float32 || value.shape.rank!=Rank::Vector || value.plane_count!=1)return false;
    if(direction==domain::PortDirection::Input && value.representation.encoding!=StorageEncoding::Native)return false;
    const auto maximum=value.shape.extents[0].maximum;
    if(maximum>std::numeric_limits<std::size_t>::max())return false;
    count=std::size_t(maximum);return true;
}
}
bool vector_capacity(const dataflow_runtime::NumericOperationNodeView& node,domain::PortDirection direction,domain::PortId id,std::size_t& count) noexcept {
    for(const auto& port:direction==domain::PortDirection::Input?node.inputs:node.outputs)
        if(port.id==id)return vector_port_capacity(port,direction,count);
    return false;
}
bool output_shape(const dataflow_runtime::NumericOperationNode& node,domain::PortId id,Rank rank,std::size_t rows,std::size_t columns,bool variable) noexcept {
    return output_shape(definition_view(node),id,rank,rows,columns,variable);
}
bool output_shape(const dataflow_runtime::NumericOperationNodeView& node,domain::PortId id,Rank rank,std::size_t rows,std::size_t columns,bool variable) noexcept {
    for(const auto& port:node.outputs){
        if(port.id!=id)continue;
        const auto& numeric=port.contract;
        return numeric.dtype==DType::Float32 && numeric.shape.rank==rank && numeric.plane_count==1 &&
            numeric.shape.extents[0].maximum>=rows && numeric.shape.extents[1].maximum>=columns &&
            numeric.shape.extents[0].minimum<=(variable?0:rows) && numeric.shape.extents[1].minimum<=columns;
    }
    return false;
}
namespace {

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

Status validate_vector(const dataflow_runtime::NumericOperationInput& input,std::size_t maximum,
    const NumericBufferContract& expected,const FrameLimits& limits) noexcept {
    const auto* contract=input.value.contract;
    if(input.status!=Status::ok)return Status::unavailable;
    if(!contract || contract->dtype!=DType::Float32 || contract->shape.rank!=Rank::Vector || input.value.planes.size()!=1 ||
       contract->representation.encoding!=StorageEncoding::Native || !same_physical_contract(*contract,expected) ||
       input.value.actual[0]>maximum || input.contiguous_float32.size()!=input.value.actual[0] ||
       (!input.contiguous_float32.empty() && !input.contiguous_float32.data()))return Status::invalid_argument;
    const auto status=validate_view(input.value,limits);
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
    for(auto& output:outputs){output.status=Status::unavailable;output.origin={};}
}
dataflow_runtime::NumericOperationOrigin batch_stamp(const dataflow_runtime::NumericOperationOrigin& input) noexcept {
    return dataflow_runtime::numeric_operation_origin(input,dataflow_runtime::NumericOperationPublication::Batch);
}
Status write_vector(dataflow_runtime::NumericOperationOutput& output,std::span<const float> values,const dataflow_runtime::NumericOperationOrigin& stamp) noexcept {
    output.value.actual={values.size(),1};
    for(std::size_t i=0;i<values.size();++i){const auto status=store_element(output.value,{0,i,0},values[i]);if(status!=Status::ok)return status;}
    output.origin=stamp;output.status=Status::ok;return Status::ok;
}
Status write_scalar(dataflow_runtime::NumericOperationOutput& output,float value,const dataflow_runtime::NumericOperationOrigin& stamp) noexcept {
    output.value.actual={1,1};const auto status=store_element(output.value,{0,0,0},value);
    if(status==Status::ok){output.origin=stamp;output.status=Status::ok;}return status;
}
}
