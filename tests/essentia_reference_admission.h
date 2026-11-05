// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include "essentia_reference_fixture.h"
#include <ooura_backend.h>
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
    parameters.values={{"method",NumericLiteral{std::uint64_t{4}}}};
    return rhythm_graph_adapters::make_essentia_operation_node(10,kind,inputs,std::move(parameters),
        profile().admission.frame,65536,out);
}
inline void fixed_source_admission() {
    for(const auto kind:std::array<std::string_view,3>{"EssentiaOnsetRows","EssentiaInformationRows","EssentiaErbRows"}){
        ConfiguredProcessingNode good;
        check(compose_spectral(kind,spectral_contract(44100),good)==Status::ok,"canonical physical frequency grid admitted");
        ConfiguredProcessingNode rejected;
        check(compose_spectral(kind,spectral_contract(48000),rejected)!=Status::ok,"wrong known PCM Fs rejected before source bank selection");
        check(compose_spectral(kind,spectral_contract(0,48000./2048),rejected)!=Status::ok,"wrong declared Hz step rejected even with unknown PCM clock");
        good.inputs[0].contract=spectral_contract(48000);
        const ResolvedProcessingNode resolved{good.id,good.kind,NodeRole::Processor,ProcessingOperation::NumericExtension,
            good.inputs,good.outputs,&good.parameters,good.enabled};
        auto p=profile();check(!p.operation_factory->assess(resolved,p).supported,"assessment rejects hand-edited incompatible physical declaration");
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
