// SPDX-License-Identifier: AGPL-3.0-or-later
#include "batch_operation_plan.h"
#include <dsp_kernels/prepared_storage.h>
namespace rhythm_graph_adapters::detail {
using namespace dataflow_runtime;using namespace rhythm_reference_ports;using namespace signal_contracts;
namespace {
struct PeakRows {const NumericOperationInput* values;const NumericOperationInput* counts;std::span<InterpolatedPeak> row;};
bool valid_peak_rows(const PeakRows& source) noexcept {
    for(std::size_t frame=0;frame<source.counts->value.actual[0];++frame){
        std::uint64_t count=0;if(load_element(source.counts->value,{0,frame,0},count)!=Status::ok || count>source.row.size())return false;
        for(std::size_t peak=0;peak<count;++peak){float position=0;if(load_element(source.values->value,{0,frame,2*peak},position)!=Status::ok || position<0)return false;}
    }
    return true;
}
std::span<const InterpolatedPeak> read_peaks(const void* context,std::size_t index) noexcept {
    const auto& source=*static_cast<const PeakRows*>(context);std::uint64_t count=0;
    if(index>=source.values->value.actual[0] || load_element(source.counts->value,{0,index,0},count)!=Status::ok || count>source.row.size())return {};
    for(std::size_t peak=0;peak<count;++peak)
        if(load_element(source.values->value,{0,index,2*peak},source.row[peak].position)!=Status::ok || load_element(source.values->value,{0,index,2*peak+1},source.row[peak].value)!=Status::ok)return {};
    return source.row.first(std::size_t(count));
}
class FramePath final:public BatchKernel<FramePath> {
public:
    bool prepare(const BatchPlan& p){plan_=p;if(!kernel_.prepare(p.rows,p.histogram.path))return false;row_.resize(p.columns/2);return true;}
    std::size_t workspace_bytes() const noexcept override {return kernel_.workspace_bytes()+row_.capacity()*sizeof(InterpolatedPeak);}
private:
    void reset_kernel() noexcept override {kernel_.reset(true);}
    bool run(std::span<const NumericOperationInput> in,std::span<NumericOperationOutput> out) noexcept override {
        if(in[0].value.actual[0]!=in[1].value.actual[0] || in[0].value.actual[1]!=plan_.columns)return false;
        float main=0;if(load_element(in[2].value,{0,0,0},main)!=Status::ok || main<0)return false;
        PeakRows source{&in[0],&in[1],row_};if(!valid_peak_rows(source))return false;
        return kernel_.process({&source,read_peaks},in[0].value.actual[0],main) && batch_vector(out[0],kernel_.values()) && batch_scalar(out[1],kernel_.mean_bpm()) && batch_status(out[2],std::uint8_t(kernel_.status()));
    }
    PreparedFrameTempoPath kernel_;std::vector<InterpolatedPeak> row_;
};
}
std::unique_ptr<AdapterOperation> prepare_histogram_batch(const dataflow_runtime::NumericOperationNodeView&,const BatchPlan& p){
    if(p.kind==BatchKind::FramePath){auto value=std::make_unique<FramePath>();return value->prepare(p)?std::move(value):nullptr;}
    return {};
}
std::size_t histogram_batch_object_size(BatchKind kind) noexcept {
    switch(kind){case BatchKind::FramePath:return sizeof(FramePath);default:return 0;}
}
}
