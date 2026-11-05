// SPDX-License-Identifier: AGPL-3.0-or-later
#include "essentia_operations.h"
#include <dsp_kernels/prepared_storage.h>
#include <algorithm>

namespace rhythm_graph_adapters::detail {
using namespace rhythm_reference_ports;using namespace dataflow_runtime;using namespace signal_contracts;
namespace {
bool write_row(NumericOperationOutput& out,std::size_t row,std::span<const float> values) noexcept {
    for(std::size_t col=0;col<values.size();++col)
        if(store_element(out.value,{0,row,col},values[col])!=Status::ok)return false;
    return true;
}
class Rows final:public BatchKernel<Rows> {
public:
    bool prepare(const EssentiaPlan& p) {
        source_=p;plan_.kind=BatchKind::PulseTempo;plan_.outputs=p.outputs;
        std::copy_n(p.shapes.begin(),p.outputs,plan_.shape.begin());
        magnitude_.resize(p.bins);phase_.resize(p.bins);distances_.resize(p.bins);bands_.resize(40);
        if(p.kind==EssentiaKind::Onset)return onset_.prepare(p.bins,{SpectralOnsetMethod(p.method),44100});
        if(p.kind==EssentiaKind::Information)return gain_.prepare(p.bins,2,233);
        if(p.kind==EssentiaKind::Novelty)return novelty_.prepare(p.bins,PolarNovelty::BeatEmphasis);
        return erb_.prepare(p.bins,{});
    }
    std::size_t workspace_bytes() const noexcept override {
        return onset_.workspace_bytes()+gain_.workspace_bytes()+novelty_.workspace_bytes()+erb_.workspace_bytes()+
            (magnitude_.capacity()+phase_.capacity()+distances_.capacity()+bands_.capacity())*sizeof(float);
    }
    Status process(std::span<const NumericOperationInput> in,std::span<NumericOperationOutput> out,
        NumericOperationControl control) noexcept override {
        invalidate_outputs(out);
        for(const auto& input:in)if(!essentia_frequency_input(input))return Status::incompatible_contract;
        return BatchOperation::process(in,out,control);
    }
private:
    void reset_kernel() noexcept override {onset_.reset();gain_.reset();novelty_.reset();}
    bool read(std::span<const NumericOperationInput> in,std::size_t row) noexcept {
        const auto magnitude=batch_rows(in[0]);
        if(!magnitude.read(magnitude.context,row,magnitude_))return false;
        if(in.size()==2){const auto phase=batch_rows(in[1]);if(!phase.read(phase.context,row,phase_))return false;}
        return std::all_of(magnitude_.begin(),magnitude_.end(),[](float x){return x>=0;});
    }
    bool onset_row(std::size_t row,std::span<NumericOperationOutput> out) noexcept {
        float value=0;
        if(!onset_.process(magnitude_,phase_,value) || store_element(out[0].value,{0,row,0},value)!=Status::ok)return false;
        return out.size()==2 || write_row(out[2],row,onset_.intermediate_values());
    }
    bool row(std::size_t i,std::span<NumericOperationOutput> out) noexcept {
        if(source_.kind==EssentiaKind::Onset)return onset_row(i,out);
        if(source_.kind==EssentiaKind::Information){
            float value=0;return gain_.process(magnitude_,value) && store_element(out[0].value,{0,i,0},value)==Status::ok;
        }
        if(source_.kind==EssentiaKind::Novelty)return novelty_.process(magnitude_,phase_,distances_) && write_row(out[0],i,distances_);
        return erb_.process(magnitude_,bands_) && write_row(out[0],i,bands_);
    }
    bool run(std::span<const NumericOperationInput> in,std::span<NumericOperationOutput> out) noexcept override {
        if(in.size()==2 && (in[0].value.actual!=in[1].value.actual || !equal_frame_identity(in[0],in[1])))return false;
        reset_kernel();const auto count=in[0].value.actual[0];
        output_shapes(out,count);
        if(!rows(in,out,count))return false;
        return source_.kind!=EssentiaKind::Erb || erb_outputs(out);
    }
    void output_shapes(std::span<NumericOperationOutput> out,std::size_t count) const noexcept {
        for(std::size_t j=0;j<source_.outputs;++j)
            out[j].value.actual={source_.shapes[j].variable?count:source_.shapes[j].rows,source_.shapes[j].columns};
    }
    bool erb_outputs(std::span<NumericOperationOutput> out) const noexcept {
        if(!batch_vector(out[1],erb_.centers_hz()))return false;
        for(std::size_t band=0;band<40;++band)
            if(!write_row(out[2],band,erb_.weights().subspan(band*source_.bins,source_.bins)))return false;
        return true;
    }
    bool rows(std::span<const NumericOperationInput> in,std::span<NumericOperationOutput> out,std::size_t count) noexcept {
        const bool timed=source_.kind==EssentiaKind::Onset || source_.kind==EssentiaKind::Information;
        for(std::size_t i=0;i<count;++i){
            if(!read(in,i) || !row(i,out))return false;
            if(timed && store_element(out[1].value,{0,i,0},double(i)/double(source_.frame_rate))!=Status::ok)return false;
        }
        return true;
    }
    EssentiaPlan source_;
    PreparedSpectralOnset onset_;PreparedInformationGain gain_;
    PreparedPolarNovelty novelty_;PreparedErbFilterBank erb_;
    std::vector<float> magnitude_,phase_,distances_,bands_;
};
class Agreement final:public BatchKernel<Agreement> {
public:
    bool prepare(const EssentiaPlan& p) {
        plan_.kind=BatchKind::PulseTempo;plan_.outputs=p.outputs;
        std::copy_n(p.shapes.begin(),p.outputs,plan_.shape.begin());
        return kernel_.prepare(5,p.rows,p.minimum_time);
    }
    std::size_t workspace_bytes() const noexcept override {return kernel_.workspace_bytes();}
private:
    bool run(std::span<const NumericOperationInput> in,std::span<NumericOperationOutput> out) noexcept override {
        std::array<BeatSequenceView,5> candidates;
        for(std::size_t i=0;i<5;++i)candidates[i]={in[i].contiguous_float32};
        BeatAgreementSelection selection;
        if(!kernel_.process(candidates,selection) || selection.selected>=5)return false;
        if(!batch_vector(out[0],candidates[selection.selected].seconds) || !batch_scalar(out[1],selection.confidence) ||
           !batch_integer(out[2],selection.selected) || !batch_vector(out[3],kernel_.method_scores()))return false;
        return pairwise(out[4]);
    }
    bool pairwise(NumericOperationOutput& out) const noexcept {
        out.value.actual={5,5};
        for(std::size_t i=0;i<5;++i)
            if(!write_row(out,i,kernel_.pairwise_information_gain().subspan(i*5,5)))return false;
        return true;
    }
    PreparedBeatAgreement kernel_;
};
// A current-session barrier preserves the source algorithm's shared RNG order.
// All five trackers remain separate operations downstream of these output ports.
class CandidateInputs final:public BatchKernel<CandidateInputs> {
public:
    bool prepare(const EssentiaPlan& p) {
        plan_.kind=BatchKind::PulseTempo;plan_.outputs=p.outputs;
        std::copy_n(p.shapes.begin(),p.outputs,plan_.shape.begin());return true;
    }
    std::size_t workspace_bytes() const noexcept override {return 0;}
private:
    bool run(std::span<const NumericOperationInput> in,std::span<NumericOperationOutput> out) noexcept override {
        for(std::size_t i=0;i<5;++i)
            if(!batch_vector(out[i],in[i].contiguous_float32))return false;
        return true;
    }
};
template<class T> std::unique_ptr<AdapterOperation> make(const EssentiaPlan& p) {
    auto value=std::make_unique<T>();return value->prepare(p)?std::move(value):nullptr;
}
bool row_storage(const EssentiaPlan& p,std::size_t& bytes) noexcept {
    using namespace dsp_kernels::prepared_storage;
    if(p.kind==EssentiaKind::Onset && !PreparedSpectralOnset::storage_bytes(p.bins,{SpectralOnsetMethod(p.method),44100},bytes))return false;
    if(p.kind==EssentiaKind::Information && !PreparedInformationGain::storage_bytes(p.bins,2,233,bytes))return false;
    if(p.kind==EssentiaKind::Novelty && !PreparedPolarNovelty::storage_bytes(p.bins,PolarNovelty::BeatEmphasis,bytes))return false;
    if(p.kind==EssentiaKind::Erb && !PreparedErbFilterBank::storage_bytes(p.bins,{},bytes))return false;
    return add_array(bytes,p.bins,3*sizeof(float)) && add_array(bytes,40,sizeof(float));
}
}
bool essentia_operation_storage(const EssentiaPlan& p,std::size_t& bytes) noexcept {
    bytes=0;
    if(p.kind==EssentiaKind::CandidateInputs)return true;
    if(p.kind==EssentiaKind::Agreement)return PreparedBeatAgreement::storage_bytes(5,p.rows,p.minimum_time,bytes);
    return row_storage(p,bytes);
}
std::size_t essentia_operation_object_bytes(EssentiaKind kind) noexcept {
    if(kind==EssentiaKind::CandidateInputs)return sizeof(CandidateInputs);
    return kind==EssentiaKind::Agreement?sizeof(Agreement):sizeof(Rows);
}
std::unique_ptr<AdapterOperation> prepare_essentia_operation(const domain::ConfiguredProcessingNode& node) {
    EssentiaPlan p;if(!essentia_operation_plan(definition_view(node),p))return {};
    if(p.kind==EssentiaKind::CandidateInputs)return make<CandidateInputs>(p);
    return p.kind==EssentiaKind::Agreement?make<Agreement>(p):make<Rows>(p);
}
}
