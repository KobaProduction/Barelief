#include "operation_kernel_state.h"
#include "frame_access.h"
#include "preparation_support.h"
#include <domain/music/contracts.h>
#include <domain/signal/contracts.h>
#include <algorithm>
#include <cmath>
#include <cstring>

namespace dataflow_runtime::detail {
namespace {
dsp_kernels::MelNormalization mel_norm(std::string_view value) noexcept {
    using N=dsp_kernels::MelNormalization;
    if(value=="none")return N::None;
    if(value=="l0")return N::L0;
    if(value=="lp")return N::Lp;
    if(value=="maximum")return N::Maximum;
    if(value=="minimum")return N::Minimum;
    return N::Slaney;
}
dsp_kernels::MelFilterBankConfig mel_config(const domain::MelFilterParameters& p) noexcept {
    dsp_kernels::MelFilterBankConfig c;c.sample_rate=p.sample_rate;c.bin_hz=p.bin_hz;c.first_bin=p.first_bin;c.bands=p.bands;
    c.fmin_hz=p.fmin_hz;c.fmax_hz=p.fmax_hz;c.norm_p=p.norm_p;c.normalization=mel_norm(p.norm);
    c.scale=p.scale=="htk"?dsp_kernels::MelScale::Htk:dsp_kernels::MelScale::Slaney;return c;
}
dsp_kernels::PowerToDbConfig db_config(const domain::PowerToDbParameters& p) noexcept {
    using A=dsp_kernels::DbReductionAxes;using R=dsp_kernels::DbReference;
    return {float(p.reference),float(p.amin),float(p.top_db),p.top_db_enabled,p.reference_mode=="maximum"?R::Maximum:R::Value,
        p.axes=="rows"?A::Rows:p.axes=="columns"?A::Columns:A::All};
}
std::array<std::size_t,2> matrix_size(const NumericContract& c) noexcept {
    if(c.shape.rank==Rank::Scalar)return {1,1};
    return c.shape.rank==Rank::Matrix?std::array{std::size_t(c.shape.extents[0].maximum),std::size_t(c.shape.extents[1].maximum)}:
        std::array{std::size_t{1},std::size_t(c.shape.extents[0].maximum)};
}
std::array<std::size_t,2> matrix_size(const NumericView& view) noexcept {
    if(view.contract->shape.rank==Rank::Scalar)return {1,1};
    return view.contract->shape.rank==Rank::Matrix?view.actual:std::array{std::size_t{1},view.actual[0]};
}
ElementIndex element(Rank rank,std::size_t row,std::size_t column) noexcept {
    return rank==Rank::Matrix?ElementIndex{0,row,column}:ElementIndex{0,column,0};
}
bool read_row(const void* user,std::size_t row,std::span<float> values) noexcept {
    const auto& source=*static_cast<const NumericView*>(user);
    for(std::size_t column=0;column<values.size();++column){
        const auto status=load_element(source,element(source.contract->shape.rank,row,column),values[column]);
        if(status!=Status::ok)return false;
    }
    return true;
}
bool write_row(void* user,std::size_t row,std::span<const float> values) noexcept {
    auto& destination=*static_cast<MutableNumericView*>(user);
    for(std::size_t column=0;column<values.size();++column){
        const auto status=store_element(destination,element(destination.contract->shape.rank,row,column),values[column]);
        if(status!=Status::ok)return false;
    }
    return true;
}
void encode_coordinates(std::span<std::byte> bytes,std::span<const double> values) noexcept {
    for(std::size_t index=0;index<values.size();++index){
        std::uint64_t bits=0;std::memcpy(&bits,&values[index],8);
        for(std::size_t byte=0;byte<8;++byte)bytes[index*8+byte]=std::byte((bits>>(byte*8))&255);
    }
}
double coordinate(std::span<const std::byte> bytes,std::size_t index) noexcept {
    std::uint64_t bits=0;
    for(std::size_t byte=0;byte<8;++byte)bits|=std::uint64_t(std::to_integer<unsigned char>(bytes[index*8+byte]))<<(byte*8);
    double value=0;std::memcpy(&value,&bits,8);return value;
}
bool mel_coordinates(const NumericView& source,std::span<const double> frequencies) noexcept {
    const auto axis=source.contract->shape.rank==Rank::Matrix?1u:0u;
    if(source.contract->coordinates[axis].mode!=CoordinateMode::ExplicitFloat64LE)return true;
    const auto bytes=source.coordinates.axes[axis];if(bytes.size()!=source.actual[axis]*8)return false;
    for(std::size_t bin=0;bin<source.actual[axis];++bin){
        const auto hz=coordinate(bytes,bin),expected=frequencies[bin];
        if(!std::isfinite(hz) || std::abs(hz-expected)>1e-6*std::max(1.,expected))return false;
    }
    return true;
}
Status copy_axis(const NumericView& source,State& state,MutableNumericView& destination,std::size_t axis) noexcept {
    if(destination.contract->coordinates[axis].mode!=CoordinateMode::ExplicitFloat64LE)return Status::ok;
    const auto bytes=destination.actual[axis]*8;auto& owned=state.numeric_outputs[0].coordinates[axis];
    if(source.coordinates.axes[axis].size()!=bytes || owned.size()<bytes)return Status::incompatible_contract;
    std::copy_n(source.coordinates.axes[axis].begin(),bytes,owned.begin());destination.coordinates.axes[axis]={owned.data(),bytes};return Status::ok;
}
Status copy_coordinates(const NumericView& source,State& state,MutableNumericView& destination) noexcept {
    for(std::size_t axis=0;axis<2;++axis){
        const auto status=copy_axis(source,state,destination,axis);if(status!=Status::ok)return status;
    }
    return Status::ok;
}
bool mel_resources(const domain::ResolvedProcessingNode& node,domain::ProcessingResources& resources) noexcept {
    domain::MelFilterParameters p;if(!domain::mel_parameters(node,p))return false;
    const auto bins=matrix_size(std::get<NumericContract>(node.inputs[0].contract.view()))[1];std::size_t bytes=0,work=0,values=bins;
    if(!dsp_kernels::MelFilterBank::storage_bytes(bins,mel_config(p),bytes) || !numeric_add(values,p.bands) || !numeric_mul(values,sizeof(float),work))return false;
    return numeric_add(resources.scratch_bytes,bytes) && numeric_add(resources.payload_bytes,work) && numeric_mul(bins,sizeof(double),work) && numeric_add(resources.payload_bytes,work);
}
bool db_resources(const domain::ResolvedProcessingNode& node,domain::ProcessingResources& resources) noexcept {
    domain::PowerToDbParameters p;if(!domain::power_to_db_parameters(node,p))return false;
    const auto size=matrix_size(std::get<NumericContract>(node.inputs[0].contract.view()));std::size_t bytes=0;
    return dsp_kernels::PreparedPowerToDb::storage_bytes(size[0],size[1],db_config(p),bytes) && numeric_add(resources.scratch_bytes,bytes);
}
Status prepare_mel(OperationKernelState& kernel,State& state,const domain::ResolvedProcessingNode& node) {
    domain::MelFilterParameters p;if(!domain::mel_parameters(node,p))return Status::invalid_argument;
    const auto input=std::get<NumericContract>(node.inputs[0].contract.view());const auto bins=matrix_size(input)[1];
    state.frequencies.resize(bins);
    for(std::size_t bin=0;bin<bins;++bin)state.frequencies[bin]=p.frequencies_hz.empty()?(double(bin)+double(p.first_bin))*p.bin_hz:p.frequencies_hz[bin];
    kernel.mel.emplace();if(!kernel.mel->prepare(bins,mel_config(p),state.frequencies))return Status::invalid_argument;
    kernel.input.resize(bins);kernel.output.resize(p.bands);
    encode_coordinates(state.numeric_outputs[0].coordinates[input.shape.rank==Rank::Matrix?1:0],kernel.mel->centers_hz());return Status::ok;
}
Status prepare_db(OperationKernelState& kernel,const domain::ResolvedProcessingNode& node) {
    domain::PowerToDbParameters p;if(!domain::power_to_db_parameters(node,p))return Status::invalid_argument;
    const auto size=matrix_size(std::get<NumericContract>(node.inputs[0].contract.view()));
    kernel.db.emplace();kernel.db_needs_peak=p.top_db_enabled || p.reference_mode=="maximum";
    return kernel.db->prepare(size[0],size[1],db_config(p))?Status::ok:Status::invalid_argument;
}
Status execute_mel(OperationKernelState& kernel,const State& state,const NumericView& source,MutableNumericView& destination) noexcept {
    if(!mel_coordinates(source,state.frequencies))return Status::incompatible_contract;
    const auto rows=matrix_size(source)[0];
    for(std::size_t row=0;row<rows;++row){
        if(!read_row(&source,row,kernel.input) || !kernel.mel->process(kernel.input,kernel.output) ||
           !write_row(&destination,row,kernel.output))return Status::invalid_argument;
    }
    return Status::ok;
}
Status execute_db(OperationKernelState& kernel,const NumericView& source,MutableNumericView& destination) noexcept {
    const auto size=matrix_size(source);
    return kernel.db->process({&source,read_row},size[0],size[1],{&destination,write_row})?Status::ok:Status::invalid_argument;
}
Status publish(State& state,const DataFrameView& current,MutableNumericView& destination,bool available) noexcept {
    auto stamp=current.stamp;stamp.available=stamp.available && available;
    auto view=destination.view();if(!stamp.available)view.storage={};
    state.numeric_outputs[0].actual=destination.actual;state.numeric_frames[0]={stamp,view};return Status::ok;
}
Status configure_output(const OperationKernelState& kernel,const NumericView& source,State& state,MutableNumericView& destination) noexcept {
    if(kernel.db){destination.actual=source.actual;return copy_coordinates(source,state,destination);}
    if(source.contract->shape.rank!=Rank::Matrix)return Status::ok;
    destination.actual={source.actual[0],kernel.output.size()};return copy_axis(source,state,destination,0);
}
}
bool spectral_frontend_resources(const domain::ResolvedProcessingNode& node,domain::ProcessingResources& resources) noexcept {
    if(!numeric_add(resources.plan_bytes,sizeof(OperationKernelState)))return false;
    return node.kind=="Mel"?mel_resources(node,resources):db_resources(node,resources);
}
Status prepare_spectral_frontend(OperationKernelState& kernel,State& state,const domain::ResolvedProcessingNode& node) {
    return node.kind=="Mel"?prepare_mel(kernel,state,node):prepare_db(kernel,node);
}
Status execute_spectral_frontend(OperationKernelState& kernel,const PlanNode& node,State& state,const DataFrameView& current,const FrameLimits& limits) noexcept {
    const auto& source=std::get<NumericView>(current.payload);auto destination=mutable_output(node,state,0);
    const auto valid=validate_values(source,ValueDomain::FiniteOnly,limits);if(valid!=Status::ok)return valid;
    const auto configured=configure_output(kernel,source,state,destination);if(configured!=Status::ok)return configured;
    const auto size=matrix_size(source);
    if(node.node.enabled && kernel.db && kernel.db_needs_peak && (!size[0] || !size[1]))return publish(state,current,destination,false);
    const auto status=!node.node.enabled?fill_silence(destination,limits):kernel.mel?execute_mel(kernel,state,source,destination):execute_db(kernel,source,destination);
    return status==Status::ok?publish(state,current,destination,true):status;
}
}
