// SPDX-License-Identifier: AGPL-3.0-or-later
#include "loudness_operations.h"
namespace rhythm_graph_adapters::detail {
using namespace signal_contracts;using namespace dataflow_runtime;using namespace rhythm_reference_ports;
namespace {
class Salience final:public LoudnessKernel<Salience> {
public:
    bool prepare(const LoudnessPlan& p){plan_=p;return kernel_.prepare(p.rows,p.columns,p.window);}
    std::size_t workspace_bytes() const noexcept override {return kernel_.workspace_bytes();}
private:
    bool run(std::span<const NumericOperationInput> in,std::span<NumericOperationOutput> out) noexcept override {
        if(in[0].value.actual[0]!=in[1].value.actual[0] || !kernel_.process(batch_rows(in[0]),in[1].contiguous_float32,in[0].value.actual[1]) ||
           !loudness_shape(out[0],kernel_.bands(),kernel_.beats()) || !batch_vector(out[1],kernel_.tick_thresholds()))return false;
        for(std::size_t k=0;k<kernel_.bands();++k)if(!loudness_matrix_row(&out[0],k,kernel_.band_values(k)))return false;return true;
    }
    PreparedBeatBandSalience kernel_;
};
class Meter final:public LoudnessKernel<Meter> {
public:
    bool prepare(const LoudnessPlan& p){plan_=p;return kernel_.prepare(p.columns,p.rows);}
    std::size_t workspace_bytes() const noexcept override {return kernel_.workspace_bytes();}
private:
    bool run(std::span<const NumericOperationInput> in,std::span<NumericOperationOutput> out) noexcept override {
        float meter=0;
        return kernel_.process(batch_rows(in[0]),in[0].value.actual[0],in[0].value.actual[1],meter) && batch_scalar(out[0],meter) &&
            batch_vector(out[1],kernel_.summed_correlation()) && batch_vector(out[2],kernel_.harmonic_histogram()) &&
            loudness_indices(out[3],kernel_.band_peak_lags()) && batch_vector(out[4],kernel_.band_peak_values());
    }
    PreparedCorrelationMeter kernel_;
};
bool coordinate_vector(NumericOperationOutput& out,std::size_t n,double step) noexcept {
    if(!loudness_shape(out,n))return false;
    for(std::size_t k=0;k<n;++k)if(store_element(out.value,{0,k,0},double(k)*step)!=Status::ok)return false;return true;
}
class Transform final:public LoudnessKernel<Transform> {
public:
    bool prepare(const LoudnessPlan& p){plan_=p;return kernel_.prepare(p.columns,p.transform);}
    std::size_t workspace_bytes() const noexcept override {return kernel_.workspace_bytes();}
private:
    bool run(std::span<const NumericOperationInput> in,std::span<NumericOperationOutput> out) noexcept override {
        const auto frames=kernel_.frames(in[0].value.actual[0]);
        return in[0].value.actual[1]==plan_.columns && loudness_shape(out[0],frames,kernel_.bins()) &&
            kernel_.process(batch_rows(in[0]),in[0].value.actual[0],{&out[0],loudness_matrix_row}) &&
            coordinate_vector(out[1],kernel_.bins(),double(plan_.analysis_rate)/double(plan_.transform.frame_size)) &&
            coordinate_vector(out[2],frames,double(plan_.transform.hop)/double(plan_.analysis_rate)) && loudness_complex(out[3],kernel_.spectral_frame().complex_values);
    }
    PreparedBandRhythmTransform kernel_;
};
class Activity final:public LoudnessKernel<Activity> {
public:
    bool prepare(const LoudnessPlan& p){plan_=p;return kernel_.prepare(p.rows,p.activity);}
    std::size_t workspace_bytes() const noexcept override {return kernel_.workspace_bytes();}
private:
    bool run(std::span<const NumericOperationInput> in,std::span<NumericOperationOutput> out) noexcept override {
        if(!kernel_.process(in[0].contiguous_float32))return false;const auto& scaling=kernel_.scaling();const auto& result=scaling.result();
        return batch_scalar(out[0],result.score) && batch_status(out[1],std::uint8_t(result.status)) && batch_integer(out[2],result.available_scales) &&
            loudness_indices(out[3],scaling.scales()) && batch_vector(out[4],scaling.fluctuations()) && batch_vector(out[5],scaling.exponents()) && batch_vector(out[6],kernel_.integrated_activity());
    }
    PreparedActivityFluctuation kernel_;
};
template<class T> std::unique_ptr<AdapterOperation> make(const LoudnessPlan& p){auto value=std::make_unique<T>();return value->prepare(p)?std::move(value):nullptr;}
}
std::unique_ptr<AdapterOperation> prepare_rhythm_matrix_kernel(const domain::ResolvedProcessingNode&,const LoudnessPlan& p){
    switch(p.kind){case LoudnessKind::Salience:return make<Salience>(p);case LoudnessKind::Meter:return make<Meter>(p);case LoudnessKind::Transform:return make<Transform>(p);case LoudnessKind::Activity:return make<Activity>(p);default:return {};}
}
std::size_t rhythm_matrix_kernel_size(LoudnessKind kind) noexcept {
    switch(kind){case LoudnessKind::Salience:return sizeof(Salience);case LoudnessKind::Meter:return sizeof(Meter);case LoudnessKind::Transform:return sizeof(Transform);case LoudnessKind::Activity:return sizeof(Activity);default:return 0;}
}
}
