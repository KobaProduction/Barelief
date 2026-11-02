#pragma once
#include <pipeline/numeric_engine.h>
#include <bdsp_protocol/numeric_control_limits.h>
#include <domain/numeric/contracts.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <mutex>

namespace basic_signal_probe {
using namespace signal_contracts;
using namespace pipeline;
inline const domain::ConfiguredProcessingNode* node(const domain::NumericProcessingGraph& graph,domain::NodeId id) {
    const auto at=std::find_if(graph.nodes.begin(),graph.nodes.end(),[id](const auto& value){return value.id==id;});
    return at==graph.nodes.end()?nullptr:&*at;
}
inline bool graph_is_basic(const domain::NumericProcessingGraph& graph,std::size_t size) {
    if(graph.nodes.size()!=3||graph.edges.size()!=2||!graph.configuration_bindings.empty())return false;
    const auto* generator=node(graph,1);const auto* window=node(graph,2);const auto* view=node(graph,3);
    if(!generator||!window||!view)return false;
    if(generator->kind!="SignalGenerator"||window->kind!="Window"||view->kind!="VectorLineView")return false;
    for(const auto edge:{domain::Edge{1,2,0,0},domain::Edge{2,3,0,0}})
        if(std::find(graph.edges.begin(),graph.edges.end(),edge)==graph.edges.end())return false;
    const auto contract=std::get<NumericContract>(window->outputs.front().contract.view());
    return contract.dtype==DType::Float32&&contract.shape.rank==Rank::Vector&&contract.shape.extents[0]==ExtentBounds{size,size};
}
inline std::vector<EndpointIdentity> endpoints() {
    return {{"main",1,domain::PortDirection::Output,0},{"main",2,domain::PortDirection::Output,0},{"main",3,domain::PortDirection::Input,0}};
}
struct Observation {
    static constexpr std::size_t retained_samples=8192;
    std::mutex mutex;std::array<float,retained_samples> samples{};std::array<std::int64_t,retained_samples> positions;
    std::array<std::int64_t,retained_samples> timestamps{};
    std::size_t size;DeliveryStamp fence;bool bad=false;const char* reason="";
    std::uint64_t sources=0,windows=0,views=0,last_window=0,last_view=0;std::int64_t last_begin=0;
    DeliveredNumericFrame pending_window,pending_view;
    Observation(std::size_t count,DeliveryStamp delivery):size(count),fence(delivery){positions.fill(-1);}
    void fail(const char* message){if(!bad)reason=message;bad=true;}
    bool readable(const NumericView& numeric,std::size_t index,float& value) {
        if(load_element(numeric,{0,index,0},value)==Status::ok&&std::isfinite(value))return true;
        fail("nonfinite or unreadable Float32 value");return false;
    }
    bool valid(const DeliveredNumericFrame& delivered,const DataFrameView& frame,const NumericView* numeric) {
        if(!delivered.context||delivered.delivery!=fence){fail("public delivery context/fence");return false;}
        if(delivered.status!=Status::ok||!frame.stamp.available||!numeric){fail("unavailable numeric frame");return false;}
        if(validate_view(frame,bdsp_protocol::numeric_control::desktop_limits().frame)!=Status::ok){fail("invalid frame shape/coordinates");return false;}
        if(numeric->contract->dtype!=DType::Float32||numeric->contract->shape.rank!=Rank::Vector){fail("expected Float32 vector");return false;}
        return true;
    }
    bool store_source_sample(const NumericView& numeric,const FrameStamp& stamp,std::size_t index) {
        float value=0;if(!readable(numeric,index,value))return false;
        const auto position=stamp.sample_begin+std::int64_t(index);const auto slot=std::size_t(position)%retained_samples;
        samples[slot]=value;positions[slot]=position;timestamps[slot]=stamp.timestamp_ticks;return true;
    }
    void source(const NumericView& numeric,const FrameStamp& stamp) {
        if(!stamp.has_sample_interval||stamp.sample_begin<0||stamp.sample_count!=numeric.actual[0]){fail("generator sample interval");return;}
        if(!numeric.actual[0]||numeric.actual[0]>256){fail("generator packet geometry");return;}
        for(std::size_t index=0;index<numeric.actual[0];++index)
            if(!store_source_sample(numeric,stamp,index))return;
        ++sources;
    }
    bool covered(const FrameStamp& stamp)const {
        for(std::size_t index=0;index<size;++index){
            const auto position=stamp.sample_begin+std::int64_t(index);
            if(position>=0&&positions[std::size_t(position)%retained_samples]!=position)return false;
        }
        return true;
    }
    bool values_match(const NumericView& numeric,const FrameStamp& stamp) {
        for(std::size_t index=0;index<size;++index){
            float actual=0;if(!readable(numeric,index,actual))return false;
            const auto position=stamp.sample_begin+std::int64_t(index);
            const float expected=position<0?0.f:samples[std::size_t(position)%retained_samples];
            if(actual!=expected){fail("Window value differs from generator stream");return false;}
        }
        return true;
    }
    void progress(const FrameStamp& stamp,bool sink) {
        if(sink){++views;last_view=stamp.sequence;return;}
        if(windows&&(stamp.sequence!=last_window+1||stamp.sample_begin<=last_begin))fail("Window input progression");
        ++windows;last_window=stamp.sequence;last_begin=stamp.sample_begin;
    }
    bool window_geometry(const NumericView& numeric,const FrameStamp& stamp) {
        if(numeric.actual!=std::array<std::size_t,2>{size,1}||stamp.sample_count!=size){fail("Window actual size");return false;}
        if(!stamp.has_sample_interval){fail("Window output interval");return false;}
        return true;
    }
    bool window(const DeliveredNumericFrame& delivered,bool sink) {
        const auto frame=delivered.frame.view();const auto numeric=std::get<NumericView>(frame.payload);
        if(!window_geometry(numeric,frame.stamp))return true;
        if(!covered(frame.stamp))return false;
        const auto last=frame.stamp.sample_begin+std::int64_t(size)-1;
        if(last<0 || frame.stamp.timestamp_ticks!=timestamps[std::size_t(last)%retained_samples]){fail("Window changed the input timestamp");return true;}
        if(values_match(numeric,frame.stamp))progress(frame.stamp,sink);
        return true;
    }
    void retry_pending() {
        if(pending_window.frame&&window(pending_window,false))pending_window={};
        if(pending_view.frame&&window(pending_view,true))pending_view={};
    }
    void route_window(const DeliveredNumericFrame& delivered,const EndpointIdentity& endpoint) {
        const bool sink=endpoint.node==3&&endpoint.direction==domain::PortDirection::Input;
        if(!sink&&(endpoint.node!=2||endpoint.direction!=domain::PortDirection::Output)){fail("unexpected Window/view endpoint");return;}
        if(!window(delivered,sink)){if(sink)pending_view=delivered;else pending_window=delivered;}
    }
    void route(const DeliveredNumericFrame& delivered,const DataFrameView& frame,const NumericView& numeric) {
        const auto& endpoint=delivered.context->endpoint;
        if(endpoint.graph_id!="main"||endpoint.port!=0){fail("unexpected endpoint identity");return;}
        if(endpoint.node==1&&endpoint.direction==domain::PortDirection::Output){source(numeric,frame.stamp);retry_pending();return;}
        route_window(delivered,endpoint);
    }
    void observe(const DeliveredNumericFrame& delivered) {
        std::lock_guard lock(mutex);if(bad)return;
        if(!delivered.frame){fail("missing owned frame");return;}
        const auto frame=delivered.frame.view();const auto* numeric=std::get_if<NumericView>(&frame.payload);
        if(valid(delivered,frame,numeric))route(delivered,frame,*numeric);
    }
    bool ready(){std::lock_guard lock(mutex);return bad||(windows>=3&&views>=3&&last_window==last_view);}
    bool good(){std::lock_guard lock(mutex);return !bad&&sources>=3&&windows>=3&&views>=3&&last_window==last_view;}
    void report(const char* phase) {
        std::lock_guard lock(mutex);
        std::fprintf(stderr,"basic phase=%s N=%zu generator_packets=%llu Window_outputs=%llu view_inputs=%llu last_output=%llu reason=%s\n",phase,size,(unsigned long long)sources,(unsigned long long)windows,(unsigned long long)views,(unsigned long long)last_window,reason);
    }
};
}
