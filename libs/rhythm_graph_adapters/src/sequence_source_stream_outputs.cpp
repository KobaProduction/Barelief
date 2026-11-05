// SPDX-License-Identifier: AGPL-3.0-or-later
#include "sequence_source_operations.h"
#include "sequence_source_io.h"
#include "pcm_stage_write.h"

namespace rhythm_graph_adapters::detail {
using namespace rhythm_reference_ports;
using namespace dataflow_runtime;
using namespace signal_contracts;
namespace {
Status copied_matrix(NumericOperationOutput& output,std::size_t rows,std::size_t columns,const FrameStamp& stamp) noexcept {
    output.value.actual={rows,columns};output.stamp=stamp;output.status=Status::ok;return Status::ok;
}
Status optional_row(NumericOperationOutput& output,std::span<const float> values,const FrameStamp& stamp) noexcept {
    return values.empty()?Status::ok:write_vector(output,values,stamp);
}
Status trajectory_value(NumericOperationOutput& output,const PreparedTrajectoryOnsetRecipe& source,std::size_t frames,std::size_t bands,const FrameStamp& stamp) noexcept {
    const auto port=output.port;const auto frame=source.frame_stages();
    if(!port)return write_vector(output,source.onsets(),stamp);
    if(port==1)return copied_matrix(output,frames>2?frames-2:0,1,stamp);
    if(port==2)return copied_matrix(output,frames,bands,stamp);
    if(port==8)return write_pcm_scalar(output,std::uint64_t(source.acquisition_size()),stamp);
    if(!frame.spectrum.valid())return Status::ok;
    if(port==3)return write_pcm_complex(output,frame.spectrum.fourier.complex_values,stamp);
    if(port==4)return write_vector(output,frame.spectrum.magnitude,stamp);
    if(port==5)return sequence_source_centers(output,frame.band_centers_hz,stamp);
    if(port==6)return write_pcm_scalar(output,std::int64_t(frame.spectrum.first_sample),stamp);
    if(port==7)return write_pcm_scalar(output,std::uint64_t(frame.spectrum.frame_index),stamp);
    return write_vector(output,port==9?frame.spectrum.fourier.transform_input:frame.spectrum.window_coefficients,stamp);
}
}
Status SequenceSourceOperation::publish_stream(NumericOperationOutput& output,const FrameStamp& stamp) noexcept {
    const auto port=output.port;
    if(plan_.kind==SequenceSourceKind::Trajectory)return trajectory_value(output,std::get<PreparedTrajectoryOnsetRecipe>(kernel_),result_rows_,plan_.features,stamp);
    if(plan_.kind==SequenceSourceKind::Activation){
        const auto& source=std::get<PreparedActivationPeakSequence>(kernel_);
        if(!port)return write_vector(output,source.events(),stamp);
        return port==1?write_pcm_scalar(output,source.frame_counter(),stamp):write_pcm_scalar(output,std::uint64_t(source.buffer_frames()),stamp);
    }
    if(port<2)return copied_matrix(output,result_rows_,plan_.features,stamp);
    if(plan_.kind==SequenceSourceKind::Difference)return write_pcm_scalar(output,std::uint64_t(result_rows_),stamp);
    if(!result_rows_)return Status::ok;
    const auto& source=std::get<PreparedPcmSpectralDifference>(kernel_).spectrum_stage();const auto frame=source.values();
    if(port==2)return frame.complex.empty()?Status::ok:write_pcm_complex(output,frame.complex,stamp);
    if(port==3)return optional_row(output,frame.magnitude,stamp);
    if(port==4)return optional_row(output,frame.powered,stamp);
    if(port==5)return optional_row(output,frame.linear,stamp);
    if(port==6)return optional_row(output,frame.features,stamp);
    return port==7?write_vector(output,source.window_coefficients(),stamp):sequence_source_centers(output,source.source_filter_centers_hz(),stamp);
}
}
