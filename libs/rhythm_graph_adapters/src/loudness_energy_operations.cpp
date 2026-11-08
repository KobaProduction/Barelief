// SPDX-License-Identifier: AGPL-3.0-or-later
#include "loudness_operations.h"
namespace rhythm_graph_adapters::detail {
using namespace signal_contracts;using namespace dataflow_runtime;using namespace rhythm_reference_ports;
namespace {
class Ratios final:public LoudnessKernel<Ratios> {
public:
    bool prepare(const LoudnessPlan& p,std::span<const float> borders){plan_=p;values_.resize(p.bands);return kernel_.prepare(p.rows,p.rate,borders);}
    std::size_t workspace_bytes() const noexcept override {return kernel_.workspace_bytes()+values_.capacity()*sizeof(float);}
private:
    bool run(std::span<const NumericOperationInput> in,std::span<NumericOperationOutput> out) noexcept override {
        float energy=0;return kernel_.process(in[0].contiguous_float32,energy,values_) && batch_scalar(out[0],energy) && batch_vector(out[1],values_);
    }
    PreparedSpectralBandRatios kernel_;std::vector<float> values_;
};
class Frequency final:public LoudnessKernel<Frequency> {
public:
    bool prepare(const LoudnessPlan& p,std::span<const float> borders){plan_=p;values_.resize(p.bands);return kernel_.prepare(p.rows,p.rate,borders);}
    std::size_t workspace_bytes() const noexcept override {return kernel_.workspace_bytes()+values_.capacity()*sizeof(float);}
private:
    bool run(std::span<const NumericOperationInput> in,std::span<NumericOperationOutput> out) noexcept override {
        return kernel_.process(in[0].contiguous_float32,values_) && batch_vector(out[0],values_);
    }
    PreparedFrequencyBandEnergy kernel_;std::vector<float> values_;
};
template<class T> std::unique_ptr<AdapterOperation> make(const LoudnessPlan& p,std::span<const float> borders){auto value=std::make_unique<T>();return value->prepare(p,borders)?std::move(value):nullptr;}
}
std::unique_ptr<AdapterOperation> prepare_energy_kernel(const dataflow_runtime::NumericOperationNodeView& n,const LoudnessPlan& p){
    const auto borders=loudness_borders(n,p.kind);
    switch(p.kind){case LoudnessKind::Ratios:return make<Ratios>(p,borders);case LoudnessKind::Frequency:return make<Frequency>(p,borders);default:return {};}
}
std::size_t energy_kernel_size(LoudnessKind kind) noexcept {
    switch(kind){case LoudnessKind::Ratios:return sizeof(Ratios);case LoudnessKind::Frequency:return sizeof(Frequency);default:return 0;}
}
}
