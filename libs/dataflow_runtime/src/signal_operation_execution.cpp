#include "operation_kernel_state.h"
#include "node_state.h"
#include "preparation_support.h"
#include "frame_access.h"
#include "signal_time.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
namespace dataflow_runtime::detail {
using namespace domain;using namespace signal_contracts;
namespace {
bool same_interval(const FrameStamp& a,const FrameStamp& b) noexcept {
    return a.producer_epoch==b.producer_epoch && a.generation==b.generation && a.epoch==b.epoch &&
        a.graph_revision==b.graph_revision && a.timestamp_ticks==b.timestamp_ticks &&
        a.has_sample_interval==b.has_sample_interval && (!a.has_sample_interval || (a.sample_begin==b.sample_begin && a.sample_count==b.sample_count));
}
template<class Scalar> Status load_plane(OperationKernelState& kernel,const NumericView& source,std::span<Scalar> target,std::size_t stride) noexcept {
    for(std::size_t row=0;row<source.actual[0];++row)for(std::size_t column=0;column<source.actual[1];++column){
        const auto status=load_element(source,{0,row,column},target[row*stride+column]);
        if(status!=Status::ok){kernel.pending={};return status;}
    }
    return Status::ok;
}
Status retain_coordinates(OperationKernelState& kernel,std::size_t lane,const NumericView& source) noexcept {
    for(std::size_t axis=0;axis<2;++axis){
        auto& destination=kernel.pair_coordinates[lane][axis];const auto data=source.coordinates.axes[axis];
        const auto bytes=source.contract->coordinates[axis].mode==CoordinateMode::ExplicitFloat64LE?source.actual[axis]*sizeof(double):0;
        if(data.size()!=bytes || destination.size()<bytes)return Status::incompatible_contract;
        std::copy(data.begin(),data.end(),destination.begin());
    }
    kernel.pair_actual[lane]=source.actual;return Status::ok;
}
Status retain_interval(OperationKernelState& kernel,std::size_t lane,const DataFrameView& current,const NumericView& source,bool polar) noexcept {
    kernel.pair_actual[lane]=source.actual;
    if(polar){const auto coordinates=retain_coordinates(kernel,lane,source);if(coordinates!=Status::ok){kernel.pending={};return coordinates;}}
    kernel.stamps[lane]=current.stamp;kernel.stamps[lane].stream_id={};kernel.pending[lane]=true;return Status::ok;
}
double polar_axis_value(std::span<const std::byte> bytes,std::size_t index) noexcept {
    std::uint64_t bits=0;
    for(std::size_t byte=0;byte<sizeof(double);++byte)bits|=std::uint64_t(std::to_integer<unsigned char>(bytes[index*sizeof(double)+byte]))<<(8*byte);
    double value=0;std::memcpy(&value,&bits,sizeof(value));return value;
}
bool matches_frequency_grid(const OperationKernelState& kernel,std::span<const std::byte> bytes,std::size_t count) noexcept {
    if(bytes.size()!=count*sizeof(double))return false;
    for(std::size_t bin=0;bin<count;++bin){
        const auto actual=polar_axis_value(bytes,bin),expected=double(kernel.polar_geometry.first_bin+bin)*kernel.bin_hz;
        if(!std::isfinite(actual) || std::abs(actual-expected)>1e-9*std::max(1.,std::abs(expected)))return false;
    }
    return true;
}
bool amplitude_grid(const OperationKernelState& kernel,const NumericView& source) noexcept {
    const auto axis=source.contract->shape.rank==Rank::Matrix?1u:0u;
    const auto& coordinates=source.contract->coordinates[axis];
    if(kernel.bin_hz==0 || coordinates.mode!=CoordinateMode::ExplicitFloat64LE || coordinates.unit!=NumericUnit::Hertz)return true;
    return matches_frequency_grid(kernel,source.coordinates.axes[axis],source.actual[axis]);
}
Status load_pair_plane(OperationKernelState& kernel,std::size_t lane,const NumericView& source,bool polar) noexcept {
    if(!kernel.precise_input.empty())return load_plane(kernel,source,std::span(kernel.precise_input),source.actual[1]);
    auto& target=lane?kernel.imaginary:kernel.input;
    const auto stride=polar && source.contract->shape.rank==Rank::Matrix?kernel.polar_geometry.bins:source.actual[1];
    if(lane && polar)std::fill(target.begin(),target.end(),0.f);
    return load_plane(kernel,source,std::span(target),stride);
}
Status retain_plane(OperationKernelState& kernel,std::size_t lane,const DataFrameView& current,const FrameLimits& limits,bool polar,bool calibrated,bool empty_columns=false) noexcept {
    const auto* source=std::get_if<NumericView>(&current.payload);auto& target=lane?kernel.imaginary:kernel.input;
    if(!source || source->actual[1]>target.size() ||
       (source->actual[1]?source->actual[0]>target.size()/source->actual[1]:!empty_columns)){kernel.pending={};return Status::incompatible_contract;}
    if(calibrated && !amplitude_grid(kernel,*source)){kernel.pending={};return Status::incompatible_contract;}
    const auto valid=validate_values(*source,ValueDomain::FiniteOnly,limits);if(valid!=Status::ok){kernel.pending={};return valid;}
    const auto loaded=load_pair_plane(kernel,lane,*source,polar);if(loaded!=Status::ok)return loaded;
    return retain_interval(kernel,lane,current,*source,polar || source->contract->shape.rank==Rank::Matrix);
}
void copy_pair_coordinates(OperationKernelState& kernel,std::size_t from,std::size_t to) noexcept {
    for(std::size_t axis=0;axis<2;++axis){
        const auto& source=kernel.pair_coordinates[from][axis];auto& target=kernel.pair_coordinates[to][axis];
        std::copy_n(source.begin(),std::min(source.size(),target.size()),target.begin());
    }
}
Status missing_concat_geometry(OperationKernelState& kernel,const PlanNode& node,std::size_t lane,std::size_t other) noexcept {
    const auto contract=std::get<NumericContract>(node.endpoint_plan->inputs[other].contract);
    const auto axis=std::size_t(kernel.concat_axis),unchanged=1-axis;
    const auto actual=kernel.pair_actual[lane][unchanged];const auto bounds=contract.shape.extents[unchanged];
    if(actual<bounds.minimum || actual>bounds.maximum)return Status::incompatible_contract;
    kernel.pair_actual[other]=kernel.pair_actual[lane];
    kernel.pair_actual[other][axis]=std::size_t(contract.shape.extents[axis].maximum);
    return Status::ok;
}
Status prepare_missing_plane(OperationKernelState& kernel,const PlanNode& node,std::size_t lane,std::size_t other) noexcept {
    if(node.endpoint_plan->inputs[other].parent==absent){
        if(node.operation==ProcessingOperation::ComplexPolar){kernel.pending={};return Status::unavailable;}
        auto& target=other?kernel.imaginary:kernel.input;std::fill(target.begin(),target.end(),0);
        kernel.stamps[other]=kernel.stamps[lane];kernel.pair_actual[other]=kernel.pair_actual[lane];kernel.pending[other]=true;
        copy_pair_coordinates(kernel,lane,other);
        if(node.operation==ProcessingOperation::ArrayConcat)return missing_concat_geometry(kernel,node,lane,other);
    }
    return Status::ok;
}
bool pair_geometry_matches(const OperationKernelState& kernel,const PlanNode& node) noexcept {
    if(node.operation==ProcessingOperation::ArrayConcat)return kernel.pair_actual[0][1-std::size_t(kernel.concat_axis)]==kernel.pair_actual[1][1-std::size_t(kernel.concat_axis)];
    if(node.operation!=ProcessingOperation::ComplexPolar || node.node.kind==NodeKind::AmplitudePhase)return kernel.pair_actual[0]==kernel.pair_actual[1];
    return std::get<NumericContract>(node.numeric_input).shape.rank!=Rank::Matrix || kernel.pair_actual[0][0]==kernel.pair_actual[1][0];
}
Status validate_pending_pair(OperationKernelState& kernel,const PlanNode& node,std::size_t other) noexcept {
    if(node.operation==ProcessingOperation::ArrayConcat && !same_interval(kernel.stamps[0],kernel.stamps[1])){kernel.pending[other]=false;return Status::would_block;}
    if(!pair_geometry_matches(kernel,node)){kernel.pending={};return Status::incompatible_contract;}
    if(!same_interval(kernel.stamps[0],kernel.stamps[1])){kernel.pending[other]=false;return Status::would_block;}
    return Status::ok;
}
Status await_pair(OperationKernelState& kernel,const PlanNode& node,std::size_t lane) noexcept {
    if(node.node.kind==NodeKind::RealFourier)return Status::ok;
    const auto other=1-lane;
    const auto ready=prepare_missing_plane(kernel,node,lane,other);if(ready!=Status::ok)return ready;
    if(!kernel.pending[0] || !kernel.pending[1])return Status::would_block;
    return validate_pending_pair(kernel,node,other);
}
FrameStamp combined_stamp(const OperationKernelState& kernel,const DataFrameView& current,bool paired) noexcept {
    auto result=current.stamp;if(!paired)return result;
    result.discontinuity=kernel.stamps[0].discontinuity || kernel.stamps[1].discontinuity;
    result.frequency_dependent_latency=kernel.stamps[0].frequency_dependent_latency || kernel.stamps[1].frequency_dependent_latency;
    result.latency_seconds=latency_greater(kernel.stamps[0].latency_seconds,kernel.stamps[1].latency_seconds)?kernel.stamps[0].latency_seconds:kernel.stamps[1].latency_seconds;
    return result;
}
Status project_polar_value(OperationKernelState& kernel,const PlanNode& node,std::size_t i) noexcept {
        const auto geometry=kernel.polar_geometry;
        const auto bin=geometry.first_bin+i%geometry.bins;
        if(node.node.kind==NodeKind::AmplitudePhase && bin==0){
            if(!kernel.spectrum_scaling.dc_offset(kernel.input[i],kernel.output[2*i]))return Status::overflow;
            return Status::ok;
        }
        const bool endpoint=!bin || (!(geometry.fft_size%2) && bin==geometry.fft_size/2);
        if(!kernel.spectrum_scaling.project(kernel.input[i],kernel.imaginary[i],geometry.one_sided && !endpoint,kernel.output[2*i],endpoint))return Status::overflow;
        const auto options=kernel.polar_options;
        if(!dsp_kernels::complex_polar_value(kernel.input[i],kernel.imaginary[i],nullptr,&kernel.output[2*i+1],
            {options.zero_phase,options.magnitude_norm,options.real_endpoints && endpoint}))return Status::overflow;
    return Status::ok;
}
Status project_polar(OperationKernelState& kernel,const PlanNode& node) noexcept {
    const auto count=kernel.pair_actual[0][0]*kernel.pair_actual[0][1];
    for(std::size_t i=0;i<count;++i){
        const auto status=project_polar_value(kernel,node,i);if(status!=Status::ok)return status;
    }
    return Status::ok;
}
bool complex_row(OperationKernelState& kernel,std::span<const float> real,std::span<const float> imaginary,std::span<float> output) noexcept {
    if(auto* backend=std::get_if<dsp_kernels::PreparedFourierBackend>(&kernel.operation)){
        for(std::size_t i=0;i<real.size();++i){kernel.fourier_input[2*i]=real[i];kernel.fourier_input[2*i+1]=imaginary[i];}
        return backend->process(kernel.fourier_input,output);
    }
    return std::get<dsp_kernels::FFT>(kernel.operation).process_complex(real,imaginary,output);
}
bool real_row(OperationKernelState& kernel,std::span<const float> input,std::span<float> output) noexcept {
    if(auto* backend=std::get_if<dsp_kernels::PreparedFourierBackend>(&kernel.operation))return backend->process(input,output);
    return std::get<dsp_kernels::PreparedRfft>(kernel.operation).process(input,output);
}
Status precise_rows(OperationKernelState& kernel,std::size_t rows) noexcept {
    auto& transform=std::get<dsp_kernels::detail::PreparedReferenceFourier>(kernel.operation);
    const auto n=kernel.fourier_size;
    for(std::size_t row=0;row<rows;++row){
        if(!transform.forward(std::span<const double>(kernel.precise_input).subspan(row*n,n),kernel.precise_output))return Status::overflow;
        for(std::size_t i=0;i<n+2;++i){
            const auto value=kernel.precise_output[i];if(std::abs(value)>std::numeric_limits<float>::max())return Status::overflow;
            kernel.output[row*(n+2)+i]=float(value);
        }
    }
    return Status::ok;
}
Status transform_rows(OperationKernelState& kernel,const PlanNode& node) noexcept {
    const bool real=node.node.kind==NodeKind::RealFourier;
    const auto n=kernel.fourier_size,stride=real?n+2:2*n;
    const auto rows=std::get<NumericContract>(node.numeric_input).shape.rank==Rank::Matrix?kernel.pair_actual[0][0]:1;
    if(!kernel.precise_input.empty())return precise_rows(kernel,rows);
    for(std::size_t row=0;row<rows;++row){
        const auto input=std::span<const float>(kernel.input).subspan(row*n,n);
        auto output=std::span<float>(kernel.output).subspan(row*stride,stride);
        const bool ready=real?real_row(kernel,input,output):
            complex_row(kernel,input,std::span<const float>(kernel.imaginary).subspan(row*n,n),output);
        if(!ready)return Status::overflow;
    }
    return Status::ok;
}
Status concatenate_pair(OperationKernelState& kernel) noexcept {
    const auto left=kernel.pair_actual[0],right=kernel.pair_actual[1];
    if(kernel.concat_axis==ArrayConcatAxis::First){
        const auto count=left[0]*left[1];
        std::copy_n(kernel.input.begin(),count,kernel.output.begin());
        std::copy_n(kernel.imaginary.begin(),right[0]*right[1],kernel.output.begin()+count);
        return Status::ok;
    }
    const auto columns=left[1]+right[1];
    for(std::size_t row=0;row<left[0];++row){
        std::copy_n(kernel.input.begin()+row*left[1],left[1],kernel.output.begin()+row*columns);
        std::copy_n(kernel.imaginary.begin()+row*right[1],right[1],kernel.output.begin()+row*columns+left[1]);
    }
    return Status::ok;
}
Status compute_paired(OperationKernelState& kernel,const PlanNode& node) noexcept {
    if(!node.node.enabled){std::fill(kernel.output.begin(),kernel.output.end(),0);return Status::ok;}
    if(node.operation==ProcessingOperation::ArrayConcat)return concatenate_pair(kernel);
    if(node.operation==ProcessingOperation::ComplexPolar)return project_polar(kernel,node);
    return transform_rows(kernel,node);
}
std::size_t fourier_index(const OperationKernelState& kernel,const MutableNumericView& output,PortId port,std::size_t index,bool real) noexcept {
    const auto n=kernel.fourier_size;
    const auto row=output.contract->shape.rank==Rank::Matrix?index/output.actual[1]:0;
    const auto bin=output.contract->shape.rank==Rank::Matrix?index%output.actual[1]:index;
    return row*(real?n+2:2*n)+2*bin+port;
}
std::size_t polar_index(const OperationKernelState& kernel,const MutableNumericView& output,PortId port,std::size_t index,bool amplitude_phase) noexcept {
    const auto bins=kernel.polar_geometry.bins;
    if(amplitude_phase && port==2)return 2*index*bins;
    const auto row=output.contract->shape.rank==Rank::Matrix?index/output.actual[1]:0;
    const auto column=output.contract->shape.rank==Rank::Matrix?index%output.actual[1]:index;
    return 2*(row*bins+column+std::size_t(amplitude_phase))+port;
}
Status write_plane(const OperationKernelState& kernel,const PlanNode& node,MutableNumericView& output,PortId port,std::size_t count) noexcept {
    const bool concat=node.operation==ProcessingOperation::ArrayConcat,polar=node.operation==ProcessingOperation::ComplexPolar;
    for(std::size_t i=0;i<count;++i){
        const auto index=concat?i:polar?polar_index(kernel,output,port,i,node.node.kind==NodeKind::AmplitudePhase):
            fourier_index(kernel,output,port,i,node.node.kind==NodeKind::RealFourier);
        const auto status=store_element(output,{0,i/output.actual[1],i%output.actual[1]},kernel.output[index]);if(status!=Status::ok)return status;
    }
    return Status::ok;
}
Status fourier_coordinates(const OperationKernelState& kernel,State& state,MutableNumericView& output,PortId port) noexcept {
    if(output.contract->coordinates[0].mode!=CoordinateMode::ExplicitFloat64LE)return Status::ok;
    const auto bytes=output.actual[0]*sizeof(double);const auto& source=kernel.pair_coordinates[0][0];
    auto& destination=state.numeric_outputs[port].coordinates[0];
    if(source.size()<bytes || destination.size()<bytes)return Status::exhausted;
    std::copy_n(source.begin(),bytes,destination.begin());output.coordinates.axes[0]={destination.data(),bytes};return Status::ok;
}
bool coordinate_prefix_matches(const OperationKernelState& kernel,std::size_t axis) noexcept {
    const auto& left=kernel.pair_coordinates[0][axis];const auto& right=kernel.pair_coordinates[1][axis];
    const auto common=std::min(kernel.pair_actual[0][axis],kernel.pair_actual[1][axis])*sizeof(double);
    return left.size()>=common && right.size()>=common && std::equal(left.begin(),left.begin()+common,right.begin());
}
Status polar_coordinate_axis(const OperationKernelState& kernel,State& state,MutableNumericView& output,PortId port,std::size_t axis,std::size_t skip) noexcept {
    const auto bytes=output.actual[axis]*sizeof(double);
    if(!coordinate_prefix_matches(kernel,axis))return Status::incompatible_contract;
    const auto& source=kernel.pair_coordinates[port==2?0:port][axis];if(source.size()<bytes+skip)return Status::incompatible_contract;
    auto& destination=state.numeric_outputs[port].coordinates[axis];if(destination.size()<bytes)return Status::exhausted;
    std::copy_n(source.begin()+skip,bytes,destination.begin());output.coordinates.axes[axis]={destination.data(),bytes};
    return Status::ok;
}
Status polar_coordinates(const OperationKernelState& kernel,State& state,MutableNumericView& output,PortId port,bool amplitude_phase) noexcept {
    for(std::size_t axis=0;axis<2;++axis)if(output.contract->coordinates[axis].mode==CoordinateMode::ExplicitFloat64LE){
        const auto bin_axis=output.contract->shape.rank==Rank::Matrix?1u:0u;
        const auto skip=amplitude_phase && port!=2 && axis==bin_axis?sizeof(double):0;
        const auto status=polar_coordinate_axis(kernel,state,output,port,axis,skip);if(status!=Status::ok)return status;
    }
    return Status::ok;
}
MutableNumericView plane_output(const OperationKernelState& kernel,const PlanNode& node,State& state,PortId port) noexcept {
    auto output=mutable_output(node,state,port);
    if(node.operation==ProcessingOperation::ComplexPolar){
        if(node.node.kind!=NodeKind::AmplitudePhase)output.actual=kernel.pair_actual[port];
        else if(std::get<NumericContract>(node.numeric_input).shape.rank==Rank::Matrix)output.actual[0]=kernel.pair_actual[0][0];
    }
    if(node.operation==ProcessingOperation::Fourier && std::get<NumericContract>(node.numeric_input).shape.rank==Rank::Matrix)output.actual[0]=kernel.pair_actual[0][0];
    if(node.operation==ProcessingOperation::ArrayConcat){
        output.actual=kernel.pair_actual[0];
        const auto axis=std::size_t(kernel.concat_axis);output.actual[axis]+=kernel.pair_actual[1][axis];
    }
    return output;
}
Status concat_coordinates(const OperationKernelState& kernel,State& state,MutableNumericView& output) noexcept {
    const auto axis=1-std::size_t(kernel.concat_axis);
    if(output.contract->coordinates[axis].mode!=CoordinateMode::ExplicitFloat64LE)return Status::ok;
    const auto bytes=output.actual[axis]*sizeof(double);
    const auto& left=kernel.pair_coordinates[0][axis];const auto& right=kernel.pair_coordinates[1][axis];
    auto& target=state.numeric_outputs[0].coordinates[axis];
    if(left.size()<bytes || right.size()<bytes || target.size()<bytes)return Status::exhausted;
    if(!std::equal(left.begin(),left.begin()+bytes,right.begin()))return Status::incompatible_contract;
    std::copy_n(left.begin(),bytes,target.begin());output.coordinates.axes[axis]={target.data(),bytes};
    return Status::ok;
}
Status plane_coordinates(const OperationKernelState& kernel,const PlanNode& node,State& state,MutableNumericView& output,PortId port) noexcept {
    if(node.operation==ProcessingOperation::ComplexPolar)return polar_coordinates(kernel,state,output,port,node.node.kind==NodeKind::AmplitudePhase);
    if(node.operation==ProcessingOperation::ArrayConcat)return concat_coordinates(kernel,state,output);
    return node.operation==ProcessingOperation::Fourier?fourier_coordinates(kernel,state,output,port):Status::ok;
}
Status publish_planes(OperationKernelState& kernel,const PlanNode& node,State& state,const FrameStamp& stamp) noexcept {
    for(PortId port=0;port<state.numeric_frames.size();++port){
        auto output=plane_output(kernel,node,state,port);
        const auto count=output.actual[0]*output.actual[1];
        const auto coordinates=plane_coordinates(kernel,node,state,output,port);if(coordinates!=Status::ok)return coordinates;
        const auto status=write_plane(kernel,node,output,port,count);if(status!=Status::ok)return status;
        state.numeric_frames[port]={stamp,output.view()};
    }
    return Status::ok;
}
Status complete_signal_pair(OperationKernelState& kernel,const PlanNode& node,State& state,const DataFrameView& current) noexcept {
    const auto stamp=combined_stamp(kernel,current,node.node.kind!=NodeKind::RealFourier);kernel.pending={};
    const auto computed=compute_paired(kernel,node);if(computed!=Status::ok)return computed;
    return publish_planes(kernel,node,state,stamp);
}
}
namespace {
Status accept_signal_pair(OperationKernelState& kernel,const PlanNode& node,State& state,std::size_t lane,const DataFrameView& current,const FrameLimits& limits) noexcept {
    if(lane>=(node.node.kind==NodeKind::RealFourier?1u:2u))return Status::invalid_argument;
    if(!current.stamp.available){kernel.pending={};return Status::unavailable;}
    const auto retained=retain_plane(kernel,lane,current,limits,node.operation==ProcessingOperation::ComplexPolar,node.node.kind==NodeKind::AmplitudePhase,node.operation==ProcessingOperation::ArrayConcat);if(retained!=Status::ok)return retained;
    const auto paired=await_pair(kernel,node,lane);if(paired!=Status::ok)return paired;
    return complete_signal_pair(kernel,node,state,current);
}
}
Status accept_paired_operation(const PlanNode& node,State& state,std::size_t lane,const DataFrameView& current,const FrameLimits& limits) noexcept {
    auto& kernel=*std::get<PreparedOperationKernel>(state.kernel).value_;
    if(kernel.tempo)return kernel.tempo->accept(node,state,lane,current,limits);
    if(node.operation==ProcessingOperation::ArrayProduct)return accept_array_product(kernel,node,state,lane,current,limits);
    return accept_signal_pair(kernel,node,state,lane,current,limits);
}
}
