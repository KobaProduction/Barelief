// SPDX-License-Identifier: AGPL-3.0-or-later
#include "loudness_operations.h"
namespace rhythm_graph_adapters::detail {
using namespace signal_contracts;using namespace dataflow_runtime;using namespace rhythm_reference_ports;
namespace {
class Segment final:public LoudnessKernel<Segment> {
public:
    bool prepare(const LoudnessPlan& p,std::span<const float> borders){plan_=p;values_.resize(p.bands);return kernel_.prepare(p.segment,borders);}
    std::size_t workspace_bytes() const noexcept override {return kernel_.workspace_bytes()+values_.capacity()*sizeof(float);}
private:
    bool run(std::span<const NumericOperationInput> in,std::span<NumericOperationOutput> out) noexcept override {
        BeatSegmentEnergyResult result;
        return kernel_.process(in[0].contiguous_float32,result,values_) && batch_scalar(out[0],result.energy) && batch_vector(out[1],values_) &&
            batch_integer(out[2],result.onset_sample) && loudness_complex(out[3],kernel_.spectral_frame().complex_values);
    }
    PreparedBeatSegmentEnergy kernel_;std::vector<float> values_;
};
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
class Sequence final:public LoudnessKernel<Sequence> {
public:
    bool prepare(const LoudnessPlan& p,std::span<const float> borders){plan_=p;return kernel_.prepare(p.secondary,p.sequence,borders);}
    std::size_t workspace_bytes() const noexcept override {return kernel_.workspace_bytes();}
private:
    bool run(std::span<const NumericOperationInput> in,std::span<NumericOperationOutput> out) noexcept override {
        if(!kernel_.process(in[0].contiguous_float32,in[1].contiguous_float32) || !batch_vector(out[0],kernel_.energies()) ||
           !loudness_indices(out[2],kernel_.source_beat_indices()) || !loudness_shape(out[1],kernel_.rows(),plan_.bands))return false;
        for(std::size_t k=0;k<kernel_.rows();++k)if(!loudness_matrix_row(&out[1],k,kernel_.band_ratios(k)))return false;return true;
    }
    PreparedBeatLoudnessSequence kernel_;
};
template<class T> std::unique_ptr<AdapterOperation> make(const LoudnessPlan& p,std::span<const float> borders){auto value=std::make_unique<T>();return value->prepare(p,borders)?std::move(value):nullptr;}
}
std::unique_ptr<AdapterOperation> prepare_energy_kernel(const domain::ResolvedProcessingNode& n,const LoudnessPlan& p){
    const auto borders=loudness_borders(n,p.kind);
    switch(p.kind){case LoudnessKind::Segment:return make<Segment>(p,borders);case LoudnessKind::Ratios:return make<Ratios>(p,borders);case LoudnessKind::Frequency:return make<Frequency>(p,borders);case LoudnessKind::Sequence:return make<Sequence>(p,borders);default:return {};}
}
std::size_t energy_kernel_size(LoudnessKind kind) noexcept {
    switch(kind){case LoudnessKind::Segment:return sizeof(Segment);case LoudnessKind::Ratios:return sizeof(Ratios);case LoudnessKind::Frequency:return sizeof(Frequency);case LoudnessKind::Sequence:return sizeof(Sequence);default:return 0;}
}
}
