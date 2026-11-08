// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include "essentia_reference_fixture.h"
#include <ooura_backend.h>
#include <dataflow_runtime/numeric_operation_binding.h>
namespace essentia_reference {
inline OwnedProcessingContract spectral_contract(std::uint32_t fs,double frequency_step=44100./2048) {
    static constexpr std::uint32_t mono=0;
    NumericContract c;c.dtype=DType::Float32;c.channel_order={&mono,1};c.semantic=Semantic::Numeric;
    c.unit=NumericUnit::RawAmplitude;c.shape={Rank::Matrix,{Axis::Time,Axis::Bin},{{{0,4},{1025,1025}}}};
    c.clock={"source",{1,fs?fs:44100},fs};
    c.coordinates[1]={CoordinateMode::Regular,NumericUnit::Hertz,{0,1},{std::int64_t(frequency_step*2048),2048}};
    OwnedProcessingContract owned;
    check(own_processing_contract(c,profile().admission.frame,65536,owned)==Status::ok,"own declared source frequency grid");
    return owned;
}
inline Status compose_spectral(std::string_view kind,const OwnedProcessingContract& c,ConfiguredProcessingNode& out) {
    const std::array inputs{c};TransformProcessingParameters parameters;
    parameters.values={{"method",NumericLiteral{std::uint64_t{5}}}};
    return rhythm_graph_adapters::make_essentia_operation_node(10,kind,inputs,std::move(parameters),
        profile().admission.frame,65536,out);
}
inline void reject_declared_frequency(const ConfiguredProcessingNode& node) {
    const ResolvedProcessingNode resolved{node.id,node.kind,NodeRole::Processor,ProcessingOperation::NumericExtension,
        node.inputs,node.outputs,&node.parameters,node.enabled};
    const auto p=profile();
    check(p.operation_factory->assess(numeric_operation_node_view(resolved),numeric_operation_profile(p)).supported,
        "numerical assessment sees identical storage geometry");
    ConfiguredProcessingNode source;source.id=1;source.kind="NetworkInput";
    source.parameters=EndpointProcessingParameters{{{"sample_rate",NumericLiteral{std::uint64_t{48000}}}}};
    source.outputs={{0,PortDirection::Output,node.inputs[0].contract}};
    NumericProcessingGraph graph;graph.graph_id="essentia-declared-frequency";
    graph.nodes={source,node};graph.edges={{1,node.id,0,0}};
    std::shared_ptr<const PreparedGraph> plan;std::unique_ptr<RuntimeContext> state;
    check(PreparedGraph::prepare_numeric(graph,p,plan,state)!=Status::ok && !plan && !state,
        "runtime rejects hand-edited incompatible declared frequency atomically");
}
inline void fixed_source_admission() {
    for(const auto kind:std::array<std::string_view,3>{"EssentiaOnsetRows","EssentiaInformationRows","EssentiaErbRows"}){
        ConfiguredProcessingNode good;
        check(compose_spectral(kind,spectral_contract(44100),good)==Status::ok,"canonical physical frequency grid admitted");
        ConfiguredProcessingNode wrong_rate,wrong_grid;
        check(compose_spectral(kind,spectral_contract(48000),wrong_rate)==Status::ok,"numerical factory sees matching buffer geometry");
        reject_declared_frequency(wrong_rate);
        check(compose_spectral(kind,spectral_contract(0,48000./2048),wrong_grid)==Status::ok,"numerical factory does not inspect coordinate metadata");
        reject_declared_frequency(wrong_grid);
    }
}
inline void retired_compositions_have_no_dispatch() {
    constexpr std::array<std::string_view,26> retired{"PcmBeat","PcmRhythmSummary","GlobalOnset","PeriodBeatTracker",
        "MultiFeatureBeatTracker","BandBeatEmphasis","LegacyRhythm","PulseTempo","NoveltyTempoHistogram",
        "BandRhythmTransform","BeatSegmentEnergy","BeatLoudnessSequence","LoopPcmTempo","CorrelationMeter",
        "ActivityFluctuation","PulseCorrelation","FixedTempoCandidates","CombPeriodPhase","FeaturePeriodTracker",
        "MovingThreshold","PulseOnset","BandNovelty","FourierTempoHistogram","PhaseTempoPulse","OnsetRate","TriangularSpectralBands"};
    const auto factory=profile().operation_factory;
    for(const auto kind:retired){
        NumericOperationNodeView node;node.id=1;node.kind=kind;
        std::shared_ptr<const NumericOperationDescriptor> descriptor;
        check(factory->describe(node,descriptor)==Status::unavailable && !descriptor,"retired composition has no runtime descriptor");
    }
}
inline void builtin_fourier(NumericProcessingGraph& graph) {
    for(auto& node:graph.nodes)if(node.kind=="Fourier" || node.kind=="RealFourier")
        for(auto& setting:std::get<TransformProcessingParameters>(node.parameters).values)
            if(setting.key=="backend_id")setting.value=std::string{};
}
inline void fourier_provider_admission() {
    auto input=graph(EssentiaReferenceMethod::Degara);auto p=profile();
    std::shared_ptr<const PreparedGraph> plan;std::unique_ptr<RuntimeContext> state;
    p.fourier_backend.reset();
    check(PreparedGraph::prepare_numeric(input,p,plan,state)!=Status::ok && !plan && !state,"named Essentia FFT requires provider before publication");
    p.fourier_backend={&desktop_fourier::ooura_backend_factory(),[](const auto*){}};
    check(PreparedGraph::prepare_numeric(input,p,plan,state)!=Status::ok && !plan && !state,"wrong FFT provider rejects without fallback");
    builtin_fourier(input);p.fourier_backend.reset();
    check(PreparedGraph::prepare_numeric(input,p,plan,state)==Status::ok,"explicit empty selection retains builtin without provider");
}
}
