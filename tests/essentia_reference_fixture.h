// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include <core_app/numeric_essentia_reference_graph.h>
#include <dataflow_runtime/runtime.h>
#include <rhythm_graph_adapters/essentia_composition.h>
#include <rhythm_graph_adapters/profile.h>
#include <rhythm_reference_ports/pcm_rhythm_summary.h>
#include <kiss_backend.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <vector>

namespace essentia_reference {
using namespace domain;using namespace signal_contracts;using namespace dataflow_runtime;
extern std::size_t allocations;
extern std::size_t numerical_failures;
inline void check(bool value,const char* message) {
    if(!value){std::fprintf(stderr,"FAIL Essentia reference: %s\n",message);std::exit(1);}
}
inline NumericRuntimeProfile profile() {
    constexpr FrameLimits frames{8,2097152,8388608,65536,4096,65536};
    NumericRuntimeProfile p{{128,256,16,64,1048576,16777216,frames},
        {{33554432,268435456,134217728,1048576},0,536870912},
        {2097152,8,8388608,8388608,16777216},{true,true,true}};
    p.fourier_backend={&desktop_fourier::kiss_backend_factory(),[](const auto*){}};
    p.music_enabled=true;return rhythm_graph_adapters::with_reference_operations(std::move(p));
}
inline void preparation_failure(const NumericProcessingGraph& g,const NumericRuntimeProfile& p,Status status) {
    if(status==Status::ok)return;
    std::vector<std::size_t> workspace(g.nodes.size()*2);ProcessingGraphAdmission admitted;
    const auto capabilities=PreparedGraph::numeric_capabilities(p);
    const auto d=admit_processing_graph(g,p.admission,capabilities,workspace,admitted);
    std::fprintf(stderr,"Essentia prepare status=%u admission=%u node=%d port=%u reason=%u peak=%zu\n",
        unsigned(status),unsigned(d.error),int(d.node),unsigned(d.port),unsigned(d.reason),admitted.peak_bytes);
    for(const auto& node:g.nodes){
        ResolvedProcessingNode resolved;
        const auto diagnostic=resolve_processing_node(node,p.admission,resolved);
        if(!diagnostic || !capabilities.query(capabilities.context,resolved,capabilities.profile).supported)
            std::fprintf(stderr,"Essentia unsupported node=%d kind=%s domain_error=%u\n",int(node.id),node.kind.c_str(),unsigned(diagnostic.error));
    }
}
inline NumericProcessingGraph graph(EssentiaReferenceMethod method) {
    auto result=numeric_essentia_reference_example(method).graph;
    std::erase_if(result.nodes,[](const auto& node){const auto k=node_kind(node.kind);return node.id<3 || (k && is_view(*k));});
    std::erase_if(result.edges,[&](const auto& edge){
        return std::none_of(result.nodes.begin(),result.nodes.end(),[&](const auto& n){return n.id==edge.from;}) ||
            std::none_of(result.nodes.begin(),result.nodes.end(),[&](const auto& n){return n.id==edge.to;});
    });
    auto& source=result.nodes[0];check(source.id==3,"complete session ingress");
    source.kind="NetworkInput";source.inputs.clear();
    source.parameters=EndpointProcessingParameters{{{"sample_rate",NumericLiteral{std::uint64_t{44100}}}}};
    return result;
}
inline std::vector<float> signal(bool silence=false,std::size_t samples=352800) {
    std::vector<float> values(samples);if(silence)return values;
    constexpr std::array frequencies{110.,251.,503.,997.,1999.,4001.,6007.,10007.};
    for(std::size_t i=0;i<values.size();++i){
        const auto period=std::size_t(22050);const double t=double(i)/44100.;
        const double envelope=std::exp(-double(i%period)/(44100.*.006));
        for(std::size_t k=0;k<frequencies.size();++k)
            values[i]+=float(.07*envelope*std::sin(6.283185307179586*frequencies[k]*t+.17*double(k)));
    }
    return values;
}
struct Observation {
    void* diagnostic_context=nullptr;
    void (*diagnostic)(void*,const NumericPortFrameView&)=nullptr;
    struct Counts {std::size_t ok=0,unavailable=0,error=0;Status last=Status::unavailable;};
    std::array<std::array<Counts,6>,64> stages{};
    std::array<std::vector<float>,5> curves,candidates;
    std::vector<float> ticks,intervals,estimates,scores;
    std::array<float,25> pairwise{};std::uint64_t selected=0;
    float bpm=0,confidence=0;std::size_t errors=0;
    std::array<NodeId,5> candidate_order{};std::size_t candidate_count=0;
    std::array<std::array<bool,6>,64> available{};std::size_t boundary_markers=0;
    Observation() {
        for(auto& v:curves)v.reserve(1024);for(auto& v:candidates)v.reserve(1024);
        ticks.reserve(1024);intervals.reserve(1024);estimates.reserve(1024);scores.reserve(5);
    }
    static void vector(const NumericView& v,std::vector<float>& values) {
        check(v.actual[0]<=values.capacity(),"capture preallocated capacity");values.resize(v.actual[0]);
        for(std::size_t i=0;i<values.size();++i)
            check(load_element(v,{0,i,0},values[i])==Status::ok,"read numerical stage");
    }
    static void times(NodeId id,const NumericView& v) {
        const double rate=id==46?44100./512:44100./1024;
        check(v.contract->dtype==DType::Float64,"time grid stays separate from Float32 analytics");
        check(v.contract->unit==NumericUnit::Seconds,"time payload explicitly declares seconds");
        for(std::size_t i=0;i<v.actual[0];++i){
            double actual=0;check(load_element(v,{0,i,0},actual)==Status::ok,"read explicit time coordinate");
            check(actual==double(i)/rate,"time coordinate follows explicit source hop and local session origin");
        }
    }
    void onset(NodeId id,PortId port,const NumericView& v) {
        constexpr std::array<NodeId,5> odfs{40,41,42,45,46};
        const auto found=std::find(odfs.begin(),odfs.end(),id);if(found==odfs.end())return;
        if(!port)vector(v,curves[std::size_t(found-odfs.begin())]);
        if(port==1 && id!=45)times(id,v);
    }
    void summary(PortId port,const NumericView& v) {
        if(!port)check(load_element(v,{0,0,0},bpm)==Status::ok,"read BPM");
        if(port==1)check(load_element(v,{0,0,0},confidence)==Status::ok,"read confidence");
        if(port==2)vector(v,intervals);
        if(port==3)vector(v,estimates);
    }
    void capture(NodeId id,PortId port,const NumericView& v) {
        onset(id,port,v);
        if(id>=50 && id<=54 && port==1)candidate(id,v);
        if(id==60)agreement(port,v);
        if(id==61)summary(port,v);
    }
    void agreement(PortId port,const NumericView& v) {
        if(!port)vector(v,ticks);
        if(port==2)check(load_element(v,{0,0,0},selected)==Status::ok,"read selected method");
        if(port==3)vector(v,scores);
        if(port==4){
            check(v.actual==std::array<std::size_t,2>{5,5},"pairwise agreement shape");
            for(std::size_t i=0;i<5;++i)for(std::size_t j=0;j<5;++j)
                check(load_element(v,{0,i,j},pairwise[i*5+j])==Status::ok,"read pairwise agreement");
        }
    }
    void candidate(NodeId id,const NumericView& v) {
        check(candidate_count<candidate_order.size(),"one candidate per independent batch");
        candidate_order[candidate_count++]=id;vector(v,candidates[std::size_t(id-50)]);
    }
    static void output(void* context,const NumericPortFrameView& event) {
        auto& self=*static_cast<Observation*>(context);
        self.observe_event(event);
        const auto id=event.identity.node;const auto port=event.identity.port;
        const bool watched=((id==40 || id==41 || id==42 || id==45 || id==46) && !port) ||
            ((id==40 || id==41 || id==42 || id==46) && port==1) ||
            (id>=50 && id<=54 && port==1) || (id==60 && port<5) || (id==61 && port<4);
        if(!watched)return;
        if(event.status==Status::unavailable && !event.frame.stamp.available && !self.available[std::size_t(id)][port]){
            ++self.boundary_markers;return;
        }
        if(event.status!=Status::ok || !event.frame.stamp.available){self.failure(event);return;}
        self.available[std::size_t(id)][port]=true;
        self.capture(event.identity.node,event.identity.port,std::get<NumericView>(event.frame.payload));
    }
    void observe_event(const NumericPortFrameView& event) {
        tally(event);
        if(diagnostic)diagnostic(diagnostic_context,event);
    }
    void tally(const NumericPortFrameView& event) {
        const auto id=std::size_t(event.identity.node),port=std::size_t(event.identity.port);
        if(id>=stages.size() || port>=stages[id].size())return;
        auto& count=stages[id][port];count.last=event.status;
        if(event.status==Status::ok && event.frame.stamp.available)++count.ok;
        else if(event.status==Status::unavailable && !event.frame.stamp.available)++count.unavailable;
        else ++count.error;
    }
    void diagnostics() const {
        if(!errors && std::all_of(available[61].begin(),available[61].begin()+4,[](bool value){return value;}))return;
        for(std::size_t id=0;id<stages.size();++id)for(std::size_t port=0;port<stages[id].size();++port){
            const auto& count=stages[id][port];
            if(count.ok || count.unavailable || count.error)
                std::fprintf(stderr,"Essentia stage node=%zu port=%zu ok=%zu unavailable=%zu error=%zu last=%u\n",
                    id,port,count.ok,count.unavailable,count.error,unsigned(count.last));
        }
    }
    void failure(const NumericPortFrameView& event) {
        if(errors<12){
            const auto* v=std::get_if<NumericView>(&event.frame.payload);const auto& stamp=event.frame.stamp;
            std::fprintf(stderr,"Essentia event node=%d port=%u status=%u available=%d actual=%zux%zu source_seq=%llu gen=%llu epoch=%llu interval=%d begin=%lld count=%llu\n",
                int(event.identity.node),unsigned(event.identity.port),unsigned(event.status),int(stamp.available),
                v?v->actual[0]:0,v?v->actual[1]:0,static_cast<unsigned long long>(stamp.source_sequence),
                static_cast<unsigned long long>(stamp.generation),static_cast<unsigned long long>(stamp.epoch),
                int(stamp.has_sample_interval),static_cast<long long>(stamp.sample_begin),static_cast<unsigned long long>(stamp.sample_count));
        }
        ++errors;
    }
};
inline void require_publication(const Observation& seen,NodeId id,PortId port) {
    const auto count=seen.stages[std::size_t(id)][port].ok;
    if(count!=1)std::fprintf(stderr,"Essentia required publication node=%d port=%u successes=%zu expected=1\n",int(id),unsigned(port),count);
    check(count==1,"each required output publishes exactly once per complete session");
}
inline void require_candidate_publications(const Observation& seen,bool multi) {
    constexpr std::array<NodeId,5> odfs{40,41,42,45,46};
    for(std::size_t i=0;i<(multi?5u:1u);++i){
        require_publication(seen,odfs[i],0);require_publication(seen,NodeId(50+i),1);
        if(odfs[i]!=45)require_publication(seen,odfs[i],1);
        if(multi)require_publication(seen,47,PortId(i));
    }
}
inline void require_publications(const Observation& seen,EssentiaReferenceMethod method) {
    const bool multi=method==EssentiaReferenceMethod::MultiFeature;
    require_candidate_publications(seen,multi);
    for(PortId port=0;port<4;++port)require_publication(seen,61,port);
    if(multi){require_publication(seen,60,0);require_publication(seen,60,3);}
}
struct Differences {
    std::size_t count=0,first=0,maximum_index=0;float maximum=0;
    void record(std::size_t index,float actual,float expected,float tolerance) {
        const auto error=std::abs(actual-expected);
        if(error>maximum || !std::isfinite(error)){maximum=error;maximum_index=index;}
        if(error<=tolerance*(1+std::abs(expected)))return;
        if(!count)first=index;++count;
    }
    void report(std::span<const float> actual,std::span<const float> expected,float tolerance,
        const char* message,std::size_t method) const {
        std::fprintf(stderr,"Essentia compare %s method=%zu failures=%zu/%zu first=%zu actual=%.9g expected=%.9g limit=%.9g max_abs=%.9g max_index=%zu\n",
            message,method,count,actual.size(),first,double(actual[first]),double(expected[first]),
            double(tolerance*(1+std::abs(expected[first]))),double(maximum),maximum_index);
    }
};
inline void compare(std::span<const float> actual,std::span<const float> expected,float tolerance,const char* message,std::size_t method=0) {
    if(actual.size()!=expected.size()){
        std::fprintf(stderr,"Essentia compare %s method=%zu actual_count=%zu expected_count=%zu\n",message,method,actual.size(),expected.size());
        ++numerical_failures;return;
    }
    Differences difference;
    for(std::size_t i=0;i<actual.size();++i)difference.record(i,actual[i],expected[i],tolerance);
    if(difference.count){++numerical_failures;difference.report(actual,expected,tolerance,message,method);}
}
inline void compare_scalar(float actual,float expected,float tolerance,const char* message) {
    const auto error=std::abs(actual-expected);if(error<tolerance)return;
    ++numerical_failures;
    std::fprintf(stderr,"Essentia scalar %s actual=%.9g expected=%.9g abs_error=%.9g strict_limit=%.9g\n",
        message,double(actual),double(expected),double(error),double(tolerance));
}
inline void push(GraphExecutor& executor,const NumericProcessingGraph& g,std::span<const float> pcm,Observation& observed,
    std::uint64_t sequence=1,std::span<const float> backing={}) {
    const auto c=std::get<NumericContract>(g.nodes[0].outputs[0].contract.view());
    const std::array<PlaneLayout,1> planes{{{0,{sizeof(float),0}}}};
    FrameStamp stamp;stamp.available=true;stamp.sequence=stamp.source_sequence=sequence;
    stamp.producer_epoch=stamp.generation=stamp.epoch=1;stamp.has_sample_interval=true;stamp.sample_count=pcm.size();
    if(std::any_of(g.nodes.begin(),g.nodes.end(),[](const auto& n){return n.id==59;}))
        check(executor.process_clock(59,stamp,{}, {Observation::output,nullptr,nullptr,&observed})==Status::ok,"publish explicit zero confidence constant");
    const auto storage=backing.empty()?pcm:backing;
    const DataFrameView frame{stamp,NumericView{&c,std::as_bytes(storage),planes,{pcm.size(),1}}};
    const auto status=executor.process_numeric_ports(3,frame,{}, {Observation::output,nullptr,nullptr,&observed});
    if(status!=Status::ok)observed.diagnostics();
    check(status==Status::ok,"execute session graph");
}
}
