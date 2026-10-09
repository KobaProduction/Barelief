#pragma once
#include "synthetic_capture.h"
#include <pipeline/numeric_engine.h>
#include <desktop/numeric_project_document.h>
#include <QElapsedTimer>
#include <QCoreApplication>
#include <QJsonDocument>
#include <QThread>
#include <array>
#include <mutex>
#include <cmath>
#include <cstdio>

// Actual capture -> canonical executor -> owned delivery. No injected frames,
// physical capture, renderer demand, or DSP work in editor models.
inline bool measure_shipped_example(const domain::NumericProcessingGraph& graph,const char* name,
                                    pipeline::AudioSourceKind expected_source) {
    using namespace pipeline;using namespace signal_contracts;
    struct Stage { unsigned frames=0;double first=0,last=0;bool positive=false,changed=false; };
    struct Metrics {std::mutex mutex;std::array<Stage,12> stage{};bool bad=false;unsigned opens=0,rows=0,view_rows=0;std::uint64_t ordinal=0,count=0,view_count=0;};
    Metrics metrics;
    const auto factory=test_fixture::synthetic_capture_factory();
    NumericEngine engine([&](auto kind){
        {std::lock_guard lock(metrics.mutex);++metrics.opens;metrics.bad|=kind!=expected_source && !(graph.nodes.size()==15 && kind==AudioSourceKind::SystemOutput);}
        return factory(kind);
    });
    bool replied=false,accepted=false;
    engine.set_reply_handler([&](const auto& reply){replied=true;accepted=reply.accepted;if(!accepted)std::fprintf(stderr,"%s command: %s\n",name,qPrintable(reply.error));});
    auto wait=[](const auto& predicate){QElapsedTimer timer;timer.start();while(timer.elapsed()<5000){QCoreApplication::processEvents();if(predicate())return true;QThread::msleep(2);}return predicate();};
    auto submit=[&](EngineCommand::Kind kind,QJsonObject document={}){replied=false;EngineCommand command;command.kind=kind;command.graph=std::move(document);engine.submit(std::move(command));return wait([&]{return replied;})&&accepted;};
    QByteArray bytes;
    if(!desktop::encode_numeric_graph(graph,bytes)||!submit(EngineCommand::ApplyGraph,QJsonDocument::fromJson(bytes).object()))return false;
    auto sample=[&](const DeliveredNumericFrame& delivered,unsigned stage){
        if(!delivered.frame||!delivered.frame.view().stamp.available)return;
        const auto view=delivered.frame.view();const auto* value=std::get_if<NumericView>(&view.payload);
        if(!value)return;
        if((stage==7 && delivered.context->endpoint.port==1)||stage==12){
            std::uint64_t count=0;if(load_element(*value,{0,0,0},count)!=Status::ok){metrics.bad=true;return;}(stage==12?metrics.view_count:metrics.count)=count;return;
        }
        if(value->contract->dtype!=DType::Float32){metrics.bad=true;return;}
        const auto& expected=stage<=7?graph.nodes[stage-1].outputs[0].contract:graph.nodes[stage-1].inputs[0].contract;
        const auto contract=std::get<NumericContract>(expected.view());
        // Compact History rows retain Mel's vector contract.
        if(stage!=7 && stage!=8 && (!same_contract(contract,*value->contract)||value->actual[0]!=contract.shape.extents[0].maximum)){metrics.bad=true;return;}
        if((stage==7||stage==8) && (value->actual[0]!=50||value->contract->semantic!=Semantic::MelEnergy)){metrics.bad=true;return;}
        double energy=0;
        for(std::size_t plane=0;plane<value->planes.size();++plane)for(std::size_t i=0;i<value->actual[0];++i){float x=0;
            if(load_element(*value,{plane,i,0},x)!=Status::ok||!std::isfinite(x)){metrics.bad=true;return;}energy+=double(x)*x;
        }
        auto& state=metrics.stage[stage];if(!state.frames)state.first=energy;
        ++state.frames;state.last=energy;state.positive|=energy>1e-12;state.changed|=std::abs(energy-state.first)>1e-10;
    };
    const auto tap=engine.subscribe_numeric_tap({},[&](auto delivered){
        if(!delivered.context)return;const auto endpoint=delivered.context->endpoint;
        if(endpoint.node<1||endpoint.node>12||endpoint.node==8)return;
        if(endpoint.direction!=(endpoint.node<=7?PortDirection::Output:PortDirection::Input))return;
        if(endpoint.node==7&&endpoint.port==0)return;
        std::lock_guard lock(metrics.mutex);sample(delivered,unsigned(endpoint.node));
    },[&](auto row){
        if(row.event!=NumericHistoryEvent::Row||!row.compact.context)return;const auto endpoint=row.compact.context->endpoint;
        if(!((endpoint.node==7&&endpoint.direction==PortDirection::Output)||(endpoint.node==8&&endpoint.direction==PortDirection::Input)))return;
        std::lock_guard lock(metrics.mutex);
        metrics.bad|=row.capacity!=400||row.filled_count!=std::min(row.accepted_row_ordinal,std::uint64_t{400});
        if(endpoint.node==7){metrics.bad|=row.accepted_row_ordinal<=metrics.ordinal;metrics.ordinal=row.accepted_row_ordinal;++metrics.rows;}else ++metrics.view_rows;
        sample(row.compact,unsigned(endpoint.node));
    });
    if(!tap)return false;
    const bool delivered=wait([&]{std::lock_guard lock(metrics.mutex);if(metrics.bad)return true;
        if(metrics.rows<5||metrics.view_rows<5||metrics.count<5||metrics.view_count<5)return false;for(unsigned i=1;i<=11;++i)if(metrics.stage[i].frames<5||!metrics.stage[i].positive||!metrics.stage[i].changed)return false;return true;
    });
    engine.unsubscribe_numeric_tap(tap);
    std::lock_guard lock(metrics.mutex);
    std::printf("shipped %s: source_opens=%u rows=%u view_rows=%u ordinal=%llu filled=%llu view_count=%llu",name,metrics.opens,metrics.rows,metrics.view_rows,
                static_cast<unsigned long long>(metrics.ordinal),static_cast<unsigned long long>(metrics.count),static_cast<unsigned long long>(metrics.view_count));
    bool good=delivered&&!metrics.bad&&metrics.opens>0&&metrics.rows>=5&&metrics.view_rows>=5&&metrics.count>=5&&metrics.view_count>=5;
    for(unsigned i=1;i<=11;++i){const auto& state=metrics.stage[i];std::printf(" stage%u=%u/%.9g/%.9g",i,state.frames,state.first,state.last);good&=state.frames>=5&&state.positive&&state.changed;}
    std::printf(" %s\n",good?"PASS":"FAIL");return good;
}
