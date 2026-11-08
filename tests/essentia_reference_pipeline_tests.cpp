// SPDX-License-Identifier: AGPL-3.0-or-later
#include "essentia_reference_export.h"
#include "essentia_reference_admission.h"
#include "essentia_reference_finite.h"
#include <new>

namespace essentia_reference {std::size_t allocations=0,numerical_failures=0;}

namespace {
using namespace essentia_reference;
struct FourierPowerCapture {
    float expected=0;std::size_t calls=0;
    static void output(void* context,const NumericPortFrameView& event) {
        if(event.identity.node!=313 || event.identity.port || event.status==Status::unavailable)return;
        auto& self=*static_cast<FourierPowerCapture*>(context);
        check(event.status==Status::ok && event.frame.stamp.available,"fresh Fourier power publication");
        const auto& value=std::get<NumericView>(event.frame.payload);++self.calls;
        for(std::size_t row=0;row<value.actual[0];++row)for(std::size_t column=0;column<value.actual[1];++column){
            float actual=0;check(load_element(value,{0,row,column},actual)==Status::ok,"read Fourier power");
            check(std::abs(actual-self.expected)<1e-5f,"Fourier power uses both operands from the current batch");
        }
    }
};
NumericProcessingGraph fourier_power_graph() {
    auto result=numeric_essentia_reference_example().graph;result.graph_id="essentia-repeated-fourier-power";
    std::erase_if(result.nodes,[](const auto& node){return node.id<309 || node.id>313;});
    std::erase_if(result.edges,[](const auto& edge){return edge.from<309 || edge.from>313 || edge.to<310 || edge.to>313;});
    auto& source=result.nodes.front();check(source.id==309,"explicit padded Fourier input");
    source.kind="NetworkInput";source.inputs.clear();
    source.parameters=EndpointProcessingParameters{{{"sample_rate",NumericLiteral{std::uint64_t{44100}}}}};
    return result;
}
void explicit_padding_contracts(const NumericProcessingGraph& graph) {
    const auto original=std::find_if(graph.nodes.begin(),graph.nodes.end(),[](const auto& n){return n.id==300;});
    const auto padded=std::find_if(graph.nodes.begin(),graph.nodes.end(),[](const auto& n){return n.id==301;});
    const auto cropped=std::find_if(graph.nodes.begin(),graph.nodes.end(),[](const auto& n){return n.id==303;});
    check(original!=graph.nodes.end() && padded!=graph.nodes.end() && cropped!=graph.nodes.end(),"explicit band threshold route exists");
    const auto before=std::get<NumericContract>(original->outputs[0].contract.view());
    const auto after=std::get<NumericContract>(padded->outputs[0].contract.view());
    const auto restored=std::get<NumericContract>(cropped->outputs[0].contract.view());
    check(after.shape.extents[0].maximum==before.shape.extents[0].maximum+16,"range padding retains the complete original array");
    check(restored.shape.extents==before.shape.extents,"FIR crop restores original extents for explicit subtraction");
}
void explicit_coefficient_contracts(const NumericProcessingGraph& graph) {
    for(const auto& node:graph.nodes){
        if(node.kind!="ArrayFill")continue;
        const auto& parameters=std::get<TransformProcessingParameters>(node.parameters).values;
        const auto values=std::find_if(parameters.begin(),parameters.end(),[](const auto& value){return value.key=="values";});
        check(values!=parameters.end(),"source coefficient values are explicit");
        const auto count=std::get<NumericArray>(values->value).values.size();
        const auto contract=std::get<NumericContract>(node.outputs[0].contract.view());
        if(count)check(contract.shape.extents[0]==ExtentBounds{count,count},"coefficient contract uses the declared array length");
        check(std::any_of(graph.edges.begin(),graph.edges.end(),[&](const auto& edge){return edge.to==node.id && !edge.input_port;}),
            "each coefficient fill has an explicit size and publication dependency");
    }
}
void explicit_mel_contracts(const NumericProcessingGraph& graph) {
    std::size_t total=0;
    for(const auto& node:graph.nodes){
        if(node.id!=280 && node.id!=285)continue;
        const auto& parameters=std::get<TransformProcessingParameters>(node.parameters).values;
        const auto weights=std::find_if(parameters.begin(),parameters.end(),[](const auto& value){return value.key=="weights";});
        check(weights!=parameters.end(),"Mel projection coefficients are explicit");
        const auto count=std::get<NumericArray>(weights->value).values.size();
        check(count<=max_numeric_array_values,"each Mel coefficient array fits the public parameter bound");total+=count;
    }
    check(total==40*1025,"all forty Mel rows retain all source coefficients");
}
void repeated_fourier_power() {
    const auto graph=fourier_power_graph();auto p=profile();GraphExecutor executor{p};
    application::PreparedExecution prepared;
    check(executor.prepare_numeric(graph,{p.admission,p.resources},prepared)==Status::ok,"prepare actual Essentia Fourier power route");
    check(executor.apply(std::move(prepared))==Status::ok,"apply repeated Fourier power route");
    const auto contract=std::get<NumericContract>(graph.nodes.front().outputs[0].contract.view());
    const auto rows=std::size_t(contract.shape.extents[0].maximum),columns=std::size_t(contract.shape.extents[1].maximum);
    std::vector<float> input(rows*columns);
    const std::array<PlaneLayout,1> planes{{{0,{columns*sizeof(float),sizeof(float)}}}};
    for(std::uint64_t batch=1;batch<=2;++batch){
        const float amplitude=batch==1?.5f:2.f;std::fill(input.begin(),input.end(),0.f);
        for(std::size_t row=0;row<rows;++row)input[row*columns+1]=amplitude;
        FrameStamp stamp;stamp.available=true;stamp.sequence=stamp.source_sequence=batch;stamp.timestamp_ticks=std::int64_t(batch);
        stamp.producer_epoch=stamp.generation=stamp.epoch=1;
        const DataFrameView frame{stamp,NumericView{&contract,std::as_bytes(std::span(input)),planes,{rows,columns}}};
        FourierPowerCapture capture{amplitude*amplitude};
        check(executor.process_numeric_ports(309,frame,{}, {FourierPowerCapture::output,nullptr,nullptr,&capture})==Status::ok,"execute different sequential Fourier batches");
        check(capture.calls==1,"one power spectrum per batch after both squared components update");
    }
}
void session(EssentiaReferenceMethod method,bool silence,const char* output) {
    const auto input=signal(silence);const auto g=graph(method);auto p=profile();GraphExecutor executor{p};
    application::PreparedExecution prepared;
    const auto prepared_status=executor.prepare_numeric(g,{p.admission,p.resources},prepared);
    preparation_failure(g,p,prepared_status);check(prepared_status==Status::ok,"prepare reference graph");
    check(executor.apply(std::move(prepared))==Status::ok,"apply reference graph");
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
        check(!observed.curves[i].empty(),"explicit candidate has a complete ODF");
        check(std::all_of(observed.curves[i].begin(),observed.curves[i].end(),[](float value){return std::isfinite(value) && value>=0;}),"nonnegative finite ODF");
        check(std::is_sorted(observed.candidates[i].begin(),observed.candidates[i].end()),"ordered candidate times");
    }
    if(count==5){
        check(observed.selected<5,"selected method belongs to candidate set");
        compare(observed.ticks,observed.candidates[observed.selected],0,"agreement publishes a complete input candidate");
    }
    Observation repeat;std::srand(17);push(executor,g,input,repeat,2);
    check(!repeat.errors,"repeat session retains no post-publication errors");require_publications(repeat,method);
    compare(repeat.candidates[0],observed.candidates[0],0,"independent batches reset row histories");
}
void admission() {
    const auto multi=numeric_essentia_reference_example();
    explicit_padding_contracts(multi.graph);
    explicit_coefficient_contracts(multi.graph);
    explicit_mel_contracts(multi.graph);
    check(std::string_view(multi.id)=="essentia-reference-multifeature","named multifeature preset");
    check(std::count_if(multi.graph.nodes.begin(),multi.graph.nodes.end(),[](const auto& n){
        return n.kind=="EssentiaCandidateInputs";
    })==1,"explicit current-session ODF synchronization before independent candidates");
    const auto degara=numeric_essentia_reference_example(EssentiaReferenceMethod::Degara);
    check(std::string_view(degara.id)=="essentia-reference-degara","named Degara preset");
    check(std::none_of(multi.graph.nodes.begin(),multi.graph.nodes.end(),[](const auto& n){
        return n.kind=="PcmBeat" || n.kind=="PcmRhythmSummary" || n.kind=="MultiFeatureBeatTracker" ||
            n.kind=="BandBeatEmphasis" || n.kind=="PeriodBeatTracker";
    }),"graph exposes frontend, individual candidates, agreement and summary");
    for(const auto id:std::array<NodeId,5>{283,284,315,316,317}){
        const auto node=std::find_if(multi.graph.nodes.begin(),multi.graph.nodes.end(),[=](const auto& n){return n.id==id;});
        check(node!=multi.graph.nodes.end(),"explicit source arithmetic node exists");
        for(const auto& output:node->outputs)
            check(std::get<NumericContract>(output.contract.view()).dtype==DType::Float32,"Essentia constants and products preserve source Float32 arithmetic");
    }
}
void finite_session(EssentiaReferenceMethod method,bool silence,const char* output) {
    const bool multi=method==EssentiaReferenceMethod::MultiFeature;
    const auto input=signal(silence,finite_samples);const auto g=finite_graph(method);auto p=profile();GraphExecutor executor{p};
    application::PreparedExecution prepared;
    const auto status=executor.prepare_numeric(g,{p.admission,p.resources},prepared);
    preparation_failure(g,p,status);check(status==Status::ok,"prepare complete finite graph");
    check(executor.apply(std::move(prepared))==Status::ok,"apply complete finite graph");
    // Physical capacity follows the declared contract; only the first N samples are data.
    auto backing=input;backing.resize(finite_capacity);
    Observation seen;std::srand(17);const auto before=allocations;push(executor,g,input,seen,1,backing);
    check(allocations==before,"finite batch has no hot allocation");seen.diagnostics();
    check(!seen.errors,"whole-input required endpoints available");require_publications(seen,method);
    for(PortId port=0;port<4;++port)require_publication(seen,61,port);
    if(multi)for(PortId port=0;port<5;++port)require_publication(seen,60,port);
    check(seen.candidate_count==(multi?5u:1u),"one execution of each whole-input tracker");
    if(!silence)check(!seen.candidates[0].empty(),"changing-tone positive control produces candidates");
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
    repeated_fourier_power();
    admission();
    fixed_source_admission();retired_compositions_have_no_dispatch();
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
