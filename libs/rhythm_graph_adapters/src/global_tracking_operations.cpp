// SPDX-License-Identifier: AGPL-3.0-or-later
#include "tracking_operations.h"
namespace rhythm_graph_adapters::detail {
using namespace dataflow_runtime;using namespace rhythm_reference_ports;using namespace signal_contracts;
namespace {
template<class T> bool typed_vector(NumericOperationOutput& out,std::span<const T> values) noexcept {
    out.value.actual={values.size(),1};for(std::size_t i=0;i<values.size();++i)if(store_element(out.value,{0,i,0},values[i])!=Status::ok)return false;return true;
}
bool number64(NumericOperationOutput& out,double value) noexcept {out.value.actual={1,1};return store_element(out.value,{0,0,0},value)==Status::ok;}
class InformationGain final:public TrackingKernel<InformationGain> {
public:
    bool prepare(const TrackingPlan& p){configure_plan(p);return kernel_.prepare(p.rows,p.first,p.stop);}
    std::size_t workspace_bytes() const noexcept override {return kernel_.workspace_bytes();}
private:
    void reset_kernel() noexcept override {kernel_.reset();}
    bool run(std::span<const NumericOperationInput> in,std::span<NumericOperationOutput> out) noexcept override {float value=0;return kernel_.process(in[0].contiguous_float32,value) && batch_scalar(out[0],value);}
    PreparedInformationGain kernel_;
};
class Emphasis final:public TrackingKernel<Emphasis> {
public:
    bool prepare(const TrackingPlan& p){configure_plan(p);result_.resize(p.rows);return kernel_.prepare(p.rows,p.emphasis);}
    std::size_t workspace_bytes() const noexcept override {return kernel_.workspace_bytes()+result_.capacity()*sizeof(float);}
private:
    bool run(std::span<const NumericOperationInput> in,std::span<NumericOperationOutput> out) noexcept override {
        const auto rows=in[0].value.actual[0];return kernel_.process(batch_rows(in[0]),rows,std::span(result_).first(rows)) && batch_vector(out[0],std::span(result_).first(rows)) && batch_vector(out[1],kernel_.band_weights());
    }
    PreparedBandBeatEmphasis kernel_;std::vector<float> result_;
};
class Erb final:public TrackingKernel<Erb> {
public:
    bool prepare(const TrackingPlan& p){configure_plan(p);result_.resize(p.erb.bands);return kernel_.prepare(p.rows,p.erb);}
    std::size_t workspace_bytes() const noexcept override {return kernel_.workspace_bytes()+result_.capacity()*sizeof(float);}
private:
    bool run(std::span<const NumericOperationInput> in,std::span<NumericOperationOutput> out) noexcept override {
        return kernel_.process(in[0].contiguous_float32,result_) && batch_vector(out[0],result_) && batch_vector(out[1],kernel_.centers_hz()) && tracking_matrix(out[2],kernel_.weights(),tracking_.erb.bands,tracking_.rows);
    }
    PreparedErbFilterBank kernel_;std::vector<float> result_;
};
class Global final:public TrackingKernel<Global> {
public:
    bool prepare(const TrackingPlan& p){configure_plan(p);return kernel_.prepare(p.rows,p.global);}
    std::size_t workspace_bytes() const noexcept override {return kernel_.workspace_bytes();}
private:
    bool run(std::span<const NumericOperationInput> in,std::span<NumericOperationOutput> out) noexcept override {
        if(!kernel_.process(in[0].contiguous_float32))return false;const auto stage=kernel_.frame_stages();
        return batch_vector(out[0],kernel_.curve()) && batch_vector(out[1],kernel_.band_weights()) && batch_vector(out[2],stage.distances) && batch_vector(out[3],stage.erb_values) && batch_vector(out[4],stage.erb_centers_hz) && tracking_frame_outputs(out,5,stage.spectrum);
    }
    PreparedGlobalOnsetRecipe kernel_;
};
class PcmBeat final:public TrackingKernel<PcmBeat> {
public:
    bool prepare(const TrackingPlan& p){configure_plan(p);return kernel_.prepare(p.rows,p.pcm);}
    std::size_t workspace_bytes() const noexcept override {return kernel_.workspace_bytes();}
private:
    bool run(std::span<const NumericOperationInput> in,std::span<NumericOperationOutput> out) noexcept override {
        if(!kernel_.process(in[0].contiguous_float32) || !batch_vector(out[0],kernel_.ticks()) || !batch_scalar(out[1],kernel_.confidence()) || !batch_vector(out[2],kernel_.method_scores()))return false;
        for(std::size_t i=0;i<5;++i)if(!batch_vector(out[3+i],kernel_.onset_curve(i)) || !batch_vector(out[8+i],kernel_.candidate_times(i)))return false;
        return tracking_frame_outputs(out,13,kernel_.frame_stages().spectrum);
    }
    PreparedPcmBeatRecipe kernel_;
};
template<class T> std::unique_ptr<AdapterOperation> make(const TrackingPlan& p){auto value=std::make_unique<T>();return value->prepare(p)?std::move(value):nullptr;}
}
bool tracking_unsigned_vector(NumericOperationOutput& out,std::span<const std::uint32_t> values) noexcept {return typed_vector(out,values);}
bool tracking_mask(NumericOperationOutput& out,std::span<const std::uint8_t> values) noexcept {return typed_vector(out,values);}
bool tracking_matrix(NumericOperationOutput& out,std::span<const float> values,std::size_t rows,std::size_t columns) noexcept {
    if(!columns || rows>SIZE_MAX/columns || values.size()!=rows*columns)return false;out.value.actual={rows,columns};
    for(std::size_t r=0;r<rows;++r)for(std::size_t c=0;c<columns;++c)if(store_element(out.value,{0,r,c},values[r*columns+c])!=Status::ok)return false;return true;
}
bool tracking_frame_outputs(std::span<NumericOperationOutput> out,std::size_t first,const SpectralFrameStages& stage) noexcept {
    return batch_vector(out[first],stage.magnitude) && batch_vector(out[first+1],stage.phase) && tracking_matrix(out[first+2],stage.fourier.complex_values,stage.fourier.complex_values.size()/2,2) &&
        batch_vector(out[first+3],stage.fourier.windowed_input) && batch_vector(out[first+4],stage.fourier.transform_input) && batch_vector(out[first+5],stage.window_coefficients) && number64(out[first+6],stage.first_sample) && batch_scalar(out[first+7],stage.sample_rate);
}
std::unique_ptr<AdapterOperation> prepare_global_tracking(const domain::ResolvedProcessingNode&,const TrackingPlan& p){
    if(p.global_recipe)return make<Global>(p);
    switch(p.kind){case TrackingKind::InformationGain:return make<InformationGain>(p);case TrackingKind::BandEmphasis:return make<Emphasis>(p);case TrackingKind::Erb:return make<Erb>(p);case TrackingKind::PcmBeat:return make<PcmBeat>(p);default:return {};}
}
std::size_t global_tracking_object_bytes(TrackingKind k,bool global) noexcept {
    if(global)return sizeof(Global);switch(k){case TrackingKind::InformationGain:return sizeof(InformationGain);case TrackingKind::BandEmphasis:return sizeof(Emphasis);case TrackingKind::Erb:return sizeof(Erb);case TrackingKind::PcmBeat:return sizeof(PcmBeat);default:return 0;}
}
} // namespace rhythm_graph_adapters::detail
