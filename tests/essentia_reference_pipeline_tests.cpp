// SPDX-License-Identifier: AGPL-3.0-or-later
#include "essentia_reference_export.h"
#include "essentia_reference_admission.h"
#include "essentia_reference_fourier_diagnostic.h"
#include "essentia_reference_finite.h"
#include <new>

namespace essentia_reference {std::size_t allocations=0,numerical_failures=0;}

namespace {
using namespace essentia_reference;
void session(EssentiaReferenceMethod method,bool silence,const char* output) {
    const auto input=signal(silence);const auto g=graph(method);auto p=profile();GraphExecutor executor{p};
    application::PreparedExecution prepared;
    const auto prepared_status=executor.prepare_numeric(g,{p.admission,p.resources},prepared);
    preparation_failure(g,p,prepared_status);check(prepared_status==Status::ok,"prepare reference graph");
    check(executor.apply(std::move(prepared))==Status::ok,"apply reference graph");
    rhythm_reference_ports::PcmBeatRecipeConfig config;
    config.method=method==EssentiaReferenceMethod::MultiFeature?rhythm_reference_ports::PcmBeatMethod::MultiFeature:
        rhythm_reference_ports::PcmBeatMethod::SingleComplex;
    config.window_precision=dsp_kernels::NumericalPrecision::DesktopReference;
    rhythm_reference_ports::PreparedPcmRhythmSummary oracle;
    check(oracle.prepare(input.size(),config,&desktop_fourier::kiss_backend_factory()),"prepare independent source composition");
    std::srand(17);check(oracle.process(input),"source composition processes session");
    Observation observed;std::srand(17);const auto before=allocations;push(executor,g,input,observed);
    check(allocations==before,"no hot C++ allocation including intermediate delivery");
    observed.diagnostics();
    export_case(output,method,silence,input,observed);
    check(observed.errors==0,"all subscribed stages available");
    require_publications(observed,method);
    std::fprintf(stderr,"Essentia case method=%s signal=%s boundary_markers=%zu\n",
        method==EssentiaReferenceMethod::MultiFeature?"multifeature":"degara",silence?"silence":"tones8",observed.boundary_markers);
    for(std::size_t port=0;port<4;++port)check(observed.available[61][port],"all summary outputs actually published");
    const auto count=method==EssentiaReferenceMethod::MultiFeature?5u:1u;
    check(observed.candidate_count==count,"all independent candidate trackers executed");
    for(std::size_t i=0;i<count;++i){
        check(observed.candidate_order[i]==NodeId(50+i),"candidate shared RNG draws follow source order");
        compare(observed.curves[i],oracle.tracker().onset_curve(i),2e-4f,"ODF stage parity",i);
        compare(observed.candidates[i],oracle.tracker().candidate_times(i),1e-6f,"candidate tick parity",i);
    }
    if(count==5)compare(observed.ticks,oracle.tracker().ticks(),1e-6f,"agreement selects original complete candidate");
    compare(observed.intervals,oracle.summary().intervals(),1e-6f,"interval parity");
    compare(observed.estimates,oracle.summary().estimates(),1e-5f,"BPM observation parity");
    compare_scalar(observed.bpm,oracle.summary().bpm(),1e-3f,"final BPM parity");
    compare_scalar(observed.confidence,oracle.summary().confidence(),1e-4f,"raw confidence parity");
    Observation repeat;std::srand(17);push(executor,g,input,repeat,2);
    check(!repeat.errors,"repeat session retains no post-publication errors");require_publications(repeat,method);
    compare(repeat.candidates[0],observed.candidates[0],0,"independent batches reset row histories");
}
void admission() {
    const auto multi=numeric_essentia_reference_example();
    check(std::string_view(multi.id)=="essentia-reference-multifeature","named multifeature preset");
    check(std::count_if(multi.graph.nodes.begin(),multi.graph.nodes.end(),[](const auto& n){
        return n.kind=="EssentiaCandidateInputs";
    })==1,"explicit current-session ODF synchronization before independent candidates");
    const auto degara=numeric_essentia_reference_example(EssentiaReferenceMethod::Degara);
    check(std::string_view(degara.id)=="essentia-reference-degara","named Degara preset");
    check(std::none_of(multi.graph.nodes.begin(),multi.graph.nodes.end(),[](const auto& n){
        return n.kind=="PcmBeat" || n.kind=="PcmRhythmSummary" || n.kind=="MultiFeatureBeatTracker";
    }),"graph exposes frontend, individual candidates, agreement and summary");
}
void finite_session(EssentiaReferenceMethod method,bool silence,const char* output) {
    const bool multi=method==EssentiaReferenceMethod::MultiFeature;
    const auto input=signal(silence,finite_samples);const auto g=finite_graph(method);auto p=profile();GraphExecutor executor{p};
    application::PreparedExecution prepared;
    const auto status=executor.prepare_numeric(g,{p.admission,p.resources},prepared);
    preparation_failure(g,p,status);check(status==Status::ok,"prepare complete finite graph");
    check(executor.apply(std::move(prepared))==Status::ok,"apply complete finite graph");
    rhythm_reference_ports::PcmBeatRecipeConfig config;
    config.method=multi?rhythm_reference_ports::PcmBeatMethod::MultiFeature:rhythm_reference_ports::PcmBeatMethod::SingleComplex;
    config.window_precision=dsp_kernels::NumericalPrecision::DesktopReference;
    rhythm_reference_ports::PreparedPcmRhythmSummary oracle;
    check(oracle.prepare(finite_capacity,config,&desktop_fourier::kiss_backend_factory()),"prepare whole-input oracle capacity");
    std::srand(17);check(oracle.process(input),"independent recipe processes all real PCM");
    // Physical capacity follows the declared contract; only the first N samples are data.
    auto backing=input;backing.resize(finite_capacity);
    Observation seen;std::srand(17);const auto before=allocations;push(executor,g,input,seen,1,backing);
    check(allocations==before,"finite batch has no hot allocation");seen.diagnostics();
    check(!seen.errors,"whole-input required endpoints available");require_publications(seen,method);
    for(PortId port=0;port<4;++port)require_publication(seen,61,port);
    if(multi)for(PortId port=0;port<5;++port)require_publication(seen,60,port);
    check(seen.candidate_count==(multi?5u:1u),"one execution of each whole-input tracker");
    if(!silence)check(!seen.candidates[0].empty(),"changing-tone positive control produces candidates");
    compare_finite(seen,oracle,multi);
    export_case(output,method,silence,input,seen,multi?(silence?"finite-multi-silence":"finite-multi-tones8"):"finite-single-tones8");
}
}
void* operator new(std::size_t bytes) {
    ++essentia_reference::allocations;
    if(auto* value=std::malloc(bytes?bytes:1))return value;
    throw std::bad_alloc();
}
void operator delete(void* value) noexcept {std::free(value);}
void operator delete(void* value,std::size_t) noexcept {std::free(value);}
int main(int argc,char** argv) {
    if(argc>1 && std::string_view(argv[1])=="--fourier-diagnostic")return fourier_diagnostic();
    admission();
    fixed_source_admission();
    fourier_provider_admission();
    finite_admission();
    const char* output=argc>1?argv[1]:nullptr;
    session(EssentiaReferenceMethod::Degara,false,output);
    session(EssentiaReferenceMethod::MultiFeature,false,output);
    session(EssentiaReferenceMethod::MultiFeature,true,output);
    finite_session(EssentiaReferenceMethod::Degara,false,output);
    finite_session(EssentiaReferenceMethod::MultiFeature,false,output);
    finite_session(EssentiaReferenceMethod::MultiFeature,true,output);
    if(numerical_failures){std::fprintf(stderr,"Essentia reference strict numerical gates failed=%zu\n",numerical_failures);return 1;}
    std::puts("Essentia reference pipeline PASS");return 0;
}
