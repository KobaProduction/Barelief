// SPDX-License-Identifier: AGPL-3.0-or-later
#include "batch_operation_plan.h"
#include <dsp_kernels/prepared_storage.h>
namespace rhythm_graph_adapters::detail {
using namespace dataflow_runtime;using namespace rhythm_reference_ports;using namespace signal_contracts;
namespace {
bool write_histogram_frame(const PreparedFourierTempoHistogram& k,std::size_t frame,std::span<NumericOperationOutput> out,std::size_t& index) noexcept {
    const auto values=k.analysis_tempogram_frame(frame);const auto peaks=k.raw_peaks(frame);
    for(std::size_t bin=0;bin<values.size();++bin)if(store_element(out[3].value,{0,frame,bin},values[bin])!=Status::ok)return false;
    if(store_element(out[9].value,{0,frame,0},std::uint64_t(peaks.size()))!=Status::ok)return false;
    for(const auto peak:peaks){if(store_element(out[7].value,{0,index,0},peak.position)!=Status::ok || store_element(out[7].value,{0,index,1},peak.value)!=Status::ok)return false;++index;}
    return true;
}
bool histogram_outputs(const PreparedFourierTempoHistogram& k,const BatchPlan& p,std::span<NumericOperationOutput> out) noexcept {
    if(!batch_scalar(out[0],k.bpm()) || !batch_peaks(out[1],k.candidates()) || !batch_vector(out[2],k.frame_bpms()) ||
       !batch_vector(out[4],k.pulse()) || !batch_peaks(out[5],k.ticks()) || !batch_status(out[6],std::uint8_t(k.status())) || !batch_peaks(out[8],k.observations()) || !batch_vector(out[10],k.histogram()))return false;
    const auto frames=k.collected_frames();out[3].value.actual={frames,p.shape[3].columns};out[9].value.actual={frames,1};
    std::size_t peak_count=0;for(std::size_t frame=0;frame<frames;++frame)peak_count+=k.raw_peaks(frame).size();
    out[7].value.actual={peak_count,2};std::size_t index=0;
    for(std::size_t frame=0;frame<frames;++frame)if(!write_histogram_frame(k,frame,out,index))return false;
    return true;
}
class FourierHistogram final:public BatchKernel<FourierHistogram> {
public:
    bool prepare(const domain::ResolvedProcessingNode& n,const BatchPlan& p){plan_=p;return kernel_.prepare(p.rows,p.columns,array_parameter(n,"synthesis_window"),p.histogram);}
    std::size_t workspace_bytes() const noexcept override {return kernel_.workspace_bytes();}
private:
    void reset_kernel() noexcept override {kernel_.reset(true);}
    bool run(std::span<const NumericOperationInput> in,std::span<NumericOperationOutput> out) noexcept override {
        if(in[0].value.actual!=in[1].value.actual || in[0].value.actual[1]!=plan_.columns)return false;
        return kernel_.process(batch_rows(in[0]),batch_rows(in[1]),in[0].value.actual[0]) && histogram_outputs(kernel_,plan_,out);
    }
    PreparedFourierTempoHistogram kernel_;
};
class NoveltyHistogram final:public BatchKernel<NoveltyHistogram> {
public:
    bool prepare(const domain::ResolvedProcessingNode& n,const BatchPlan& p){plan_=p;return kernel_.prepare(p.rows,p.novelty,array_parameter(n,"raw_window"));}
    std::size_t workspace_bytes() const noexcept override {return kernel_.workspace_bytes();}
private:
    void reset_kernel() noexcept override {kernel_.reset(true);}
    bool run(std::span<const NumericOperationInput> in,std::span<NumericOperationOutput> out) noexcept override {
        return kernel_.process(in[0].contiguous_float32) && histogram_outputs(kernel_.result(),plan_,out) && batch_vector(out[11],kernel_.analysis_window()) && batch_vector(out[12],kernel_.synthesis_window());
    }
    PreparedNoveltyTempoHistogram kernel_;
};
class PhasePulse final:public BatchKernel<PhasePulse> {
public:
    bool prepare(const domain::ResolvedProcessingNode& n,const BatchPlan& p){plan_=p;const auto& c=p.histogram.path;return kernel_.prepare(p.rows,p.columns,array_parameter(n,"synthesis_window"),{c.frame_rate,c.frequency_step,c.hop});}
    std::size_t workspace_bytes() const noexcept override {return kernel_.workspace_bytes();}
private:
    void reset_kernel() noexcept override {kernel_.reset();}
    bool run(std::span<const NumericOperationInput> in,std::span<NumericOperationOutput> out) noexcept override {
        if(in[0].value.actual[0]!=in[1].value.actual[0] || in[0].value.actual[1]!=plan_.columns)return false;
        return kernel_.process(batch_rows(in[0]),in[1].contiguous_float32) && batch_vector(out[0],kernel_.pulse()) && batch_peaks(out[1],kernel_.ticks()) &&
            batch_vector(out[2],kernel_.last_unwrapped_phase()) && batch_vector(out[3],kernel_.last_synthesis_frame());
    }
    PreparedPhaseTempoPulse kernel_;
};
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
std::unique_ptr<AdapterOperation> prepare_histogram_batch(const domain::ResolvedProcessingNode& n,const BatchPlan& p){
    if(p.kind==BatchKind::FramePath){auto value=std::make_unique<FramePath>();return value->prepare(p)?std::move(value):nullptr;}
    if(p.kind==BatchKind::PhasePulse){auto value=std::make_unique<PhasePulse>();return value->prepare(n,p)?std::move(value):nullptr;}
    if(p.kind==BatchKind::FourierHistogram){auto value=std::make_unique<FourierHistogram>();return value->prepare(n,p)?std::move(value):nullptr;}
    if(p.kind==BatchKind::NoveltyHistogram){auto value=std::make_unique<NoveltyHistogram>();return value->prepare(n,p)?std::move(value):nullptr;}
    return {};
}
std::size_t histogram_batch_object_size(BatchKind kind) noexcept {
    switch(kind){case BatchKind::FramePath:return sizeof(FramePath);case BatchKind::PhasePulse:return sizeof(PhasePulse);case BatchKind::FourierHistogram:return sizeof(FourierHistogram);case BatchKind::NoveltyHistogram:return sizeof(NoveltyHistogram);default:return 0;}
}
}
