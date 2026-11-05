// SPDX-License-Identifier: AGPL-3.0-or-later
#include "sequence_source_operations.h"
#include "sequence_source_io.h"
#include "operation_io.h"

namespace rhythm_graph_adapters::detail {
using namespace rhythm_reference_ports;
using namespace dataflow_runtime;
using namespace signal_contracts;
namespace {
struct TrajectoryWriter {
    std::span<NumericOperationOutput> outputs;
    mutable bool failed=false;
    static void frame(const void* context,const TrajectoryOnsetFrameStages& frame) noexcept {
        const auto& self=*static_cast<const TrajectoryWriter*>(context);
        auto* bands=output_at(self.outputs,2);auto* novelty=output_at(self.outputs,1);
        const auto row=frame.spectrum.frame_index;
        for(std::size_t band=0;band<frame.bands.size();++band)
            if(store_element(bands->value,{0,row,band},frame.bands[band])!=Status::ok)self.failed=true;
        if(frame.novelty_available && store_element(novelty->value,{0,frame.novelty_index,0},frame.novelty)!=Status::ok)self.failed=true;
    }
};
}
bool SequenceSourceOperation::calculate_stream(std::span<const NumericOperationInput> inputs,std::span<NumericOperationOutput> outputs) noexcept {
    if(plan_.kind==SequenceSourceKind::Activation)return std::get<PreparedActivationPeakSequence>(kernel_).process(inputs[0].contiguous_float32,plan_.reset_each_call);
    if(plan_.kind==SequenceSourceKind::Trajectory){
        const TrajectoryWriter writer{outputs};auto& source=std::get<PreparedTrajectoryOnsetRecipe>(kernel_);
        if(!source.process(inputs[0].contiguous_float32,{&writer,TrajectoryWriter::frame}) || writer.failed)return false;
        const auto frame=source.frame_stages();result_rows_=frame.spectrum.valid()?frame.spectrum.frame_index+1:0;return true;
    }
    auto& difference=*output_at(outputs,0);auto& current=*output_at(outputs,1);
    if(plan_.kind==SequenceSourceKind::Difference){
        auto& source=std::get<PreparedSpectralDifferenceSequence>(kernel_);
        if(!source.process(sequence_source_rows(inputs[0]),inputs[0].value.actual[0],sequence_source_writer(difference),plan_.reset_each_call,sequence_source_writer(current)))return false;
        result_rows_=source.output_frames();return true;
    }
    auto& source=std::get<PreparedPcmSpectralDifference>(kernel_);
    if(!source.process(inputs[0].contiguous_float32,sequence_source_writer(difference),plan_.reset_each_call,sequence_source_writer(current)))return false;
    result_rows_=source.output_frames();return true;
}
}
