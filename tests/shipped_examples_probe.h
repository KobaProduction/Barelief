#pragma once
#include "synthetic_capture.h"
#include <pipeline/numeric_engine.h>
#include <desktop/numeric_project_document.h>
#include <QElapsedTimer>
#include <QCoreApplication>
#include <QJsonDocument>
#include <QThread>
#include <map>
#include <tuple>
#include <mutex>
#include <cmath>
#include <cstdio>

// Test the actual canonical source/executor/owned-delivery path, including
// every DSP branch and graph-view input. Capture remains in-memory.
inline bool measure_shipped_example(const domain::NumericProcessingGraph& graph,const char* name,
                                    pipeline::AudioSourceKind) {
    using namespace pipeline;using namespace signal_contracts;
    using Key=std::tuple<domain::NodeId,domain::PortDirection,domain::PortId>;
    struct Stage {unsigned frames=0;bool positive=false;std::uint64_t first=0,last=0;};
    struct History {unsigned rows=0;std::uint64_t ordinal=0,capacity=0;bool positive=false;};
    std::mutex mutex;std::map<Key,Stage> stages;std::map<domain::NodeId,History> histories;
    bool bad=false;unsigned opens=0;std::map<AudioSourceKind,unsigned> opened,expected_opens;
    for(const auto& node:graph.nodes) {
        if(node.kind=="AudioSource" || node.kind=="TestSignal") {
            auto kind=node.kind=="TestSignal"?AudioSourceKind::TestSignal:AudioSourceKind::SystemOutput;
            if(const auto* params=std::get_if<domain::EndpointProcessingParameters>(&node.parameters))
                for(const auto& p:params->values)if(p.key=="source")if(const auto* literal=std::get_if<NumericLiteral>(&p.value))
                    if(const auto* value=std::get_if<std::int64_t>(literal))kind=AudioSourceKind(*value);
            ++expected_opens[kind];
        }
        // Device outputs and their resamplers can be intentionally unselected,
        // or provide typed silence for a missing physical channel.
        if(node.kind=="AudioSource"||node.kind=="TestSignal"||node.kind=="SampleClock"||node.kind=="PcmResampler")continue;
        if(node.kind=="Spectrogram")histories[node.id].capacity=std::get<domain::HistoryProcessingParameters>(node.parameters).capacity;
        if(node.kind=="SpectrogramView")for(const auto& edge:graph.edges)if(edge.to==node.id){const auto source=std::find_if(graph.nodes.begin(),graph.nodes.end(),[&](const auto& n){return n.id==edge.from;});if(source!=graph.nodes.end()&&source->kind=="Spectrogram")histories[node.id].capacity=std::get<domain::HistoryProcessingParameters>(source->parameters).capacity;}
        for(const auto& port:node.outputs)if(!(node.kind=="Spectrogram"&&port.id==0))stages[{node.id,domain::PortDirection::Output,port.id}]={};
        if(node.outputs.empty())for(const auto& port:node.inputs)if(node.kind!="SpectrogramView")stages[{node.id,domain::PortDirection::Input,port.id}]={};
    }
    const auto factory=test_fixture::synthetic_capture_factory();
    NumericEngine engine([&](auto kind){ {std::lock_guard lock(mutex);++opens;++opened[kind];}return factory(kind); });
    bool replied=false,accepted=false;
    engine.set_reply_handler([&](const auto& reply){replied=true;accepted=reply.accepted;if(!accepted)std::fprintf(stderr,"%s command: %s\n",name,qPrintable(reply.error));});
    auto wait=[](const auto& predicate){QElapsedTimer timer;timer.start();while(timer.elapsed()<5000){QCoreApplication::processEvents();if(predicate())return true;QThread::msleep(2);}return predicate();};
    QByteArray bytes;if(!desktop::encode_numeric_graph(graph,bytes))return false;
    EngineCommand command;command.kind=EngineCommand::ApplyGraph;command.graph=QJsonDocument::fromJson(bytes).object();engine.submit(std::move(command));
    if(!wait([&]{return replied;})||!accepted)return false;
    const auto tap=engine.subscribe_numeric_tap({},[&](const DeliveredNumericFrame& delivered){
        if(!delivered.context||!delivered.frame||!delivered.frame.view().stamp.available)return;
        const auto& endpoint=delivered.context->endpoint;std::lock_guard lock(mutex);
        const auto at=stages.find({endpoint.node,endpoint.direction,endpoint.port});if(at==stages.end())return;
        const auto view=delivered.frame.view();const auto* numeric=std::get_if<NumericView>(&view.payload);if(!numeric){bad=true;return;}
        const auto node=std::find_if(graph.nodes.begin(),graph.nodes.end(),[&](const auto& n){return n.id==endpoint.node;});
        const auto& ports=endpoint.direction==domain::PortDirection::Output?node->outputs:node->inputs;
        const auto port=std::find_if(ports.begin(),ports.end(),[&](const auto& p){return p.id==endpoint.port;});
        if(port==ports.end()||!same_contract(port->contract.view(),*numeric->contract)){bad=true;return;}
        double energy=0;
        for(std::size_t plane=0;plane<numeric->planes.size();++plane)for(std::size_t i=0;i<numeric->actual[0];++i) {
            if(numeric->contract->dtype==DType::Float32){float x=0;if(load_element(*numeric,{plane,i,0},x)!=Status::ok||!std::isfinite(x)){bad=true;return;}energy+=double(x)*x;}
            else if(numeric->contract->dtype==DType::Float64){double x=0;if(load_element(*numeric,{plane,i,0},x)!=Status::ok || !std::isfinite(x)){bad=true;return;}energy+=x*x;}
            else if(numeric->contract->dtype==DType::UInt32){std::uint32_t x=0;if(load_element(*numeric,{plane,i,0},x)!=Status::ok){bad=true;return;}energy+=double(x);}
            else if(numeric->contract->dtype==DType::UInt64){std::uint64_t x=0;if(load_element(*numeric,{plane,i,0},x)!=Status::ok){bad=true;return;}energy+=double(x);}
            else {bad=true;return;}
        }
        auto& state=at->second;if(!state.frames)state.first=view.stamp.sequence;else if(view.stamp.sequence<state.last)bad=true;
        state.last=view.stamp.sequence;++state.frames;state.positive|=energy>1e-12;
    },[&](const auto& row){
        if(row.event!=NumericHistoryEvent::Row||!row.compact.context)return;const auto& endpoint=row.compact.context->endpoint;

        std::lock_guard lock(mutex);const auto at=histories.find(endpoint.node);if(at==histories.end())return;
        const auto capacity=at->second.capacity;
        bad|=row.capacity!=capacity||row.filled_count!=std::min(row.accepted_row_ordinal,capacity)||row.accepted_row_ordinal<=at->second.ordinal;
        at->second.ordinal=row.accepted_row_ordinal;++at->second.rows;
        if(!row.compact.frame){bad=true;return;}const auto view=row.compact.frame.view();const auto* values=std::get_if<NumericView>(&view.payload);if(!values||values->contract->dtype!=DType::Float32){bad=true;return;}
        for(std::size_t i=0;i<values->actual[0];++i){float value=0;if(load_element(*values,{0,i,0},value)!=Status::ok||!std::isfinite(value)){bad=true;return;}at->second.positive|=std::abs(value)>1e-12f;}
    });
    if(!tap)return false;
    const auto ready=[&]{if(bad)return true;for(const auto& [key,s]:stages){const auto id=std::get<0>(key);const auto node=std::find_if(graph.nodes.begin(),graph.nodes.end(),[&](const auto& n){return n.id==id;});const bool agreement=node->kind=="MusicalSpectrogram" && (std::get<2>(key)==1 || std::get<2>(key)==2);
        const bool scalar_agreement=node->kind=="ScalarView" && std::any_of(graph.edges.begin(),graph.edges.end(),[&](const auto& e){if(e.to!=id)return false;const auto parent=std::find_if(graph.nodes.begin(),graph.nodes.end(),[&](const auto& n){return n.id==e.from;});return parent!=graph.nodes.end() && parent->kind=="MusicalSpectrogram";});
        const bool signed_metric=agreement||scalar_agreement||node->kind=="ChannelComparison"||id==221||((node->kind=="Fourier" || node->kind=="RealFourier") && std::get<2>(key)>=2);
        if(domain::numeric_constant_kind(node->kind)){if(!s.frames || !s.positive)return false;continue;}
        if(s.frames<5||s.last<=s.first||(!signed_metric&&!s.positive))return false;}for(const auto& [id,h]:histories)if(h.rows<5||!h.positive)return false;return true;};
    const bool delivered=wait([&]{std::lock_guard lock(mutex);return ready();});engine.unsubscribe_numeric_tap(tap);
    std::lock_guard lock(mutex);const bool good=delivered&&!bad&&opened==expected_opens&&ready();
    std::printf("shipped %s: opens=%u stages=%zu history=%zu %s\n",name,opens,stages.size(),histories.size(),good?"PASS":"FAIL");
    if(!good)for(const auto& [key,s]:stages)std::printf(" stage%lld/%u/%u frames=%u positive=%d seq=%llu..%llu\n",(long long)std::get<0>(key),unsigned(std::get<1>(key)),unsigned(std::get<2>(key)),s.frames,int(s.positive),(unsigned long long)s.first,(unsigned long long)s.last);
    return good;
}
