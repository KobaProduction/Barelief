// SPDX-License-Identifier: AGPL-3.0-or-later
#include "tracking_operations.h"
namespace rhythm_graph_adapters::detail {
using namespace rhythm_reference_ports;using namespace dataflow_runtime;using namespace signal_contracts;
namespace {
class PeriodPath final:public TrackingKernel<PeriodPath> {
public:
    bool prepare(const TrackingPlan& p){configure_plan(p);periods_.resize(p.rows);ends_.resize(p.rows);return kernel_.prepare(p.rows,p.path);}
    std::size_t workspace_bytes() const noexcept override {return kernel_.workspace_bytes()+(periods_.capacity()+ends_.capacity())*sizeof(float);}
private:
    bool run(std::span<const NumericOperationInput> in,std::span<NumericOperationOutput> out) noexcept override {
        const auto n=in[0].value.actual[0];if(!kernel_.process(batch_rows(in[0]),n,std::span(periods_).first(n),std::span(ends_).first(n)))return false;
        return batch_vector(out[0],std::span(periods_).first(n)) && batch_vector(out[1],std::span(ends_).first(n)) && tracking_matrix(out[2],kernel_.observations().first(n*tracking_.path.hop),n,tracking_.path.hop) &&
            tracking_unsigned_vector(out[3],kernel_.decoded_path().first(n));
    }
    PreparedPeriodPath kernel_;std::vector<float> periods_,ends_;
};
class Renewal final:public TrackingKernel<Renewal> {
public:
    bool prepare(const TrackingPlan& p){configure_plan(p);mask_.resize(p.rows);times_.resize(p.rows);return kernel_.prepare(p.rows,p.counts[0],p.renewal);}
    std::size_t workspace_bytes() const noexcept override {return kernel_.workspace_bytes()+mask_.capacity()+times_.capacity()*sizeof(float);}
private:
    bool run(std::span<const NumericOperationInput> in,std::span<NumericOperationOutput> out) noexcept override {
        const auto n=in[0].contiguous_float32.size();
        if(!kernel_.process(in[0].contiguous_float32,in[1].contiguous_float32,in[2].contiguous_float32,std::span(mask_).first(n)))return false;
        std::size_t count=0;
        const float resolution=1.f/(tracking_.renewal.frame_rate*float(tracking_.renewal.upsampling));
        for(std::size_t i=0;i<n;++i)if(mask_[i])times_[count++]=float(i)*resolution;
        return tracking_mask(out[0],std::span(mask_).first(n)) && tracking_unsigned_vector(out[1],kernel_.state_path()) &&
            batch_vector(out[2],std::span(times_).first(count));
    }
    PreparedRenewalBeatDecoder kernel_;std::vector<std::uint8_t> mask_;std::vector<float> times_;
};
class Agreement final:public TrackingKernel<Agreement> {
public:
    bool prepare(const TrackingPlan& p){configure_plan(p);values_.resize(p.rows*p.columns);methods_.resize(p.rows);return kernel_.prepare(p.rows,p.columns,p.minimum_time);}
    std::size_t workspace_bytes() const noexcept override {return kernel_.workspace_bytes()+values_.capacity()*sizeof(float)+methods_.capacity()*sizeof(BeatSequenceView);}
    std::unique_ptr<PreparedNumericOperation> clone() const override {auto v=std::make_unique<Agreement>(*this);for(auto& method:v->methods_)method={};return v;}
private:
    bool load_row(std::span<const NumericOperationInput> in,std::size_t r) noexcept {
        std::uint64_t n=0;if(load_element(in[1].value,{0,r,0},n)!=Status::ok || n>in[0].value.actual[1])return false;
        auto row=std::span(values_).subspan(r*tracking_.columns,std::size_t(n));
        for(std::size_t c=0;c<row.size();++c)if(load_element(in[0].value,{0,r,c},row[c])!=Status::ok)return false;
        methods_[r]={row};return true;
    }
    bool load(std::span<const NumericOperationInput> in) noexcept {
        const auto rows=in[0].value.actual[0];if(in[1].value.actual[0]!=rows)return false;
        for(std::size_t r=0;r<rows;++r)if(!load_row(in,r))return false;
        return true;
    }
    bool run(std::span<const NumericOperationInput> in,std::span<NumericOperationOutput> out) noexcept override {
        BeatAgreementSelection selection;const auto n=in[0].value.actual[0];if(!load(in) || !kernel_.process(std::span(methods_).first(n),selection))return false;
        return batch_integer(out[0],selection.selected) && batch_scalar(out[1],selection.confidence) && batch_vector(out[2],kernel_.method_scores()) && tracking_matrix(out[3],kernel_.pairwise_information_gain(),n,n);
    }
    PreparedBeatAgreement kernel_;std::vector<float> values_;std::vector<BeatSequenceView> methods_;
};
template<class T> std::unique_ptr<AdapterOperation> make(const TrackingPlan& p){auto v=std::make_unique<T>();return v->prepare(p)?std::move(v):nullptr;}
}
std::unique_ptr<AdapterOperation> prepare_period_tracking(const dataflow_runtime::NumericOperationNodeView&,const TrackingPlan& p){
    switch(p.kind){case TrackingKind::PeriodPath:return make<PeriodPath>(p);case TrackingKind::Renewal:return make<Renewal>(p);case TrackingKind::Agreement:return make<Agreement>(p);default:return {};}
}
std::size_t period_tracking_object_bytes(TrackingKind k) noexcept {
    switch(k){case TrackingKind::PeriodPath:return sizeof(PeriodPath);case TrackingKind::Renewal:return sizeof(Renewal);case TrackingKind::Agreement:return sizeof(Agreement);default:return 0;}
}
} // namespace rhythm_graph_adapters::detail
