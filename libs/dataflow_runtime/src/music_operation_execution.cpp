#include "operation_kernel_state.h"
#include "node_state.h"
#include "preparation_support.h"
#include "frame_access.h"
#include "signal_time.h"
#include <algorithm>
#include <cmath>
#include <limits>
namespace dataflow_runtime::detail {
using namespace domain;using namespace signal_contracts;
namespace {
Status write_musical_vector(const OperationKernelState& kernel,const dsp_kernels::MusicalSpectrogram& feature,PortId port,MutableNumericView& out) noexcept {
    const auto offset=port==3?feature.offsets()[3]:0;
    for(std::size_t i=0;i<out.actual[0];++i){
        const auto status=store_element(out,{0,i,0},kernel.output[offset+i]);
        if(status!=Status::ok)return status;
    }
    return Status::ok;
}
Status write_musical_output(OperationKernelState& kernel,const PlanNode& n,const dsp_kernels::MusicalSpectrogram& feature,const dsp_kernels::PeakGridAgreement& agreement,PortId p,MutableNumericView& out,FrameStamp& stamp) noexcept {
    Status status=Status::ok;
            if(n.node.kind==NodeKind::NoteSpectrum && p==note_first_output)status=store_element(out,{0,0,0},kernel.note_first_midi);
            else if(p==1 || p==2){status=store_element(out,{0,0,0},float(p==1?agreement.full_percent:agreement.selected_percent));stamp.available=stamp.available && (p==1?agreement.full_defined:agreement.selected_defined);}
            else status=write_musical_vector(kernel,feature,p,out);
    return status;
}
Status publish_musical_outputs(OperationKernelState& kernel,const PlanNode& n,State& state,const DataFrameView& current,const dsp_kernels::MusicalSpectrogram& feature,const dsp_kernels::PeakGridAgreement& agreement) noexcept {
    Status status=Status::ok;
        const PortId outputs=n.node.kind==NodeKind::BroadbandRow?1:4;
        for(PortId p=0;p<outputs;++p){auto out=mutable_output(n,state,p);auto stamp=current.stamp;
            status=write_musical_output(kernel,n,feature,agreement,p,out,stamp);
            if(status!=Status::ok)return status;
            state.numeric_frames[p]={stamp,out.view()};
        }
    return Status::ok;
}
}
Status execute_musical_spectrogram(OperationKernelState& kernel,const PlanNode& n,State& state,const DataFrameView& current) noexcept {
    auto* feature=std::get_if<dsp_kernels::MusicalSpectrogram>(&kernel.operation);
    if(!feature)return Status::unavailable;
    {
        dsp_kernels::PeakGridAgreement agreement;
        if(n.node.enabled){
            if(!feature->process(kernel.input,kernel.output,agreement))return Status::invalid_argument;
        }else std::fill(kernel.output.begin(),kernel.output.end(),0);
        return publish_musical_outputs(kernel,n,state,current,*feature,agreement);
    }

}
}
