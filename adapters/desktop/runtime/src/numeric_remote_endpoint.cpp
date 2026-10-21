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
                reply.accepted=false;reply.error=QStringLiteral("Remote session retired after command");reply.numeric_diagnostic.reset();reply.snapshot=state;
            } else reply.snapshot=state;
            if(reply_handler)reply_handler(std::move(reply));
        },Qt::QueuedConnection);
    }
    void emit_snapshot() {
        QMetaObject::invokeMethod(&owner,[this,generation=snapshot_generation,handler_generation=snapshot_handler_generation] {
            if(generation==snapshot_generation&&handler_generation==snapshot_handler_generation&&snapshot_handler)snapshot_handler(state);
        },Qt::QueuedConnection);
    }
    void finish(bool accepted,QString error={},std::optional<EngineDiagnostic> diagnostic={}) {
        if(active)emit_reply({active->id,accepted,std::move(error),state,std::move(diagnostic)});
        active.reset();apply_next=false;
        cold_snapshot=cold_selected=false;queries.clear();
    }
    void fence() {bindings.clear();staged_arena.reset();ingress.clear();received.clear();receive_losses.clear();ingress_losses.clear();history_snapshots.clear();queries.clear();cold_selected=false;codec.reset();received_sequence=0;discard_data=true;}
    void retire(QString error={}) {
        if(retiring)return;retiring=true;
        auto retired_bindings=std::move(bindings);
        retire_transport();
        if(!notify_retired_bindings(retired_bindings))return;
        retire_snapshot(std::move(error));
        retiring=false;
    }
    void retire_transport() {
        timer.stop();connected=false;fence();unsent.reset();waiting=0;capabilities=bootstrapped=rebind=false;
        arena.reset();
        if(control)control->reset();control.reset();
    }
    void notify_retired_binding(const Binding& binding,const std::function<void(DeliveredNumericFrame)>& on_frame,const std::function<void(DeliveredNumericHistoryRow)>& on_history,
        std::uint64_t frame_generation,std::uint64_t history_generation) {
            const auto contract=binding.context->contract.view();const auto* history=std::get_if<signal_contracts::NumericContract>(&contract);
            DeliveredNumericFrame unavailable{binding.context,{state.graph_revision,state.epoch},{},Status::unavailable};
            if(history&&binding.ring) {
                DeliveredNumericHistoryRow marker;marker.compact=std::move(unavailable);marker.event=NumericHistoryEvent::Unavailable;
                marker.capacity=history->shape.extents[0].maximum;if(on_history && history_generation==history_handler_generation)on_history(std::move(marker));
            } else if(on_frame && frame_generation==frame_handler_generation)on_frame(std::move(unavailable));
    }
    bool notify_retired_bindings(const std::deque<Binding>& retired_bindings) {
        QPointer<NumericRemoteEndpoint> lifetime(&owner);
        const auto on_frame=frame_handler;const auto on_history=history_handler;
        const auto frame_generation=frame_handler_generation,history_generation=history_handler_generation;
        for(const auto& binding:retired_bindings) {
            notify_retired_binding(binding,on_frame,on_history,frame_generation,history_generation);
            if(!lifetime)return false;
        }
        return true;
    }
    void retire_snapshot(QString error) {
        auto diagnostics=std::move(state.diagnostics);
        ++snapshot_generation;graph={};state={};state.diagnostics=std::move(diagnostics);token=0;nonce=0;
        state.statuses[QStringLiteral("analyzer")]={State::Stopped,error,0};
        emit_snapshot();
        if(active)finish(false,error);
        while(!work.empty()) {auto w=std::move(work.front());work.pop_front();emit_reply({w.id,false,error,state});}
        if(!error.isEmpty())warning=std::move(error);
    }
    void initialize_remote_control() {
        auto config=desktop_session_config();
        config.endpoint_authorized=true;config.device_identity=device.toStdString();config.codec_limits=control_limits();
        control=std::make_unique<control::numeric::ControlSession>(config,clock);
        bind_control_handlers();
    }
    void reset_session_identity() {
        nonce=QRandomGenerator::global()->generate64();if(!nonce)nonce=1;
        next_request=1;next_binding=1;sequence=0;last_state=0;connected=true;
    }
    void start() {
        if(control)retire();fence();warning.clear();state.diagnostics={};
        ++snapshot_generation;
        initialize_remote_control();
        reset_session_identity();
        if(control->open(nonce)!=Status::ok){retire(QStringLiteral("CONTROL2 negotiation failed"));return;}
        timer.start();
    }
    void bind_control_handlers() {
        control->set_reply_handler([](const nc::Message& message,void* context){static_cast<Impl*>(context)->reply(message);},this);
        control->set_packet_handler([](bdsp_protocol::PacketView packet,void* context){static_cast<Impl*>(context)->packet(packet);},this);
    }
    void finish_rejected(const nc::Reply& reply) {
        std::optional<EngineDiagnostic> diagnostic;
        if(reply.diagnostic.reason!=domain::ProcessingReason::None) {
            domain::ProcessingDiagnostic value;
            value.error=domain::ProcessingError::InvalidParameters;
            value.node=reply.diagnostic.node.value_or(-1);
            value.port=reply.diagnostic.port.value_or(0);
            if(reply.diagnostic.edge && *reply.diagnostic.edge<=std::numeric_limits<std::size_t>::max())
                value.edge=std::size_t(*reply.diagnostic.edge);
            value.reason=reply.diagnostic.reason;value.parameter=reply.diagnostic.parameter;
            diagnostic=EngineDiagnostic::own(value);
        }
        finish(false,QStringLiteral("Remote control rejected (%1)").arg(int(reply.error)),std::move(diagnostic));
    }

    bool accept_reply_graph(const nc::Reply& value) {
        const auto* r=&value;
        if(r->state.applied) {
            domain::NumericProcessingGraph next;
            if(!control::numeric::own_graph(*r->state.applied,control_limits(),next)||!project_graph(next,state.graph)) {
                warning=QStringLiteral("Invalid remote canonical graph");finish(false,warning);return false;
            }
            graph=std::move(next);
        } else {graph={};state.graph={};}
        return true;
    }
    void accept_capabilities_reply(const nc::Reply& value) {
        const auto* r=&value;
            capabilities=r->error==nc::Error::None&&r->capabilities&&r->capabilities->control_version==nc::SchemaVersion&&
                r->capabilities->data_version==nd::PayloadVersion&&(r->capabilities->codec_mask&3);
            if(!capabilities)warning=QStringLiteral("Remote numeric CONTROL2/DATA4 unsupported");
            bootstrapped=true;rebind=true;
    }
    void complete_active_command(const nc::Reply& value) {
        const auto* r=&value;
        if(phase==nc::CommandId::Prepare) {token=r->state.token;if(!token){finish(false,QStringLiteral("Missing preparation token"));return;}apply_next=true;}
        else finish(true);
    }
    void complete_control_reply(const nc::Reply& value) {
        const auto* r=&value;
        if(phase==nc::CommandId::Capabilities) {
            accept_capabilities_reply(*r);return;
        }
        if(!active)return;
        if(r->error!=nc::Error::None) {finish_rejected(*r);return;}
        if(phase==nc::CommandId::State&&active->command.kind==EngineCommand::Snapshot) {
            cold_snapshot=true;cold_started=clock.now_ms();cold_selected=false;state.numeric_frames.clear();return;
        }
        complete_active_command(*r);
    }
    void update_reply_state(const nc::Message& m,const nc::Reply& value) {
        const auto* r=&value;
        state.epoch=m.epoch;state.revision=r->state.revision;state.graph_revision=remote_numeric::graph_revision(m);
        state.statuses[QStringLiteral("analyzer")]={r->state.run_intent?State::Running:State::Stopped,{},state.revision};
    }
    void reply(const nc::Message& m) {
        if(m.request!=waiting)return;waiting=0;
        const auto* r=std::get_if<nc::Reply>(&m.body);if(!r)return;
        // Pace observation from completion: a large/slow State reply must not
        // immediately schedule another query and starve DATA binding/transport.
        if(phase==nc::CommandId::State)last_state=clock.now_ms();
        apply_graph_reply(m,*r);
    }
    void apply_graph_reply(const nc::Message& m,const nc::Reply& reply) {
        const auto previous=state.graph_revision;
        update_reply_state(m,reply);
        if(!accept_reply_graph(reply))return;
        if(previous!=state.graph_revision) {fence();rebind=true;}
        emit_snapshot();
        complete_control_reply(reply);
    }
    bool send(nc::Command command,std::uint64_t expected) {
        const auto request=next_request;
        auto result=control->send(command,request,expected);
        if(result==Status::would_block){unsent=command;unsent_expected=expected;return false;}
        if(result!=Status::ok) {warning=QStringLiteral("Remote CONTROL2 send failed");finish(false,warning);return false;}
        unsent.reset();++next_request;waiting=request;phase=command.id;return true;
    }
    void replace_bindings(const std::vector<EndpointIdentity>& selected,std::shared_ptr<Arena> next_arena) {
        std::deque<Binding> next;
        auto next_id=next_binding;
        if(!prepare_bindings(selected,next_arena,next,next_id))return;
        fence();arena=std::move(next_arena);bindings=std::move(next);next_binding=next_id;
    }
    void select_bindings() {
        rebind=false;
        if(!capabilities||graph.nodes.empty())return;
        auto selected=desired.empty()?default_endpoints(graph):desired;
        if(selected.size()>MaxBindings){warning=QStringLiteral("Too many remote numeric ports");return;}
        if(selected.empty()){fence();return;}
        auto next_arena=staged_arena?staged_arena:Arena::prepare_rebind(nd::desktop_limits().frame,selected.size(),arena_budget,arena);
        if(!next_arena){warning=QStringLiteral("Remote numeric retention budget exhausted");rebind=true;return;}
        replace_bindings(selected,std::move(next_arena));
    }
    bool prepare_bindings(const std::vector<EndpointIdentity>& selected,const std::shared_ptr<Arena>& next_arena,
                          std::deque<Binding>& next,std::uint64_t& next_id) {
        for(const auto& e:selected) {
            const auto* p=find_port(graph,e);if(!p){warning=QStringLiteral("Remote numeric port missing");continue;}
            if(std::any_of(next.begin(),next.end(),[&](const auto& b){return b.context->endpoint==e;}))continue;
            Binding binding;
            if(make_binding(e,p->contract,std::uint32_t(next_id++),next_arena,binding)!=Status::ok) {
                warning=QStringLiteral("Remote numeric binding preparation refused");return false;
            }
            next.push_back(std::move(binding));
        }
        return true;
    }
    void dispatch_data_message(std::uint8_t id,nd::Message& m) {
        auto found=std::find_if(bindings.begin(),bindings.end(),[&](const auto& b){return b.context->endpoint==endpoint(m.endpoint);});
        if(found==bindings.end())return;
        if(id==nd::AckId){accept_ack(m,*found);return;}
        if(!found->acknowledged||!m.envelope.delivery_sequence||m.envelope.delivery_sequence<=received_sequence)return;
        if(m.envelope.request)receive_snapshot(m,*found);
        else receive_delivery(m,*found);
    }
    bool decode_data_message(std::span<const std::uint8_t> bytes,nd::Message& m) {
        const auto identity=control->identity();
        const auto decoded=codec.decode(bytes,m,codec.reference_context(bindings,nonce,identity.session,state.graph_revision));if(decoded!=Status::ok){warning=QStringLiteral("Remote numeric decode refused: status=%1").arg(int(decoded));return false;}
        if(m.envelope.nonce!=nonce||m.envelope.session!=identity.session||m.envelope.revision!=state.graph_revision||m.envelope.delivery_epoch!=state.epoch||
           m.envelope.delivery_generation!=m.stamp.generation)return false;
        return true;
    }
    void packet(bdsp_protocol::PacketView p) {
        if(!capabilities||!control)return;
        auto bytes=p.payload;
        if(p.id==nd::FragmentId) {if(codec.assembler.accept(bytes,bytes)!=Status::ok||bytes.empty())return;}
        else if(p.id!=nd::AckId&&p.id!=nd::EventsId&&p.id!=nd::SnapshotId)return;
        nd::Message m;if(!decode_data_message(bytes,m))return;
        dispatch_data_message(p.id,m);
    }
    void accept_ack(const nd::Message& m,Binding& binding) {
        if(m.kind==nd::Kind::Unavailable&&!m.stamp.available&&m.envelope.delivery_sequence==1&&
           !m.envelope.delivery_generation&&m.envelope.request==binding.id&&m.stamp.stream_id==binding.stream&&
           signal_contracts::same_contract(m.contract,binding.wire_owner.view()))binding.acknowledged=true;
    }
    void receive_snapshot(nd::Message& m,Binding& binding) {
        auto query=std::find_if(queries.begin(),queries.end(),[&](const auto& pending){
            return pending.sent&&pending.request==m.envelope.request&&pending.endpoint==binding.context->endpoint;});
        if(query==queries.end())return;
        DeliveredNumericFrame retained;if(retain_snapshot(m,binding,codec,arena,retained)!=Status::ok)return;
        binding.ring=m.history.capacity!=0;
        if(m.kind==nd::Kind::Frame || (m.kind==nd::Kind::Unavailable && !m.history.capacity)){
            Queued ordinary=retained;enqueue_delivery(received,ordinary,receive_losses);
        }else if(history_snapshot_handler&&history_snapshots.size()<QueueSize)
            history_snapshots.push_back({retained,m.history.ordinal,m.history.filled});
        received_sequence=m.envelope.delivery_sequence;
        state.numeric_frames.push_back(std::move(retained));queries.erase(query);
    }
    void receive_delivery(nd::Message& m,Binding& binding) {
        Queued retained;
        if(retain_delivery(m,binding,codec,arena,retained)!=Status::ok){
            receive_losses.insert(binding.context->endpoint);warning=QStringLiteral("Remote numeric delivery refused");return;}
        binding.ring=m.history.capacity!=0;
        received_sequence=m.envelope.delivery_sequence;
        if(enqueue_delivery(received,retained,receive_losses)==EnqueueResult::Overflow)
            warning=QStringLiteral("Remote numeric delivery mailbox overflow");
    }
    void discard_outbound_if_ready() {
        if(discard_data&&control->state()==control::ControlSessionState::Ready)
            discard_data=control->discard_outbound_data()==Status::would_block;
    }
    void pump() {
        if(pumping||!connected||!control)return;pumping=true;
        QPointer<NumericRemoteEndpoint> lifetime(&owner);
        if(!poll_control())return;
        discard_outbound_if_ready();
        if(!drain_received(lifetime)||!drain_history_snapshots(lifetime))return;
        if(control->state()!=control::ControlSessionState::Ready||waiting){pumping=false;return;}
        process_ready();
        if(lifetime)pumping=false;
    }
    bool poll_control() {
        auto* transport=serial?static_cast<bdsp_protocol::ByteTransport*>(serial.get()):tcp.get();
        const auto pending=serial?serial->device().bytesToWrite():socket.bytesToWrite();
        const auto result=control->poll(*transport,pending);
        if(result==Status::ok||result==Status::would_block)return true;
        const QJsonObject diagnostic{{"session_state",int(control->state())},{"session_error",int(control->error())},
                {"status",int(result)},{"command",int(phase)},{"request",QString::number(waiting)},
                {"wire_pending",QString::number(control->pending_bytes())},{"assembly_bytes",QString::number(control->assembly_bytes())},
                {"device_pending",QString::number(pending)},{"receive_buffered",QString::number(serial?serial->device().bytesAvailable():socket.bytesAvailable())}};
        const auto timeout=control->error()==control::ControlSessionError::Timeout;
        state.diagnostics["control_failure"]=diagnostic;
        pumping=false;retire(timeout?QStringLiteral("Remote control timed out"):QStringLiteral("Remote session failed"));
        return false;
    }
    void dispatch_received(Queued value) {
            if(auto* f=std::get_if<DeliveredNumericFrame>(&value)){if(frame_handler)frame_handler(std::move(*f));}
            else if(history_handler)history_handler(std::move(std::get<DeliveredNumericHistoryRow>(value)));
    }
    bool drain_received(QPointer<NumericRemoteEndpoint> lifetime) {
        while(!received.empty()) {
            auto value=std::move(received.front());received.pop_front();
            dispatch_received(std::move(value));
            if(!lifetime)return false;
            if(!control||!connected){pumping=false;return false;}
        }
        return true;
    }
    bool drain_history_snapshots(QPointer<NumericRemoteEndpoint> lifetime) {
        while(!history_snapshots.empty()) {
            auto next=std::move(history_snapshots.front());history_snapshots.pop_front();
            if(next.frame.delivery.epoch==state.epoch&&next.frame.delivery.graph_revision==state.graph_revision&&history_snapshot_handler)
                history_snapshot_handler(std::move(next.frame),next.ordinal,next.filled_count);
            if(!lifetime)return false;
            if(!control||!connected){pumping=false;return false;}
        }
        return true;
    }
    void send_capabilities_request() {
        nc::Command c;c.id=nc::CommandId::Capabilities;send(c,state.revision);
    }
    void send_prepared_apply() {
        nc::Command c;c.id=nc::CommandId::Apply;c.token=token;c.graph_id=projection.wire().graph_id;send(c,state.revision);
    }
    void send_periodic_state() {
            nc::Command command;command.id=nc::CommandId::State;
            if(send(command,state.revision))last_state=clock.now_ms();
    }
    void process_ready() {
        if(unsent) {auto c=*unsent;send(c,unsent_expected);}
        else if(!bootstrapped) send_capabilities_request();
        else if(active&&apply_next) send_prepared_apply();
        else if(!active&&!work.empty())start_work();
        else if(!active&&clock.now_ms()-last_state>=500) {
            send_periodic_state();
        } else if(capabilities)schedule_data();
    }
    static bool has_routing_adapter(const QJsonObject& graph) {
        const auto nodes=graph.value("nodes").toArray();
        for(const auto& value:nodes) {
            const auto node=value.toObject();
            for(const auto& adapter:{node.value("adapter"),node.value("internal-data").toObject().value("adapter")})
                if(!adapter.isUndefined()&&(!adapter.isObject()||!adapter.toObject().isEmpty()))return true;
        }
        return false;
    }
    bool prepare_work_graph(nc::Command& command) {
        if(has_routing_adapter(active->command.graph)) {
            finish(false,QStringLiteral("Remote adapter routing configuration is not supported by CONTROL2"));return false;}
        if(active->command.expected_revision!=std::numeric_limits<quint64>::max()&&
           active->command.expected_revision!=state.graph_revision){
            finish(false,QStringLiteral("Graph revision conflict"));return false;}
        domain::NumericProcessingGraph next;
        const auto decoded=desktop::decode_numeric_graph(
            QJsonDocument(active->command.graph).toJson(QJsonDocument::Compact),next);
        if(!decoded||!projection.assign(next,control_limits())){
            finish(false,decoded?QStringLiteral("Remote graph exceeds CONTROL2 limits"):decoded.error);return false;}
        command.id=nc::CommandId::Prepare;command.graph=projection.wire();return true;
    }
    void start_work() {
        active=std::move(work.front());work.pop_front();nc::Command command;
        const auto expected=state.revision;
        switch(active->command.kind) {
        case EngineCommand::Snapshot:command.id=nc::CommandId::State;break;
        case EngineCommand::ApplyGraph:if(!prepare_work_graph(command))return;break;
        case EngineCommand::Subscribe:rebind=true;finish(true);return;
        default:finish(false,QStringLiteral("Remote canonical command unsupported"));return;
        }
        send(command,expected);
    }
    bool transmit_subscription(Binding& binding,std::uint64_t now) {
        auto* unbound=&binding;
        auto message=handshake(*unbound,nonce,control->identity().session,state.graph_revision,state.epoch);
        if(codec.encode(message,nd::SubscribeId)==Status::ok){
            unbound->sent=true;unbound->sent_at=now;codec.flush(*control);}
        return true;
    }
    bool schedule_binding() {
        auto pending=std::find_if(bindings.begin(),bindings.end(),[](const auto& binding){return !binding.acknowledged&&binding.sent;});
        auto unbound=bindings.end();const auto now=clock.now_ms();
        if(pending!=bindings.end()) {
            if(now-pending->sent_at>=500)unbound=pending;
        } else unbound=std::find_if(bindings.begin(),bindings.end(),[](const auto& binding){return !binding.acknowledged&&!binding.sent;});
        if(unbound==bindings.end())return false;
        return transmit_subscription(*unbound,now);
    }
    void select_cold_queries() {
        if(cold_selected||!std::all_of(bindings.begin(),bindings.end(),[](const auto& binding){return binding.acknowledged;}))return;
        for(const auto& binding:bindings) {
            const auto contract=binding.context->contract.view();
            const auto* history=std::get_if<signal_contracts::NumericContract>(&contract);
            if(history&&history->shape.rank==signal_contracts::Rank::Matrix)
                queries.push_back({binding.context->endpoint,next_query++,false});
        }
        cold_selected=true;
    }
    void send_cold_query() {
        auto query=std::find_if(queries.begin(),queries.end(),[](const auto& item){return !item.sent;});
        if(query==queries.end())return;
        auto binding=std::find_if(bindings.begin(),bindings.end(),[&](const auto& item){return item.context->endpoint==query->endpoint;});
        if(binding==bindings.end())return;
        auto message=handshake(*binding,nonce,control->identity().session,state.graph_revision,state.epoch);
        message.kind=nd::Kind::Query;
        message.history.capacity=std::get<signal_contracts::NumericContract>(message.contract).shape.extents[0].maximum;
        message.stamp.available=true;message.envelope.request=query->request;message.envelope.delivery_sequence=++sequence;
        if(codec.encode(message,nd::QueryId)==Status::ok){query->sent=true;codec.flush(*control);}
    }
    void schedule_cold_snapshot() {
        select_cold_queries();
        if(queries.size()>QueueSize){finish(false,QStringLiteral("Cold snapshot query exceeds bounded queue"));return;}
        if(cold_selected&&queries.empty()){finish(true);return;}
        if(clock.now_ms()-cold_started>=5000){finish(false,QStringLiteral("Cold history snapshot timeout"));return;}
        send_cold_query();
    }
    void send_ingress() {
        auto item=std::move(ingress.front());ingress.pop_front();
        auto binding=std::find_if(bindings.begin(),bindings.end(),[&](const auto& value){return value.context->endpoint==item.endpoint;});
        if(binding==bindings.end()||!binding->acknowledged)return;
        const auto delivery_sequence=++sequence;
        Status encoded;
        if(item.history_ordinal)encoded=encode_snapshot(codec,item.frame,*binding,nonce,control->identity().session,
            state.graph_revision,delivery_sequence,delivery_sequence,state.epoch,*item.history_ordinal,item.filled_count,nd::EventsId);
        else {Queued value=std::move(item.frame);encoded=encode_delivery(codec,value,*binding,nonce,
            control->identity().session,state.graph_revision,delivery_sequence,false,state.epoch);}
        if(encoded==Status::ok)codec.flush(*control);
        else warning=QStringLiteral("Remote numeric ingress exceeds DATA4 contract or limits");
    }
    void schedule_data() {
        if(rebind)select_bindings();
        if(discard_data)return;
        if(codec.wire_bytes){codec.flush(*control);return;}
        if(schedule_binding())return;
        if(cold_snapshot){schedule_cold_snapshot();return;}
        if(!ingress.empty())send_ingress();
    }
    bool reserve_subscription_arena(const std::vector<EndpointIdentity>& endpoints,QString* error) {
        if(capabilities&&!graph.nodes.empty()) {
            const auto count=endpoints.empty()?default_endpoints(graph).size():endpoints.size();
            auto prepared=Arena::prepare_rebind(nd::desktop_limits().frame,count,arena_budget,arena);
            if(!prepared){if(error)*error=QStringLiteral("Remote numeric retention budget exhausted");return false;}
            staged_arena=std::move(prepared);
        }
        return true;
    }
    bool select_subscriptions(std::vector<EndpointIdentity> endpoints,QString* error) {
        if(endpoints.size()>MaxBindings){if(error)*error=QStringLiteral("Too many numeric subscriptions");return false;}
        if(!reserve_subscription_arena(endpoints,error))return false;
        ++frame_handler_generation;++history_handler_generation;
        desired=std::move(endpoints);rebind=true;return true;
    }
    bool reconnect_serial(const desktop::SerialEndpoint& endpoint,const QString& next_device) {
        const bool changed=endpoint.port_name!=serial_endpoint->port_name||
                           endpoint.baud_rate!=serial_endpoint->baud_rate||next_device!=device;
        const bool reconnect=connected;
        if(changed){owner.disconnect_remote();serial_endpoint=endpoint;device=next_device;if(reconnect)owner.connect_remote();}
        return true;
    }
    bool configure_serial(const QJsonObject& config,QString* error) {
        auto endpoint=*serial_endpoint;endpoint.port_name=config.value("port_name").toString(endpoint.port_name);
        endpoint.baud_rate=config.value("baud_rate").toInt(endpoint.baud_rate);
        const auto next_device=config.value("device_identity").toString(device);
        if(endpoint.port_name.isEmpty()||endpoint.baud_rate<=0||next_device.isEmpty()||next_device.toUtf8().size()>256) {
            if(error)*error=QStringLiteral("Explicit UART port, baud rate and device identity required");return false;
        }
        return reconnect_serial(endpoint,next_device);
    }
    bool reconnect_tcp(const QString& next_host,int next_port,const QString& next_device) {
        const bool changed=next_host!=host||next_port!=port||next_device!=device;
        const bool reconnect=connected||socket.state()!=QAbstractSocket::UnconnectedState;
        if(changed){owner.disconnect_remote();host=next_host;port=quint16(next_port);device=next_device;
            if(reconnect)owner.connect_remote();}
        return true;
    }
    bool configure_tcp(const QJsonObject& config,QString* error) {
        const auto next_host=config.value("host").toString(host);const int next_port=config.value("port").toInt(port);
        const auto next_device=config.value("device_identity").toString(device);
        if(next_port<1||next_port>65535||next_host.isEmpty()||next_device.isEmpty()||next_device.toUtf8().size()>256) {
            if(error)*error=QStringLiteral("Explicit TCP host, port and device identity required");return false;}
        return reconnect_tcp(next_host,next_port,next_device);
    }
    bool frame_ingress_available(const EndpointIdentity& endpoint,const DeliveredNumericFrame& frame,
                                 const domain::ProcessingPort* port) {
        if(ingress.size()>=QueueSize && port)ingress_losses.insert(endpoint);
        return connected&&port&&frame.context&&frame.frame&&ingress.size()<QueueSize&&
               signal_contracts::same_contract(port->contract.view(),frame.context->contract.view());
    }
    static bool is_history_payload(const DeliveredNumericFrame& frame) {
        const auto incoming=frame.frame.view();
        const auto* payload=std::get_if<signal_contracts::NumericView>(&incoming.payload);
        return payload&&payload->ring.capacity;
    }
    bool publish_frame(const EndpointIdentity& endpoint,DeliveredNumericFrame frame,QString* error) {
        const auto* port=find_port(graph,endpoint);
        if(!frame_ingress_available(endpoint,frame,port)) {
            if(error)*error=QStringLiteral("Remote ingress unavailable, incompatible or full");return false;
        }
        if(is_history_payload(frame)) {
            if(error)*error=QStringLiteral("History ingress requires its explicit real ordinal");return false;
        }
        if(ingress_losses.erase(endpoint))frame.frame=frame.frame.with_discontinuity();
        ingress.push_back({endpoint,std::move(frame),{}});return true;
    }
    bool publish_history(const EndpointIdentity& endpoint,DeliveredNumericFrame frame,
                         std::uint64_t ordinal,std::uint64_t filled_count,QString* error) {
        const auto* port=find_port(graph,endpoint);
        if(!frame_ingress_available(endpoint,frame,port)||!frame.context->retention_owner) {
            if(error)*error=QStringLiteral("Remote History ingress unavailable, incompatible or full");return false;
        }
        if(!valid_history_snapshot(*port,frame,ordinal,filled_count)) {
            if(error)*error=QStringLiteral("History requires an exact snapshot and its real row ordinal");return false;
        }
        if(ingress_losses.erase(endpoint))frame.frame=frame.frame.with_discontinuity();
        ingress.push_back({endpoint,std::move(frame),ordinal,filled_count});return true;
    }
    static bool valid_history_snapshot(const domain::ProcessingPort& port,const DeliveredNumericFrame& frame,
                                       std::uint64_t ordinal,std::uint64_t filled_count) {
        const auto contract=port.contract.view();
        const auto* history=std::get_if<signal_contracts::NumericContract>(&contract);
        const auto value=frame.frame.view();
        const auto* payload=std::get_if<signal_contracts::NumericView>(&value.payload);
        return history&&history->shape.rank==signal_contracts::Rank::Matrix&&payload&&
               (!value.stamp.available||payload->ring.capacity)&&
               (!value.stamp.available||(filled_count<=payload->actual[0]&&ordinal>=filled_count));
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
    return impl_->select_subscriptions(std::move(endpoints),error);
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
    return impl_->serial?impl_->configure_serial(config,error):impl_->configure_tcp(config,error);
}
void NumericRemoteEndpoint::connect_remote() {
    auto& d=*impl_;if(d.retiring||d.connected||d.socket.state()!=QAbstractSocket::UnconnectedState)return;
    if(d.serial) {if(d.serial->open(*d.serial_endpoint)!=Status::ok){d.warning=d.serial->error_string();return;}d.start();}
    else d.socket.connectToHost(d.host,d.port);
}
void NumericRemoteEndpoint::disconnect_remote(){auto& d=*impl_;d.retire();if(d.serial)d.serial->close();else d.socket.abort();}
bool NumericRemoteEndpoint::publish_numeric_frame(const EndpointIdentity& endpoint,DeliveredNumericFrame frame,QString* error) {
    return impl_->publish_frame(endpoint,std::move(frame),error);
}
bool NumericRemoteEndpoint::publish_numeric_history(const EndpointIdentity& endpoint,DeliveredNumericFrame frame,std::uint64_t ordinal,std::uint64_t filled_count,QString* error) {
    return impl_->publish_history(endpoint,std::move(frame),ordinal,filled_count,error);
}
}
