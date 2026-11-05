// SPDX-License-Identifier: AGPL-3.0-or-later
#include "tracking_operations.h"
namespace rhythm_graph_adapters::detail {
using namespace rhythm_reference_ports;using namespace dataflow_runtime;using namespace signal_contracts;
namespace {
class Threshold final:public TrackingKernel<Threshold> {
public:
    bool prepare(const TrackingPlan& p){configure_plan(p);values_.resize(p.rows);return kernel_.prepare(p.rows,p.half);}
    std::size_t workspace_bytes() const noexcept override {return kernel_.workspace_bytes()+values_.capacity()*sizeof(float);}
private:
    bool run(std::span<const NumericOperationInput> in,std::span<NumericOperationOutput> out) noexcept override {const auto n=in[0].contiguous_float32.size();return kernel_.process(in[0].contiguous_float32,std::span(values_).first(n)) && batch_vector(out[0],std::span(values_).first(n));}
    PreparedMovingThreshold kernel_;std::vector<float> values_;
};
class PeriodPath final:public TrackingKernel<PeriodPath> {
public:
    bool prepare(const TrackingPlan& p){configure_plan(p);periods_.resize(p.rows);ends_.resize(p.rows);return kernel_.prepare(p.rows,p.path);}
    std::size_t workspace_bytes() const noexcept override {return kernel_.workspace_bytes()+(periods_.capacity()+ends_.capacity())*sizeof(float);}
private:
    bool run(std::span<const NumericOperationInput> in,std::span<NumericOperationOutput> out) noexcept override {
        const auto n=in[0].value.actual[0];if(!kernel_.process(batch_rows(in[0]),n,std::span(periods_).first(n),std::span(ends_).first(n)))return false;
        return batch_vector(out[0],std::span(periods_).first(n)) && batch_vector(out[1],std::span(ends_).first(n)) && tracking_matrix(out[2],kernel_.observations().first(n*tracking_.path.hop),n,tracking_.path.hop) &&
            tracking_unsigned_vector(out[3],kernel_.decoded_path().first(n)) && batch_vector(out[4],kernel_.last_correlation()) && batch_vector(out[5],kernel_.last_comb()) && batch_vector(out[6],kernel_.last_filtered());
    }
    PreparedPeriodPath kernel_;std::vector<float> periods_,ends_;
};
class Renewal final:public TrackingKernel<Renewal> {
public:
    bool prepare(const TrackingPlan& p){configure_plan(p);mask_.resize(p.rows);return kernel_.prepare(p.rows,p.counts[0],p.renewal);}
    std::size_t workspace_bytes() const noexcept override {return kernel_.workspace_bytes()+mask_.capacity();}
private:
    bool run(std::span<const NumericOperationInput> in,std::span<NumericOperationOutput> out) noexcept override {
        const auto n=in[0].contiguous_float32.size();return kernel_.process(in[0].contiguous_float32,in[1].contiguous_float32,in[2].contiguous_float32,std::span(mask_).first(n)) && tracking_mask(out[0],std::span(mask_).first(n)) && tracking_unsigned_vector(out[1],kernel_.state_path());
    }
    PreparedRenewalBeatDecoder kernel_;std::vector<std::uint8_t> mask_;
};
class PeriodBeat final:public TrackingKernel<PeriodBeat> {
public:
    bool prepare(const TrackingPlan& p){configure_plan(p);if(!kernel_.prepare(p.rows,p.beat))return false;const auto n=kernel_.output_samples(p.rows);mask_.resize(n);times_.resize(n);return true;}
    std::size_t workspace_bytes() const noexcept override {return kernel_.workspace_bytes()+mask_.capacity()+times_.capacity()*sizeof(float);}
private:
    bool run(std::span<const NumericOperationInput> in,std::span<NumericOperationOutput> out) noexcept override {
        const auto n=kernel_.output_samples(in[0].contiguous_float32.size());std::size_t count=0;
        if(!kernel_.process(in[0].contiguous_float32,std::span(mask_).first(n)) || !kernel_.beat_times(times_,count))return false;
        return tracking_mask(out[0],std::span(mask_).first(n)) && batch_vector(out[1],std::span(times_).first(count)) && batch_vector(out[2],kernel_.periods_seconds()) && batch_vector(out[3],kernel_.block_ends_seconds()) && batch_vector(out[4],kernel_.normalized_curve()) && tracking_unsigned_vector(out[5],kernel_.state_path());
    }
    PreparedPeriodBeatTracker kernel_;std::vector<std::uint8_t> mask_;std::vector<float> times_;
};
class Agreement final:public TrackingKernel<Agreement> {
public:
    bool prepare(const TrackingPlan& p){configure_plan(p);values_.resize(p.rows*p.columns);methods_.resize(p.rows);return kernel_.prepare(p.rows,p.columns,p.minimum_time);}
    std::size_t workspace_bytes() const noexcept override {return kernel_.workspace_bytes()+values_.capacity()*sizeof(float)+methods_.capacity()*sizeof(BeatSequenceView);}
    std::unique_ptr<PreparedNumericOperation> clone() const override {auto v=std::make_unique<Agreement>(*this);for(auto& method:v->methods_)method={};return v;}
private:
    bool load(std::span<const NumericOperationInput> in) noexcept {
        const auto rows=in[0].value.actual[0];if(in[1].value.actual[0]!=rows)return false;
        for(std::size_t r=0;r<rows;++r){std::uint64_t n=0;if(load_element(in[1].value,{0,r,0},n)!=Status::ok || n>in[0].value.actual[1])return false;
            auto row=std::span(values_).subspan(r*tracking_.columns,std::size_t(n));for(std::size_t c=0;c<row.size();++c)if(load_element(in[0].value,{0,r,c},row[c])!=Status::ok)return false;methods_[r]={row};}
        return true;
    }
    bool run(std::span<const NumericOperationInput> in,std::span<NumericOperationOutput> out) noexcept override {
        BeatAgreementSelection selection;const auto n=in[0].value.actual[0];if(!load(in) || !kernel_.process(std::span(methods_).first(n),selection))return false;
        return batch_integer(out[0],selection.selected) && batch_scalar(out[1],selection.confidence) && batch_vector(out[2],kernel_.method_scores()) && tracking_matrix(out[3],kernel_.pairwise_information_gain(),n,n);
    }
    PreparedBeatAgreement kernel_;std::vector<float> values_;std::vector<BeatSequenceView> methods_;
};
class MultiBeat final:public TrackingKernel<MultiBeat> {
public:
    bool prepare(const TrackingPlan& p){configure_plan(p);return kernel_.prepare(p.counts,p.multi);}
    std::size_t workspace_bytes() const noexcept override {return kernel_.workspace_bytes();}
private:
    bool run(std::span<const NumericOperationInput> in,std::span<NumericOperationOutput> out) noexcept override {
        std::array<OnsetCurveView,5> curves;for(std::size_t i=0;i<5;++i)curves[i]={in[i].contiguous_float32};
        if(!kernel_.process(curves) || !batch_vector(out[0],kernel_.ticks()) || !batch_scalar(out[1],kernel_.confidence()) || !batch_integer(out[2],kernel_.selected_method()) || !batch_vector(out[3],kernel_.method_scores()))return false;
        for(std::size_t i=0;i<5;++i)if(!batch_vector(out[4+i],kernel_.candidate_times(i)))return false;return true;
    }
    PreparedMultiFeatureBeatTracker kernel_;
};
template<class T> std::unique_ptr<AdapterOperation> make(const TrackingPlan& p){auto v=std::make_unique<T>();return v->prepare(p)?std::move(v):nullptr;}
}
std::unique_ptr<AdapterOperation> prepare_period_tracking(const domain::ResolvedProcessingNode&,const TrackingPlan& p){
    switch(p.kind){case TrackingKind::Threshold:return make<Threshold>(p);case TrackingKind::PeriodPath:return make<PeriodPath>(p);case TrackingKind::Renewal:return make<Renewal>(p);case TrackingKind::PeriodBeat:return make<PeriodBeat>(p);case TrackingKind::Agreement:return make<Agreement>(p);case TrackingKind::MultiBeat:return make<MultiBeat>(p);default:return {};}
}
std::size_t period_tracking_object_bytes(TrackingKind k) noexcept {
    switch(k){case TrackingKind::Threshold:return sizeof(Threshold);case TrackingKind::PeriodPath:return sizeof(PeriodPath);case TrackingKind::Renewal:return sizeof(Renewal);case TrackingKind::PeriodBeat:return sizeof(PeriodBeat);case TrackingKind::Agreement:return sizeof(Agreement);case TrackingKind::MultiBeat:return sizeof(MultiBeat);default:return 0;}
}
} // namespace rhythm_graph_adapters::detail
