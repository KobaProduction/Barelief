// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include "essentia_reference_fixture.h"
#include <stdexcept>

namespace essentia_reference {
inline constexpr std::size_t finite_samples=9*44100+17,finite_capacity=finite_samples+4096;
inline const ConfiguredProcessingNode& finite_node(const NumericProcessingGraph& g,NodeId id) {
    const auto found=std::find_if(g.nodes.begin(),g.nodes.end(),[=](const auto& node){return node.id==id;});
    check(found!=g.nodes.end(),"finite endpoint exists");return *found;
}
inline NumericProcessingGraph finite_graph(EssentiaReferenceMethod method) {
    auto result=numeric_essentia_finite_reference_example({finite_samples,finite_capacity},method).graph;
    std::erase_if(result.nodes,[](const auto& node){const auto kind=node_kind(node.kind);return node.id<3 || (kind && is_view(*kind));});
    std::erase_if(result.edges,[&](const auto& edge){
        return std::none_of(result.nodes.begin(),result.nodes.end(),[&](const auto& node){return node.id==edge.from;}) ||
            std::none_of(result.nodes.begin(),result.nodes.end(),[&](const auto& node){return node.id==edge.to;});
    });
    return result;
}
inline void finite_bounds_rejected(EssentiaFiniteOptions options) {
    bool rejected=false;
    try{static_cast<void>(numeric_essentia_finite_reference_example(options));}
    catch(const std::invalid_argument&){rejected=true;}
    check(rejected,"invalid finite geometry/rate rejected before preparation");
}
inline void finite_admission() {
    const auto example=numeric_essentia_finite_reference_example({finite_samples,finite_capacity});
    check(std::string_view(example.id)=="essentia-finite-multifeature","separate whole-input identity");
    const auto& batch=finite_node(example.graph,3);const auto& stream=finite_node(example.graph,1);
    const auto batch_contract=std::get<NumericContract>(batch.outputs[0].contract.view());
    const auto stream_contract=std::get<NumericContract>(stream.outputs[0].contract.view());
    check(batch.kind=="NetworkInput" && batch.inputs.empty(),"finite PCM uses explicit second ingress");
    check(batch_contract.shape.extents[0]==ExtentBounds{1,finite_capacity},"actual finite extent is independent of capacity");
    check(batch_contract.clock.sample_rate==44100 && batch_contract.clock.tick_seconds==Ratio{1,44100},"finite native clock");
    check(batch_contract.coordinates[0].step==Ratio{1,44100},"finite seconds coordinates");
    check(stream_contract.shape.extents[0]==ExtentBounds{1,4096},"stream accepts real partial packets");
    check(std::none_of(example.graph.nodes.begin(),example.graph.nodes.end(),[](const auto& node){
        return node.id==6 || node.id==7 || node.id==100 || node.id==101 || node.kind=="FrameBuffer";
    }),"finite recipe omits eight-second segmentation and fixed preview row");
    finite_bounds_rejected({0,finite_capacity});finite_bounds_rejected({finite_samples,finite_samples-1});
    finite_bounds_rejected({finite_samples,finite_capacity,48000});
}
inline void compare_finite(const Observation& seen,const rhythm_reference_ports::PreparedPcmRhythmSummary& oracle,bool multi) {
    for(std::size_t i=0;i<(multi?5u:1u);++i){
        compare(seen.curves[i],oracle.tracker().onset_curve(i),2e-4f,"whole-input ODF",i);
        compare(seen.candidates[i],oracle.tracker().candidate_times(i),1e-6f,"whole-input candidate",i);
    }
    if(multi){
        compare(seen.ticks,oracle.tracker().ticks(),1e-6f,"whole-input selection");
        compare(seen.scores,oracle.tracker().method_scores(),1e-6f,"whole-input agreement scores");
    }
    compare(seen.intervals,oracle.summary().intervals(),1e-6f,"whole-input intervals");
    compare(seen.estimates,oracle.summary().estimates(),1e-5f,"whole-input BPM observations");
    compare_scalar(seen.bpm,oracle.summary().bpm(),1e-3f,"whole-input BPM");
    compare_scalar(seen.confidence,oracle.summary().confidence(),1e-4f,"whole-input confidence");
}
}
