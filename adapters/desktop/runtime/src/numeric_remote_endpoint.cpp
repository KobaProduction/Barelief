#include <control/numeric_remote_endpoint.h>
#include <control/numeric_remote_support.h>
#include <QTimer>
#include <QRandomGenerator>
#include <QJsonArray>
#include <optional>
#include <QPointer>

namespace pipeline {
using namespace remote_numeric;
struct NumericRemoteEndpoint::Impl {
    NumericRemoteEndpoint& owner;QString host,device="numeric-desktop",warning;quint16 port=0;
    std::optional<desktop::SerialEndpoint> serial_endpoint;
    QTcpSocket socket;std::unique_ptr<desktop::QtTcpByteTransport> tcp;
    std::unique_ptr<desktop::QtSerialByteTransport> serial;
    QTimer timer;Clock clock;std::unique_ptr<control::numeric::ControlSession> control;
    CodecStorage codec;std::shared_ptr<ArenaBudget> arena_budget=std::make_shared<ArenaBudget>();std::shared_ptr<Arena> arena,staged_arena;
    EngineSnapshot state;domain::NumericProcessingGraph graph;
    std::function<void(EngineSnapshot)> snapshot_handler;std::function<void(EngineReply)> reply_handler;
    std::function<void(DeliveredNumericFrame)> frame_handler;
    std::function<void(DeliveredNumericHistoryRow)> history_handler;
    std::function<void(DeliveredNumericFrame,std::uint64_t,std::uint64_t)> history_snapshot_handler;
    struct HistorySnapshot {DeliveredNumericFrame frame;std::uint64_t ordinal,filled_count;};
    std::deque<HistorySnapshot> history_snapshots;
    std::vector<EndpointIdentity> desired;std::deque<Binding> bindings;
    struct Ingress {EndpointIdentity endpoint;DeliveredNumericFrame frame;std::optional<std::uint64_t> history_ordinal;std::uint64_t filled_count=0;};
    std::deque<Ingress> ingress;
    std::deque<Queued> received;
    struct Work {quint64 id;EngineCommand command;};std::deque<Work> work;
    std::optional<Work> active;control::numeric::GraphProjection projection;
    struct Query {EndpointIdentity endpoint;std::uint64_t request;bool sent=false;};std::deque<Query> queries;
    bool cold_snapshot=false,cold_selected=false;std::uint64_t cold_started=0,next_query=1;
    DeliveryLosses receive_losses,ingress_losses;
    bool discard_data=false;
    std::optional<nc::Command> unsent;std::uint64_t unsent_expected=0;
    std::uint64_t nonce=0,next_request=1,next_public=1,waiting=0,token=0,next_binding=1,sequence=0,received_sequence=0,last_state=0,snapshot_generation=0;
    std::uint64_t snapshot_handler_generation=0,reply_handler_generation=0,frame_handler_generation=0,history_handler_generation=0;
    nc::CommandId phase=nc::CommandId::Capabilities;
    bool connected=false,pumping=false,retiring=false,capabilities=false,bootstrapped=false,rebind=false,apply_next=false;
    Impl(NumericRemoteEndpoint& q):owner(q) {
        timer.setInterval(5);QObject::connect(&timer,&QTimer::timeout,&owner,[this]{pump();});
        QObject::connect(&socket,&QTcpSocket::connected,&owner,[this]{start();});
        QObject::connect(&socket,&QTcpSocket::disconnected,&owner,[this]{
            if(socket.state()==QAbstractSocket::UnconnectedState)retire(QStringLiteral("Remote disconnected"));
        },Qt::QueuedConnection);
        QObject::connect(&socket,&QTcpSocket::readyRead,&owner,[this]{pump();});
        QObject::connect(&socket,&QTcpSocket::errorOccurred,&owner,[this](auto){
            if(socket.state()==QAbstractSocket::UnconnectedState)retire(socket.errorString());
        },Qt::QueuedConnection);
    }
    void emit_reply(EngineReply reply) {
        QMetaObject::invokeMethod(&owner,[this,reply=std::move(reply),generation=snapshot_generation,handler_generation=reply_handler_generation]() mutable {
            if(handler_generation!=reply_handler_generation)return;
            if(generation!=snapshot_generation) {
                reply.accepted=false;reply.error=QStringLiteral("Remote session retired after command");reply.snapshot=state;
            } else reply.snapshot=state;
            if(reply_handler)reply_handler(std::move(reply));
        },Qt::QueuedConnection);
    }
    void emit_snapshot() {
        QMetaObject::invokeMethod(&owner,[this,generation=snapshot_generation,handler_generation=snapshot_handler_generation] {
            if(generation==snapshot_generation&&handler_generation==snapshot_handler_generation&&snapshot_handler)snapshot_handler(state);
        },Qt::QueuedConnection);
    }
    void finish(bool accepted,QString error={}) {
        if(active)emit_reply({active->id,accepted,std::move(error),state});active.reset();apply_next=false;
        cold_snapshot=cold_selected=false;queries.clear();
    }
    void fence() {bindings.clear();staged_arena.reset();ingress.clear();received.clear();receive_losses.clear();ingress_losses.clear();history_snapshots.clear();queries.clear();cold_selected=false;codec.reset();received_sequence=0;discard_data=true;}
    void retire(QString error={}) {
        if(retiring)return;retiring=true;
        auto retired_bindings=std::move(bindings);
        timer.stop();connected=false;fence();unsent.reset();waiting=0;capabilities=bootstrapped=rebind=false;
        arena.reset();
        if(control)control->reset();control.reset();
        // Terminal notifications use the retiring delivery fence while snapshot()
        // still names it, so the selected presenter can clear the matching views.
        QPointer<NumericRemoteEndpoint> lifetime(&owner);
        const auto on_frame=frame_handler;const auto on_history=history_handler;
        const auto frame_generation=frame_handler_generation,history_generation=history_handler_generation;
        for(const auto& binding:retired_bindings) {
            const auto contract=binding.context->contract.view();const auto* history=std::get_if<signal_contracts::NumericContract>(&contract);
            DeliveredNumericFrame unavailable{binding.context,{state.graph_revision,state.epoch},{},Status::unavailable};
            if(history&&binding.ring) {
                DeliveredNumericHistoryRow marker;marker.compact=std::move(unavailable);marker.event=NumericHistoryEvent::Unavailable;
                marker.capacity=history->shape.extents[0].maximum;if(on_history && history_generation==history_handler_generation)on_history(std::move(marker));
            } else if(on_frame && frame_generation==frame_handler_generation)on_frame(std::move(unavailable));
            if(!lifetime)return;
        }
        // No applied snapshot or cold frame remains authoritative after this
        // transport session is retired. Previously queued snapshots are fenced.
        ++snapshot_generation;graph={};state={};token=0;nonce=0;
        state.statuses[QStringLiteral("analyzer")]={State::Stopped,error,0};
        emit_snapshot();
        if(active)finish(false,error);
        while(!work.empty()) {auto w=std::move(work.front());work.pop_front();emit_reply({w.id,false,error,state});}
        if(!error.isEmpty())warning=std::move(error);
        retiring=false;
    }
    void start() {
        if(control)retire();fence();warning.clear();control::numeric::SessionConfig config;
        ++snapshot_generation;
        config.endpoint_authorized=true;config.device_identity=device.toStdString();config.codec_limits=control_limits();
        control=std::make_unique<control::numeric::ControlSession>(config,clock);
        control->set_reply_handler([](const nc::Message& m,void* p){static_cast<Impl*>(p)->reply(m);},this);
        control->set_packet_handler([](bdsp_protocol::PacketView p,void* c){static_cast<Impl*>(c)->packet(p);},this);
        nonce=QRandomGenerator::global()->generate64();if(!nonce)nonce=1;
        next_request=1;next_binding=1;sequence=0;last_state=0;connected=true;
        if(control->open(nonce)!=Status::ok){retire(QStringLiteral("CONTROL2 negotiation failed"));return;}
        timer.start();
    }
    void reply(const nc::Message& m) {
        if(m.request!=waiting)return;waiting=0;
        const auto* r=std::get_if<nc::Reply>(&m.body);if(!r)return;
        // Pace observation from completion: a large/slow State reply must not
        // immediately schedule another query and starve DATA binding/transport.
        if(phase==nc::CommandId::State)last_state=clock.now_ms();
        const auto previous=state.graph_revision;
        state.epoch=m.epoch;state.revision=r->state.revision;state.graph_revision=remote_numeric::graph_revision(m);
        state.statuses[QStringLiteral("analyzer")]={r->state.run_intent?State::Running:State::Stopped,{},state.revision};
        if(r->state.applied) {
            domain::NumericProcessingGraph next;
            if(!control::numeric::own_graph(*r->state.applied,control_limits(),next)||!project_graph(next,state.graph)) {
                warning=QStringLiteral("Invalid remote canonical graph");finish(false,warning);return;
            }
            graph=std::move(next);
        } else {graph={};state.graph={};}
        if(previous!=state.graph_revision) {fence();rebind=true;}
        emit_snapshot();
        if(phase==nc::CommandId::Capabilities) {
            capabilities=r->error==nc::Error::None&&r->capabilities&&r->capabilities->control_version==2&&
                r->capabilities->data_version==4&&(r->capabilities->codec_mask&3);
            if(!capabilities)warning=QStringLiteral("Remote numeric CONTROL2/DATA4 unsupported");
            bootstrapped=true;rebind=true;return;
        }
        if(!active)return;
        if(r->error!=nc::Error::None) {finish(false,QStringLiteral("Remote control rejected (%1)").arg(int(r->error)));return;}
        if(phase==nc::CommandId::State&&active->command.kind==EngineCommand::Snapshot) {
            cold_snapshot=true;cold_started=clock.now_ms();cold_selected=false;state.numeric_frames.clear();return;
        }
        if(phase==nc::CommandId::Prepare) {token=r->state.token;if(!token){finish(false,QStringLiteral("Missing preparation token"));return;}apply_next=true;}
        else finish(true);
    }
    bool send(nc::Command command,std::uint64_t expected) {
        const auto request=next_request;
        auto result=control->send(command,request,expected);
        if(result==Status::would_block){unsent=command;unsent_expected=expected;return false;}
        if(result!=Status::ok) {warning=QStringLiteral("Remote CONTROL2 send failed");finish(false,warning);return false;}
        unsent.reset();++next_request;waiting=request;phase=command.id;return true;
    }
    void select_bindings() {
        rebind=false;
        if(!capabilities||graph.nodes.empty())return;
        auto selected=desired.empty()?default_endpoints(graph):desired;
        if(selected.size()>MaxBindings){warning=QStringLiteral("Too many remote numeric ports");return;}
        if(selected.empty()){fence();return;}
        auto next_arena=staged_arena?staged_arena:Arena::prepare_rebind(nd::desktop_limits().frame,selected.size(),arena_budget,arena);
        if(!next_arena){warning=QStringLiteral("Remote numeric retention budget exhausted");rebind=true;return;}
        std::deque<Binding> next;
        auto next_id=next_binding;
        for(const auto& e:selected) {
            const auto* p=find_port(graph,e);if(!p){warning=QStringLiteral("Remote numeric port missing");continue;}
            if(std::any_of(next.begin(),next.end(),[&](const auto& b){return b.context->endpoint==e;}))continue;
            Binding binding;
            if(make_binding(e,p->contract,std::uint32_t(next_id++),next_arena,binding)!=Status::ok) {
                warning=QStringLiteral("Remote numeric binding preparation refused");return;
            }
            next.push_back(std::move(binding));
        }
        fence();arena=std::move(next_arena);bindings=std::move(next);next_binding=next_id;
    }
    void packet(bdsp_protocol::PacketView p) {
        if(!capabilities||!control)return;
        auto bytes=p.payload;
        if(p.id==nd::FragmentId) {if(codec.assembler.accept(bytes,bytes)!=Status::ok||bytes.empty())return;}
        else if(p.id!=nd::AckId&&p.id!=nd::EventsId&&p.id!=nd::SnapshotId)return;
        nd::Message m;const auto identity=control->identity();
        const auto decoded=codec.decode(bytes,m,codec.reference_context(bindings,nonce,identity.session,state.graph_revision));if(decoded!=Status::ok){warning=QStringLiteral("Remote numeric decode refused: status=%1").arg(int(decoded));return;}
        if(m.envelope.nonce!=nonce||m.envelope.session!=identity.session||m.envelope.revision!=state.graph_revision||m.envelope.delivery_epoch!=state.epoch||
           m.envelope.delivery_generation!=m.stamp.generation)return;
        auto found=std::find_if(bindings.begin(),bindings.end(),[&](const auto& b){return b.context->endpoint==endpoint(m.endpoint);});
        if(found==bindings.end())return;
        if(p.id==nd::AckId) {
            if(m.kind==nd::Kind::Unavailable&&!m.stamp.available&&m.envelope.delivery_sequence==1&&!m.envelope.delivery_generation&&
                m.envelope.request==found->id&&m.stamp.stream_id==found->stream&&
                signal_contracts::same_contract(m.contract,found->wire_owner.view()))found->acknowledged=true;
            return;
        }
        if(!found->acknowledged||!m.envelope.delivery_sequence||m.envelope.delivery_sequence<=received_sequence)return;
        if(m.envelope.request) {
            auto query=std::find_if(queries.begin(),queries.end(),[&](const auto& q){return q.sent&&q.request==m.envelope.request&&q.endpoint==found->context->endpoint;});
            if(query==queries.end())return;
            DeliveredNumericFrame retained;if(retain_snapshot(m,*found,codec,arena,retained)!=Status::ok)return;
            found->ring=m.history.capacity!=0;
            if(m.kind==nd::Kind::Frame || (m.kind==nd::Kind::Unavailable && !m.history.capacity)){
                Queued ordinary=retained;enqueue_delivery(received,ordinary,receive_losses);
            }else if(history_snapshot_handler&&history_snapshots.size()<QueueSize)history_snapshots.push_back({retained,m.history.ordinal,m.history.filled});
            received_sequence=m.envelope.delivery_sequence;state.numeric_frames.push_back(std::move(retained));queries.erase(query);return;
        }
        Queued retained;
        if(retain_delivery(m,*found,codec,arena,retained)!=Status::ok){receive_losses.insert(found->context->endpoint);warning=QStringLiteral("Remote numeric delivery refused");return;}
        found->ring=m.history.capacity!=0;
        received_sequence=m.envelope.delivery_sequence;
        if(enqueue_delivery(received,retained,receive_losses)==EnqueueResult::Overflow)
            warning=QStringLiteral("Remote numeric delivery mailbox overflow");
    }
    void pump() {
        if(pumping||!connected||!control)return;pumping=true;
        QPointer<NumericRemoteEndpoint> lifetime(&owner);
        auto* transport=serial?static_cast<bdsp_protocol::ByteTransport*>(serial.get()):tcp.get();
        const auto pending=serial?serial->device().bytesToWrite():socket.bytesToWrite();
        const auto result=control->poll(*transport,pending);
        if(result!=Status::ok&&result!=Status::would_block) {pumping=false;retire(QStringLiteral("Remote session failed"));return;}
        if(discard_data&&control->state()==control::ControlSessionState::Ready)
            discard_data=control->discard_outbound_data()==Status::would_block;
        while(!received.empty()) {
            auto value=std::move(received.front());received.pop_front();
            if(auto* f=std::get_if<DeliveredNumericFrame>(&value)){if(frame_handler)frame_handler(std::move(*f));}
            else if(history_handler)history_handler(std::move(std::get<DeliveredNumericHistoryRow>(value)));
            if(!lifetime)return;
            if(!control||!connected){pumping=false;return;}
        }
        while(!history_snapshots.empty()) {
            auto next=std::move(history_snapshots.front());history_snapshots.pop_front();
            if(next.frame.delivery.epoch==state.epoch&&next.frame.delivery.graph_revision==state.graph_revision&&history_snapshot_handler)
                history_snapshot_handler(std::move(next.frame),next.ordinal,next.filled_count);
            if(!lifetime)return;
            if(!control||!connected){pumping=false;return;}
        }
        if(control->state()!=control::ControlSessionState::Ready||waiting){pumping=false;return;}
        if(unsent) {auto c=*unsent;send(c,unsent_expected);}
        else if(!bootstrapped) {nc::Command c;c.id=nc::CommandId::Capabilities;send(c,state.revision);}
        else if(active&&apply_next) {nc::Command c;c.id=nc::CommandId::Apply;c.token=token;c.graph_id=projection.wire().graph_id;send(c,state.revision);}
        else if(!active&&!work.empty()) {
            active=std::move(work.front());work.pop_front();nc::Command c;
            // EnginePort uses the applied graph revision. CONTROL has a separate
            // mutation revision, including Start/Stop and preparation ownership.
            const auto expected=state.revision;
            switch(active->command.kind) {
            case EngineCommand::Snapshot:c.id=nc::CommandId::State;break;
            case EngineCommand::ApplyGraph: {
                bool routing=false;
                for(const auto value:active->command.graph.value("nodes").toArray()) {
                    const auto node=value.toObject();
                    for(const auto adapter:{node.value("adapter"),node.value("internal-data").toObject().value("adapter")})
                        routing|=!adapter.isUndefined()&&(!adapter.isObject()||!adapter.toObject().isEmpty());
                }
                if(routing){finish(false,QStringLiteral("Remote adapter routing configuration is not supported by CONTROL2"));pumping=false;return;}
                if(active->command.expected_revision!=std::numeric_limits<quint64>::max()&&active->command.expected_revision!=state.graph_revision) {
                    finish(false,QStringLiteral("Graph revision conflict"));pumping=false;return;
                }
                domain::NumericProcessingGraph next;auto decoded=desktop::decode_numeric_graph(QJsonDocument(active->command.graph).toJson(QJsonDocument::Compact),next);
                if(!decoded||!projection.assign(next,control_limits())){finish(false,decoded?QStringLiteral("Remote graph exceeds CONTROL2 limits"):decoded.error);pumping=false;return;}
                c.id=nc::CommandId::Prepare;c.graph=projection.wire();break;
            }
            case EngineCommand::Subscribe:rebind=true;finish(true);pumping=false;return;
            default:finish(false,QStringLiteral("Remote canonical command unsupported"));pumping=false;return;
            }
            send(c,expected);
        } else if(!active&&clock.now_ms()-last_state>=500) {nc::Command c;c.id=nc::CommandId::State;if(send(c,state.revision))last_state=clock.now_ms();}
        else if(capabilities) {
            if(rebind)select_bindings();
            // Complete cancellation of retiring DATA before queueing a fresh
            // binding. A partially written old packet may need another poll.
            if(discard_data){pumping=false;return;}
            if(codec.wire_bytes)codec.flush(*control);
            else {
                // readyRead and the timer can both pump. Only one binding may
                // await an ACK; retry the same ID after bounded backpressure.
                auto pending=std::find_if(bindings.begin(),bindings.end(),[](const auto& b){return !b.acknowledged&&b.sent;});
                auto unbound=bindings.end();const auto now=clock.now_ms();
                if(pending!=bindings.end()) {
                    if(now-pending->sent_at>=500)unbound=pending;
                } else unbound=std::find_if(bindings.begin(),bindings.end(),[](const auto& b){return !b.acknowledged&&!b.sent;});
                if(unbound!=bindings.end()) {auto m=handshake(*unbound,nonce,control->identity().session,state.graph_revision,state.epoch);if(codec.encode(m,nd::SubscribeId)==Status::ok){unbound->sent=true;unbound->sent_at=now;codec.flush(*control);}}
                else if(cold_snapshot) {
                    if(!cold_selected&&std::all_of(bindings.begin(),bindings.end(),[](const auto& b){return b.acknowledged;})) {
                        for(const auto& b:bindings) {
                            const auto c=b.context->contract.view();const auto* h=std::get_if<signal_contracts::NumericContract>(&c);
                            if(h&&h->shape.rank==signal_contracts::Rank::Matrix)queries.push_back({b.context->endpoint,next_query++,false});
                        }
                        cold_selected=true;
                        if(queries.size()>QueueSize){finish(false,QStringLiteral("Cold snapshot query exceeds bounded queue"));pumping=false;return;}
                    }
                    if(cold_selected&&queries.empty())finish(true);
                    else if(clock.now_ms()-cold_started>=5000)finish(false,QStringLiteral("Cold history snapshot timeout"));
                    else {
                        auto q=std::find_if(queries.begin(),queries.end(),[](const auto& q){return !q.sent;});
                        if(q!=queries.end()) {
                            auto b=std::find_if(bindings.begin(),bindings.end(),[&](const auto& b){return b.context->endpoint==q->endpoint;});
                            if(b!=bindings.end()) {
                                auto m=handshake(*b,nonce,control->identity().session,state.graph_revision,state.epoch);
                                m.kind=nd::Kind::Query;m.history.capacity=std::get<signal_contracts::NumericContract>(m.contract).shape.extents[0].maximum;m.stamp.available=true;m.envelope.request=q->request;m.envelope.delivery_sequence=++sequence;
                                if(codec.encode(m,nd::QueryId)==Status::ok){q->sent=true;codec.flush(*control);}
                            }
                        }
                    }
                }
                else if(!ingress.empty()) {
                    auto item=std::move(ingress.front());ingress.pop_front();
                    auto b=std::find_if(bindings.begin(),bindings.end(),[&](const auto& v){return v.context->endpoint==item.endpoint;});
                    if(b!=bindings.end()&&b->acknowledged) {
                        const auto delivery_sequence=++sequence;
                        Status encoded;
                        if(item.history_ordinal)encoded=encode_snapshot(codec,item.frame,*b,nonce,control->identity().session,
                            state.graph_revision,delivery_sequence,delivery_sequence,state.epoch,*item.history_ordinal,item.filled_count,nd::EventsId);
                        else {Queued value=std::move(item.frame);encoded=encode_delivery(codec,value,*b,nonce,control->identity().session,state.graph_revision,delivery_sequence,false,state.epoch);}
                        if(encoded==Status::ok)codec.flush(*control);
                        else warning=QStringLiteral("Remote numeric ingress exceeds DATA4 contract or limits");
                    }
                }
            }
        }
        pumping=false;
    }
};
NumericRemoteEndpoint::NumericRemoteEndpoint(QString host,quint16 port,QObject* parent):QObject(parent),impl_(std::make_unique<Impl>(*this)) {
    impl_->host=std::move(host);impl_->port=port;impl_->tcp=std::make_unique<desktop::QtTcpByteTransport>(impl_->socket,65536);
}
NumericRemoteEndpoint::NumericRemoteEndpoint(desktop::SerialEndpoint endpoint,QObject* parent):QObject(parent),impl_(std::make_unique<Impl>(*this)) {
    impl_->serial_endpoint=std::move(endpoint);impl_->serial=std::make_unique<desktop::QtSerialByteTransport>();
    QObject::connect(&impl_->serial->device(),&QSerialPort::readyRead,this,[this]{impl_->pump();});
    QObject::connect(&impl_->serial->device(),&QSerialPort::errorOccurred,this,[this](auto error) {
        if(error==QSerialPort::ResourceError&&impl_->serial->device().error()==error)disconnect_remote();
    },Qt::QueuedConnection);
}
NumericRemoteEndpoint::~NumericRemoteEndpoint(){disconnect_remote();}
EnginePort::PortDeliveryMode NumericRemoteEndpoint::port_delivery_mode() const noexcept{return PortDeliveryMode::CanonicalNumericPortsV2;}
bool NumericRemoteEndpoint::subscribe_numeric_frames(std::vector<EndpointIdentity> endpoints,QString* error) {
    if(endpoints.size()>MaxBindings){if(error)*error=QStringLiteral("Too many numeric subscriptions");return false;}
    auto& d=*impl_;
    if(d.capabilities&&!d.graph.nodes.empty()) {
        const auto count=endpoints.empty()?default_endpoints(d.graph).size():endpoints.size();
        auto prepared=Arena::prepare_rebind(nd::desktop_limits().frame,count,d.arena_budget,d.arena);
        if(!prepared){if(error)*error=QStringLiteral("Remote numeric retention budget exhausted");return false;}
        d.staged_arena=std::move(prepared);
    }
    ++impl_->frame_handler_generation;++impl_->history_handler_generation;
    impl_->desired=std::move(endpoints);impl_->rebind=true;return true;
}
void NumericRemoteEndpoint::set_numeric_frame_handler(std::function<void(DeliveredNumericFrame)> f){++impl_->frame_handler_generation;impl_->frame_handler=std::move(f);}
void NumericRemoteEndpoint::set_numeric_history_handler(std::function<void(DeliveredNumericHistoryRow)> f){++impl_->history_handler_generation;impl_->history_handler=std::move(f);}
void NumericRemoteEndpoint::set_numeric_history_snapshot_handler(std::function<void(DeliveredNumericFrame,std::uint64_t,std::uint64_t)> f){impl_->history_snapshots.clear();impl_->history_snapshot_handler=std::move(f);}
quint64 NumericRemoteEndpoint::submit(EngineCommand command) {
    auto& d=*impl_;const auto id=d.next_public++;
    if((!d.connected&&d.socket.state()==QAbstractSocket::UnconnectedState)||d.work.size()>=QueueSize) {
        d.emit_reply({id,false,QStringLiteral("Remote disconnected or command queue full"),d.state});return id;
    }
    d.work.push_back({id,std::move(command)});return id;
}
void NumericRemoteEndpoint::set_snapshot_handler(std::function<void(EngineSnapshot)> f){++impl_->snapshot_handler_generation;impl_->snapshot_handler=std::move(f);}
void NumericRemoteEndpoint::set_reply_handler(std::function<void(EngineReply)> f){++impl_->reply_handler_generation;impl_->reply_handler=std::move(f);}
EngineSnapshot NumericRemoteEndpoint::snapshot() const{return impl_->state;}
QString NumericRemoteEndpoint::network_warning() const{return impl_->warning;}
bool NumericRemoteEndpoint::configure_network(QJsonObject config,QString* error) {
    auto& d=*impl_;
    if(d.serial) {
        auto endpoint=*d.serial_endpoint;endpoint.port_name=config.value("port_name").toString(endpoint.port_name);
        endpoint.baud_rate=config.value("baud_rate").toInt(endpoint.baud_rate);
        const auto device=config.value("device_identity").toString(d.device);
        if(endpoint.port_name.isEmpty()||endpoint.baud_rate<=0||device.isEmpty()||device.toUtf8().size()>256) {
            if(error)*error=QStringLiteral("Explicit UART port, baud rate and device identity required");return false;
        }
        const bool changed=endpoint.port_name!=d.serial_endpoint->port_name||endpoint.baud_rate!=d.serial_endpoint->baud_rate||device!=d.device;
        const bool reconnect=d.connected;
        if(changed){disconnect_remote();d.serial_endpoint=endpoint;d.device=device;if(reconnect)connect_remote();}return true;
    }
    const auto host=config.value("host").toString(d.host);const int port=config.value("port").toInt(d.port);
    const auto device=config.value("device_identity").toString(d.device);
    if(port<1||port>65535||host.isEmpty()||device.isEmpty()||device.toUtf8().size()>256) {if(error)*error=QStringLiteral("Explicit TCP host, port and device identity required");return false;}
    const bool changed=host!=d.host||port!=d.port||device!=d.device;
    const bool reconnect=d.connected||d.socket.state()!=QAbstractSocket::UnconnectedState;
    if(changed){disconnect_remote();d.host=host;d.port=quint16(port);d.device=device;if(reconnect)connect_remote();}return true;
}
void NumericRemoteEndpoint::connect_remote() {
    auto& d=*impl_;if(d.retiring||d.connected||d.socket.state()!=QAbstractSocket::UnconnectedState)return;
    if(d.serial) {if(d.serial->open(*d.serial_endpoint)!=Status::ok){d.warning=d.serial->error_string();return;}d.start();}
    else d.socket.connectToHost(d.host,d.port);
}
void NumericRemoteEndpoint::disconnect_remote(){auto& d=*impl_;d.retire();if(d.serial)d.serial->close();else d.socket.abort();}
bool NumericRemoteEndpoint::publish_numeric_frame(const EndpointIdentity& endpoint,DeliveredNumericFrame frame,QString* error) {
    auto& d=*impl_;const auto* p=find_port(d.graph,endpoint);
    if(d.ingress.size()>=QueueSize && p)d.ingress_losses.insert(endpoint);
    if(!d.connected||!p||!frame.context||!frame.frame||d.ingress.size()>=QueueSize||
       !signal_contracts::same_contract(p->contract.view(),frame.context->contract.view())) {
        if(error)*error=QStringLiteral("Remote ingress unavailable, incompatible or full");return false;
    }
    const auto incoming=frame.frame.view();
    if(const auto* payload=std::get_if<signal_contracts::NumericView>(&incoming.payload);payload && payload->ring.capacity) {
        if(error)*error=QStringLiteral("History ingress requires its explicit real ordinal");return false;
    }
    if(d.ingress_losses.erase(endpoint))frame.frame=frame.frame.with_discontinuity();
    d.ingress.push_back({endpoint,std::move(frame),{}});return true;
}
bool NumericRemoteEndpoint::publish_numeric_history(const EndpointIdentity& endpoint,DeliveredNumericFrame frame,std::uint64_t ordinal,std::uint64_t filled_count,QString* error) {
    auto& d=*impl_;const auto* port=find_port(d.graph,endpoint);
    if(d.ingress.size()>=QueueSize && port)d.ingress_losses.insert(endpoint);
    if(!d.connected||!port||!frame.context||!frame.context->retention_owner||!frame.frame||d.ingress.size()>=QueueSize||
       !signal_contracts::same_contract(port->contract.view(),frame.context->contract.view())) {
        if(error)*error=QStringLiteral("Remote History ingress unavailable, incompatible or full");return false;
    }
    const auto c=port->contract.view();const auto* history=std::get_if<signal_contracts::NumericContract>(&c);
    const auto value=frame.frame.view();const auto* payload=std::get_if<signal_contracts::NumericView>(&value.payload);
    if(!history||history->shape.rank!=signal_contracts::Rank::Matrix||!payload|| (value.stamp.available && !payload->ring.capacity)||
       (value.stamp.available&&(filled_count>payload->actual[0] || ordinal<filled_count))) {
        if(error)*error=QStringLiteral("History requires an exact snapshot and its real row ordinal");return false;
    }
    if(d.ingress_losses.erase(endpoint))frame.frame=frame.frame.with_discontinuity();
    d.ingress.push_back({endpoint,std::move(frame),ordinal,filled_count});return true;
}
}
