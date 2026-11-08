// SPDX-License-Identifier: AGPL-3.0-or-later
#include "tracking_operations.h"
namespace rhythm_graph_adapters::detail {
using namespace dataflow_runtime;using namespace rhythm_reference_ports;using namespace signal_contracts;
namespace {
template<class T> bool typed_vector(NumericOperationOutput& out,std::span<const T> values) noexcept {
    out.value.actual={values.size(),1};for(std::size_t i=0;i<values.size();++i)if(store_element(out.value,{0,i,0},values[i])!=Status::ok)return false;return true;
}
class InformationGain final:public TrackingKernel<InformationGain> {
public:
    bool prepare(const TrackingPlan& p){configure_plan(p);return kernel_.prepare(p.rows,p.first,p.stop);}
    std::size_t workspace_bytes() const noexcept override {return kernel_.workspace_bytes();}
private:
    void reset_kernel() noexcept override {kernel_.reset();}
    bool run(std::span<const NumericOperationInput> in,std::span<NumericOperationOutput> out) noexcept override {float value=0;return kernel_.process(in[0].contiguous_float32,value) && batch_scalar(out[0],value);}
    PreparedInformationGain kernel_;
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
template<class T> std::unique_ptr<AdapterOperation> make(const TrackingPlan& p){auto value=std::make_unique<T>();return value->prepare(p)?std::move(value):nullptr;}
}
bool tracking_unsigned_vector(NumericOperationOutput& out,std::span<const std::uint32_t> values) noexcept {return typed_vector(out,values);}
bool tracking_mask(NumericOperationOutput& out,std::span<const std::uint8_t> values) noexcept {return typed_vector(out,values);}
bool tracking_matrix(NumericOperationOutput& out,std::span<const float> values,std::size_t rows,std::size_t columns) noexcept {
    if(!columns || rows>SIZE_MAX/columns || values.size()!=rows*columns)return false;out.value.actual={rows,columns};
    for(std::size_t r=0;r<rows;++r)for(std::size_t c=0;c<columns;++c)if(store_element(out.value,{0,r,c},values[r*columns+c])!=Status::ok)return false;return true;
}
std::unique_ptr<AdapterOperation> prepare_global_tracking(const dataflow_runtime::NumericOperationNodeView&,const TrackingPlan& p){
    switch(p.kind){case TrackingKind::InformationGain:return make<InformationGain>(p);case TrackingKind::Erb:return make<Erb>(p);default:return {};}
}
std::size_t global_tracking_object_bytes(TrackingKind k) noexcept {
    switch(k){case TrackingKind::InformationGain:return sizeof(InformationGain);case TrackingKind::Erb:return sizeof(Erb);default:return 0;}
}
} // namespace rhythm_graph_adapters::detail
