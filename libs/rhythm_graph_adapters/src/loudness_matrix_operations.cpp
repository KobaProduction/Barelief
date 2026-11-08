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
bool coordinate_vector(NumericOperationOutput& out,std::size_t n,double step) noexcept {
    if(!loudness_shape(out,n))return false;
    for(std::size_t k=0;k<n;++k)if(store_element(out.value,{0,k,0},double(k)*step)!=Status::ok)return false;return true;
}
template<class T> std::unique_ptr<AdapterOperation> make(const LoudnessPlan& p){auto value=std::make_unique<T>();return value->prepare(p)?std::move(value):nullptr;}
}
std::unique_ptr<AdapterOperation> prepare_rhythm_matrix_kernel(const dataflow_runtime::NumericOperationNodeView&,const LoudnessPlan& p){
    switch(p.kind){case LoudnessKind::Salience:return make<Salience>(p);default:return {};}
}
std::size_t rhythm_matrix_kernel_size(LoudnessKind kind) noexcept {
    switch(kind){case LoudnessKind::Salience:return sizeof(Salience);default:return 0;}
}
}
