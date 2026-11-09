// Adapted from madmom ml/hmm.pyx at
// CPJKU/madmom@27f032e8947204902c675e5e341a3faf5dc86dae.
// Copyright (c) 2012-2014 Department of Computational Perception, JKU Linz
// and Austrian Research Institute for Artificial Intelligence (OFAI), Vienna.
// BSD-style notice/disclaimer: packaging/licenses/madmom.txt.
#include <dsp_kernels/sparse_markov.h>
#include <dsp_kernels/prepared_storage.h>
#include <dsp_kernels/filter_projection.h>
#include <algorithm>
#include <cmath>
#include <numeric>
#include <utility>

namespace dsp_kernels {
namespace {
constexpr float negative_infinity=-std::numeric_limits<float>::infinity();
bool log_value(float value) noexcept {return std::isfinite(value) || value==negative_infinity;}
bool probability_distribution(std::span<const float> values) noexcept {
    return std::all_of(values.begin(),values.end(),[](float x){return std::isfinite(x) && std::abs(x-1.f)<=1.001e-5f;});
}
bool transition_order(const MarkovTransition& a,const MarkovTransition& b) noexcept {
    return a.to<b.to || (a.to==b.to && a.from<b.from);
}
void stable_edges(std::span<MarkovTransition> values,std::span<MarkovTransition> scratch) {
    for(std::size_t width=1;width<values.size();width=width>values.size()/2?values.size():2*width){
        for(std::size_t first=0;first<values.size();){
            const auto middle=first+std::min(width,values.size()-first);
            const auto last=middle+std::min(width,values.size()-middle);
            std::merge(values.begin()+std::ptrdiff_t(first),values.begin()+std::ptrdiff_t(middle),
                values.begin()+std::ptrdiff_t(middle),values.begin()+std::ptrdiff_t(last),scratch.begin()+std::ptrdiff_t(first),transition_order);
            first=last;
        }
        std::copy(scratch.begin(),scratch.end(),values.begin());
    }
}
bool dimensions(SparseMarkovView model,std::size_t density,std::size_t previous,std::size_t current) noexcept {
    return model.states() && density==model.states() && previous==density && current==density &&
        model.offsets.back()==model.predecessors.size() &&
        model.probabilities.size()==model.predecessors.size() && model.log_probabilities.size()==model.predecessors.size();
}
}
void PreparedSparseMarkovTransitions::append_edge(const MarkovTransition& edge) {
    if(predecessors_.empty() || !offsets_[edge.to+1] || predecessors_.back()!=edge.from){
        predecessors_.push_back(edge.from);probabilities_.push_back(edge.probability);++offsets_[edge.to+1];
    }else probabilities_.back()+=edge.probability;
}
void PreparedSparseMarkovTransitions::finish_offsets() {
    for(std::size_t i=1;i<offsets_.size();++i)offsets_[i]+=offsets_[i-1];
    logs_.resize(probabilities_.size());
    std::transform(probabilities_.begin(),probabilities_.end(),logs_.begin(),[](float p){return std::log(p);});
}
bool PreparedSparseMarkovTransitions::append(std::span<const MarkovTransition> edges,std::size_t states) {
    offsets_.assign(states+1,0);
    predecessors_.reserve(edges.size());probabilities_.reserve(edges.size());logs_.reserve(edges.size());
    for(const auto& edge:edges)append_edge(edge);
    finish_offsets();
    return true;
}
bool PreparedSparseMarkovTransitions::configure(std::size_t states,std::span<const MarkovTransition> input) {
    if(!states || states>UINT32_MAX || input.empty() || input.size()>UINT32_MAX)return false;
    std::vector<MarkovTransition> edges(input.begin(),input.end());std::vector<float> outgoing(states,0.f);
    for(const auto& edge:edges){
        if(edge.from>=states || edge.to>=states || !std::isfinite(edge.probability) || edge.probability<0 || edge.probability>1)return false;
        outgoing[edge.from]+=edge.probability;
    }
    if(!probability_distribution(outgoing))return false;
    std::vector<MarkovTransition> scratch(edges.size());stable_edges(edges,scratch);return append(edges,states);
}
bool PreparedSparseMarkovTransitions::prepare(std::size_t states,std::span<const MarkovTransition> edges) {
    std::size_t bound=0;if(!storage_bytes(states,edges.size(),bound))return false;
    PreparedSparseMarkovTransitions next;
    if(!next.configure(states,edges))return false;
    if(next.workspace_bytes()>bound)return false;
    *this=std::move(next);return true;
}
namespace {
bool viterbi_state(SparseMarkovView model,std::size_t state,float density,std::span<const float> previous,float& current,std::uint32_t& backlink) noexcept {
    if(!log_value(density))return false;
    float best=negative_infinity;
    std::uint32_t origin=0;
    for(std::size_t edge=model.offsets[state];edge<model.offsets[state+1];++edge){
        const auto from=model.predecessors[edge];
        const float score=previous[from]+model.log_probabilities[edge]+density;
        if(score>best){best=score;origin=from;}
    }
    current=best;backlink=origin;
    return log_value(best);
}
bool forward_state(SparseMarkovView model,std::size_t state,float density,std::span<const float> previous,float& current,float& total) noexcept {
    if(!std::isfinite(density) || density<0)return false;
    float sum=0;
    for(std::size_t edge=model.offsets[state];edge<model.offsets[state+1];++edge)
        sum+=previous[model.predecessors[edge]]*model.probabilities[edge];
    current=sum*density;total+=current;
    return true;
}
bool normalize_forward(std::span<float> current,float total) noexcept {
    if(!std::isfinite(total) || total<=0)return false;
    const float factor=1.f/total;
    if(!std::isfinite(factor))return false;
    for(auto& value:current)value*=factor;
    return true;
}
}
bool sparse_viterbi_step(SparseMarkovView model,std::span<const float> density,std::span<const float> previous,
                         std::span<float> current,std::span<std::uint32_t> backlinks) noexcept {
    if(!dimensions(model,density.size(),previous.size(),current.size()) || backlinks.size()!=current.size())return false;
    for(std::size_t state=0;state<current.size();++state)
        if(!viterbi_state(model,state,density[state],previous,current[state],backlinks[state]))return false;
    return true;
}
bool sparse_forward_step(SparseMarkovView model,std::span<const float> density,std::span<const float> previous,
                         std::span<float> current) noexcept {
    if(!dimensions(model,density.size(),previous.size(),current.size()))return false;
    float total=0;
    for(std::size_t state=0;state<current.size();++state)
        if(!forward_state(model,state,density[state],previous,current[state],total))return false;
    return normalize_forward(current,total);
}
bool PreparedSparseMarkovModel::initial(std::span<const float> values) {
    initial_.assign(pointers_.size(),1.f/float(pointers_.size()));
    if(values.empty())return true;
    {
        if(values.size()!=initial_.size() || !std::all_of(values.begin(),values.end(),[](float x){return std::isfinite(x) && x>=0;}))return false;
        std::copy(values.begin(),values.end(),initial_.begin());
    }
    std::vector<float> ones(initial_.size(),1.f);float sum=0;
    return project_filter_weights(initial_,ones,sum) && std::abs(sum-1.f)<=1.001e-5f;
}
void PreparedSparseMarkovModel::allocate(std::size_t frames,std::size_t densities,std::size_t states) {
    backlinks_.resize(frames*states);path_.resize(frames);row_.resize(densities);state_densities_.resize(states);
    previous_.resize(states);current_.resize(states);forward_previous_.resize(states);forward_current_.resize(states);
}
bool PreparedSparseMarkovModel::configure(std::size_t frames,std::size_t densities,std::span<const std::uint32_t> pointers,
                                         std::span<const MarkovTransition> edges,std::span<const float> initial_values) {
    if(!frames || !densities || pointers.empty() || frames>std::numeric_limits<std::size_t>::max()/pointers.size() ||
       !std::all_of(pointers.begin(),pointers.end(),[densities](auto p){return p<densities;}))return false;
    if(!transitions_.prepare(pointers.size(),edges))return false;
    pointers_.assign(pointers.begin(),pointers.end());maximum_frames_=frames;density_count_=densities;
    if(!initial(initial_values))return false;
    allocate(frames,densities,pointers.size());
    reset();return true;
}
bool PreparedSparseMarkovModel::prepare(std::size_t frames,std::size_t densities,std::span<const std::uint32_t> pointers,
                                       std::span<const MarkovTransition> edges,std::span<const float> initial_values) {
    std::size_t bound=0;if(!storage_bytes(frames,densities,pointers.size(),edges.size(),bound))return false;
    PreparedSparseMarkovModel next;
    if(!next.configure(frames,densities,pointers,edges,initial_values))return false;
    if(next.workspace_bytes()>bound)return false;
    *this=std::move(next);return true;
}
void PreparedSparseMarkovModel::reset() noexcept {
    result_frames_=0;probability_=negative_infinity;status_=MarkovResultStatus::Unprocessed;
    std::copy(initial_.begin(),initial_.end(),forward_previous_.begin());
}
bool PreparedSparseMarkovModel::load(FloatMatrixSource source,std::size_t frame,bool logarithmic) noexcept {
    if(!source.read(source.context,frame,row_))return false;
    const auto valid=[logarithmic](float x){return logarithmic?log_value(x):(std::isfinite(x) && x>=0);};
    if(!std::all_of(row_.begin(),row_.end(),valid))return false;
    for(std::size_t state=0;state<pointers_.size();++state)state_densities_[state]=row_[pointers_[state]];
    return true;
}
bool PreparedSparseMarkovModel::trace(std::size_t frames) noexcept {
    const auto best=std::max_element(previous_.begin(),previous_.end());
    probability_=*best;
    if(probability_==negative_infinity){status_=MarkovResultStatus::NoPath;return true;}
    if(!std::isfinite(probability_)){status_=MarkovResultStatus::ArithmeticFailure;return false;}
    auto state=std::uint32_t(best-previous_.begin());
    for(std::size_t frame=frames;frame-->0;){path_[frame]=state;state=backlinks_[frame*pointers_.size()+state];}
    result_frames_=frames;status_=MarkovResultStatus::Ready;return true;
}
bool PreparedSparseMarkovModel::viterbi_frame(FloatMatrixSource source,std::size_t frame) noexcept {
    if(!load(source,frame,true) || !sparse_viterbi_step(transitions_.view(),state_densities_,previous_,current_,
            std::span(backlinks_).subspan(frame*pointers_.size(),pointers_.size())))return false;
    previous_.swap(current_);
    return true;
}
bool PreparedSparseMarkovModel::forward_frame(FloatMatrixSource source,std::size_t frame,FloatMatrixDestination output) noexcept {
    if(!load(source,frame,false) || !sparse_forward_step(transitions_.view(),state_densities_,forward_previous_,forward_current_))return false;
    forward_previous_.swap(forward_current_);
    path_[frame]=std::uint32_t(std::max_element(forward_previous_.begin(),forward_previous_.end())-forward_previous_.begin());
    return !output.write || output.write(output.context,frame,forward_previous_);
}
bool PreparedSparseMarkovModel::viterbi(FloatMatrixSource source,std::size_t frames) noexcept {
    result_frames_=0;probability_=negative_infinity;status_=MarkovResultStatus::InvalidInput;
    if(!maximum_frames_ || !source.read || frames>maximum_frames_)return false;
    if(!frames){status_=MarkovResultStatus::Ready;return true;}
    std::transform(initial_.begin(),initial_.end(),previous_.begin(),[](float p){return std::log(p);});
    for(std::size_t frame=0;frame<frames;++frame)if(!viterbi_frame(source,frame))return false;
    return trace(frames);
}
bool PreparedSparseMarkovModel::forward(FloatMatrixSource source,std::size_t frames,bool reset_state,FloatMatrixDestination output) noexcept {
    result_frames_=0;probability_=negative_infinity;status_=MarkovResultStatus::InvalidInput;
    if(!maximum_frames_ || !source.read || frames>maximum_frames_)return false;
    if(reset_state)reset();
    for(std::size_t frame=0;frame<frames;++frame)if(!forward_frame(source,frame,output))return false;
    result_frames_=frames;status_=MarkovResultStatus::Ready;return true;
}
}

namespace dsp_kernels {
bool PreparedSparseMarkovTransitions::storage_bytes(std::size_t states,std::size_t edges,std::size_t& out) noexcept {
    if(!states || states>UINT32_MAX || !edges || edges>UINT32_MAX)return false;
    std::size_t bytes=0;
    if(!prepared_storage::add_array(bytes,states+1,sizeof(std::size_t)) ||
       !prepared_storage::add_array(bytes,edges,sizeof(std::uint32_t)+2*sizeof(float)+2*sizeof(MarkovTransition)) ||
       !prepared_storage::add_array(bytes,states,sizeof(float)))return false;
    out=bytes;return true;
}
std::size_t PreparedSparseMarkovTransitions::workspace_bytes() const noexcept {
    return offsets_.capacity()*sizeof(std::size_t)+predecessors_.capacity()*sizeof(std::uint32_t)+(probabilities_.capacity()+logs_.capacity())*sizeof(float);
}
bool PreparedSparseMarkovModel::storage_bytes(std::size_t frames,std::size_t densities,std::size_t states,std::size_t edges,std::size_t& out) noexcept {
    std::size_t bytes=0,backlinks=0;
    if(!frames || !densities || !PreparedSparseMarkovTransitions::storage_bytes(states,edges,bytes) ||
       !prepared_storage::mul_bytes(frames,states,backlinks) || !prepared_storage::add_array(bytes,backlinks,sizeof(std::uint32_t)) ||
       !prepared_storage::add_array(bytes,frames,sizeof(std::uint32_t)) ||
       !prepared_storage::add_array(bytes,states,sizeof(std::uint32_t)+7*sizeof(float)) ||
       !prepared_storage::add_array(bytes,densities,sizeof(float)))return false;
    out=bytes;return true;
}
std::size_t PreparedSparseMarkovModel::workspace_bytes() const noexcept {
    return transitions_.workspace_bytes()+(pointers_.capacity()+backlinks_.capacity()+path_.capacity())*sizeof(std::uint32_t)+
        (initial_.capacity()+row_.capacity()+state_densities_.capacity()+previous_.capacity()+current_.capacity()+forward_previous_.capacity()+forward_current_.capacity())*sizeof(float);
}
}
