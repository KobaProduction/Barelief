// SPDX-License-Identifier: AGPL-3.0-or-later
#include "sequence_source_io.h"

namespace rhythm_graph_adapters::detail {
using namespace dataflow_runtime;
using namespace signal_contracts;
namespace {
bool read_row(const void* context,std::size_t row,std::span<float> output) noexcept {
    const auto& source=*static_cast<const NumericBufferView*>(context);
    if(row>=source.actual[0] || output.size()!=source.actual[1])return false;
    for(std::size_t column=0;column<output.size();++column)
        if(load_element(source,{0,row,column},output[column])!=Status::ok)return false;
    return true;
}
bool write_row(void* context,std::size_t row,std::span<const float> values) noexcept {
    auto& output=*static_cast<NumericOperationOutput*>(context);
    if(row>=output.value.actual[0] || values.size()!=output.value.actual[1])return false;
    for(std::size_t column=0;column<values.size();++column)
        if(store_element(output.value,{0,row,column},values[column])!=Status::ok)return false;
    return true;
}
}
dsp_kernels::FloatMatrixSource sequence_source_rows(const NumericOperationInput& input) noexcept {return {&input.value,read_row};}
dsp_kernels::FloatMatrixDestination sequence_source_writer(NumericOperationOutput& output) noexcept {return {&output,write_row};}
Status sequence_source_flags(NumericOperationOutput& output,std::span<const std::uint8_t> values,const dataflow_runtime::NumericOperationOrigin& stamp) noexcept {
    output.value.actual={values.size(),1};
    for(std::size_t i=0;i<values.size();++i){const auto status=store_element(output.value,{0,i,0},values[i]);if(status!=Status::ok)return status;}
    output.origin=stamp;output.status=Status::ok;return Status::ok;
}
Status sequence_source_centers(NumericOperationOutput& output,std::span<const double> values,const dataflow_runtime::NumericOperationOrigin& stamp) noexcept {
    output.value.actual={values.size(),1};
    for(std::size_t i=0;i<values.size();++i){const auto status=store_element(output.value,{0,i,0},float(values[i]));if(status!=Status::ok)return status;}
    output.origin=stamp;output.status=Status::ok;return Status::ok;
}
}
