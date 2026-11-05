// SPDX-License-Identifier: AGPL-3.0-or-later
#include "value_probe.h"
#include <QJsonArray>
#include <algorithm>
#include <cmath>
#include <mutex>
#include <stdexcept>

namespace {
pipeline::EndpointIdentity endpoint(const domain::NumericProcessingGraph& graph,const QString& text) {
    const auto parts=text.split(':');bool node_ok=false,port_ok=false;
    if(parts.size()!=2)throw std::invalid_argument("observe expects node:port");
    const auto node=parts[0].toInt(&node_ok);const auto port=parts[1].toUInt(&port_ok);
    const auto found=std::find_if(graph.nodes.begin(),graph.nodes.end(),[=](const auto& value){return value.id==node;});
    if(!node_ok || !port_ok || port>65535 || found==graph.nodes.end())throw std::invalid_argument("unknown observed node");
    if(std::none_of(found->outputs.begin(),found->outputs.end(),[=](const auto& value){return value.id==port;}))throw std::invalid_argument("unknown observed output");
    return {graph.graph_id,node,domain::PortDirection::Output,domain::PortId(port)};
}
QJsonValue first_value(const signal_contracts::NumericView& view) {
    using namespace signal_contracts;
    double value=0;float single=0;std::uint64_t integer=0;
    if(view.contract->dtype==DType::Float32 && load_element(view,{0,0,0},single)==Status::ok)value=single;
    else if(view.contract->dtype==DType::Float64 && load_element(view,{0,0,0},value)==Status::ok){}
    else if(view.contract->dtype==DType::UInt64 && load_element(view,{0,0,0},integer)==Status::ok)return QString::number(integer);
    else return "unsupported dtype";
    return std::isfinite(value)?QJsonValue(value):QJsonValue(QString::number(value));
}
}
struct ValueProbe::State {
    mutable std::mutex mutex;
    std::uint64_t available=0,unavailable=0,nonempty=0;
    QString selected;
    QJsonArray shape;
    QJsonValue value;
    void observe(const pipeline::DeliveredNumericFrame& frame) {
        std::lock_guard lock(mutex);
        if(frame.status!=signal_contracts::Status::ok || !frame.frame || !frame.frame.view().stamp.available){++unavailable;return;}
        ++available;const auto borrowed=frame.frame.view();const auto* view=std::get_if<signal_contracts::NumericView>(&borrowed.payload);
        if(!view || !view->actual[0] || !view->actual[1])return;
        ++nonempty;shape={int(view->actual[0]),int(view->actual[1])};value=first_value(*view);
    }
};
ValueProbe::ValueProbe(pipeline::NumericEngine& engine,const domain::NumericProcessingGraph& graph,const QString& text):engine_(engine) {
    if(text.isEmpty())return;
    const auto selected=endpoint(graph,text);state_=std::make_shared<State>();state_->selected=text;
    token_=engine_.subscribe_numeric_tap({selected},[state=state_](auto frame){state->observe(frame);},{});
    if(!token_)throw std::runtime_error("observed output subscription rejected");
}
ValueProbe::~ValueProbe(){if(token_)engine_.unsubscribe_numeric_tap(token_);}
bool ValueProbe::satisfied() const {
    if(!state_)return true;
    std::lock_guard lock(state_->mutex);return state_->nonempty!=0;
}
QJsonObject ValueProbe::report() const {
    if(!state_)return {{"enabled",false}};
    std::lock_guard lock(state_->mutex);
    return {{"enabled",true},{"endpoint",state_->selected},{"available",qint64(state_->available)},{"unavailable",qint64(state_->unavailable)},
        {"nonempty",qint64(state_->nonempty)},{"last_shape",state_->shape},{"first_value",state_->value}};
}
