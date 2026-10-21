#include <control/numeric_control_server.h>
#include <control/numeric_remote_support.h>
#include <pipeline/numeric_engine.h>
#include <QTcpServer>
#include <QTimer>

namespace pipeline {
using namespace remote_numeric;
struct NumericControlServer::Impl {
    struct Peer;
    NumericControlServer& owner;NumericEngine& engine;QTcpServer listener;QTimer timer;Clock clock;
    application::ProcessingCapabilities profile;
    domain::NumericProcessingGraph graph;EngineSnapshot state;
    std::shared_ptr<CapabilitySnapshot> capabilities;
    std::vector<std::unique_ptr<Peer>> peers;
    std::uint64_t tap=0,next_client=0x100000000ULL,stop_generation=0,last_stop_revision=0;bool mutable_clients=true,pumping=false;
    QString error;
    Impl(NumericControlServer& q,NumericEngine& e):owner(q),engine(e),profile(e.capabilities()) {
        timer.setInterval(5);QObject::connect(&timer,&QTimer::timeout,&owner,[this]{pump();});
        QObject::connect(&listener,&QTcpServer::newConnection,&owner,[this]{accept();});
        tap=engine.subscribe_numeric_tap({EndpointIdentity{"",-1,domain::PortDirection::Output,0}},[this](DeliveredNumericFrame f){deliver(Queued{std::move(f)});},
            [this](DeliveredNumericHistoryRow r){deliver(Queued{std::move(r)});});
        refresh();timer.start();
    }
    ~Impl(){timer.stop();listener.close();engine.unsubscribe_numeric_tap(tap);engine.with_control_service([this](auto&){peers.clear();});}
    bool refresh() {
        auto next=engine.snapshot();
        if(capabilities&&next.graph_revision==state.graph_revision) {
            note_stopped_analyzer(next);
            state=std::move(next);return true;
        }
        return refresh_capabilities(std::move(next));
    }
    bool refresh_capabilities(EngineSnapshot next) {
        domain::NumericProcessingGraph candidate;
        if(!next.graph.isEmpty()) {
            const auto result=desktop::decode_numeric_graph(QJsonDocument(next.graph).toJson(QJsonDocument::Compact),candidate);
            if(!result){error=result.error;return false;}
        }
        auto caps=std::make_shared<CapabilitySnapshot>();
        if(caps->assign(candidate,profile)!=Status::ok){error=QStringLiteral("Actual numeric capabilities unavailable");return false;}
        // Projection storage remains pinned by each peer until a checked replacement.
        graph=std::move(candidate);state=std::move(next);capabilities=std::move(caps);return true;
    }
    void note_stopped_analyzer(const EngineSnapshot& next) {
        const bool was_running=state.statuses.value(QStringLiteral("analyzer")).state==State::Running;
        const bool running=next.statuses.value(QStringLiteral("analyzer")).state==State::Running;
        if(was_running&&!running&&last_stop_revision!=next.revision){++stop_generation;last_stop_revision=next.revision;}
    }
    struct Peer {
        Impl& server;std::unique_ptr<QTcpSocket> socket;
        std::unique_ptr<desktop::QtTcpByteTransport> tcp;
        std::unique_ptr<desktop::QtSerialByteTransport> serial;
        std::shared_ptr<CapabilitySnapshot> capabilities;
        std::unique_ptr<control::numeric::ControlSession> control;
        CodecStorage codec;std::shared_ptr<Arena> arena=std::make_shared<Arena>(nd::desktop_limits().frame);
        std::deque<Binding> bindings;std::deque<std::uint32_t> acknowledgements;
        struct Ingress {domain::NodeId node;DeliveredNumericFrame frame;std::optional<std::uint64_t> history_ordinal;std::uint64_t filled_count=0;};
        std::deque<Queued> outbound;std::deque<Ingress> inbound;
        std::deque<std::pair<EndpointIdentity,std::uint64_t>> queries;
        std::uint64_t revision=0,stop=0,sequence=0,received_sequence=0,reply_check=0;
        DeliveryLosses outbound_losses,inbound_losses;
        bool failed=false,gap=false,writable=true;
        Peer(Impl& s):server(s),capabilities(s.capabilities),revision(s.state.graph_revision),stop(s.stop_generation) {}
        ~Peer(){if(reply_check)server.engine.unregister_control_reply_check(reply_check);}
        void initialize(application::ProcessingControlService& service,bool writable) {
            reply_check=server.engine.register_control_reply_check([pinned=capabilities](const domain::NumericProcessingGraph& graph){
                return bool(control::numeric::reply_envelope_fits(graph,control_limits(),pinned->value));
            });
            if(!reply_check){failed=true;return;}
            this->writable=writable;
            auto config=session_config(writable);
            control=std::make_unique<control::numeric::ControlSession>(config,server.clock,&service);
            control->set_packet_handler([](bdsp_protocol::PacketView p,void* c){static_cast<Peer*>(c)->packet(p);},this);
            control->set_command_commit_handler([](const application::ProcessingReply& reply,void* context){static_cast<Peer*>(context)->server.engine.synchronize_control_state(reply);},this);
            if(control->state()==control::ControlSessionState::Failed)failed=true;
        }
        control::numeric::SessionConfig session_config(bool writable) {
            auto config=desktop_session_config();config.endpoint_authorized=true;config.allow_mutations=writable;
            config.device_identity="numeric-desktop";config.client_identity=server.next_client++;
            config.application_capabilities=&server.profile;config.advertised_capabilities=&capabilities->value;
            config.codec_limits=control_limits();config.runtime_preflights_reply_envelope=true;
            return config;
        }
        void fence() {
            bindings.clear();acknowledgements.clear();outbound.clear();outbound_losses.clear();inbound.clear();inbound_losses.clear();queries.clear();codec.reset();sequence=received_sequence=0;gap=false;
            if(control)control->discard_outbound_data();
        }
        bdsp_protocol::ByteTransport& transport(){return serial?static_cast<bdsp_protocol::ByteTransport&>(*serial):*tcp;}
        qint64 pending(){return serial?serial->device().bytesToWrite():socket->bytesToWrite();}
        void synchronize() {
            if(!control)return;
            if(revision!=server.state.graph_revision) {fence();revision=server.state.graph_revision;}
            if(stop!=server.stop_generation) {
                outbound.clear();outbound_losses.clear();inbound.clear();inbound_losses.clear();queries.clear();codec.reset();control->discard_outbound_data();stop=server.stop_generation;
            }
            // Established session capabilities stay pinned; its registered reply check
            // admits every new graph against this snapshot. New peers use fresh capabilities.
        }
        bool valid_data_envelope(const nd::Message& m) const {
            const auto identity=control->identity();
            if(m.envelope.nonce!=control->nonce()||m.envelope.session!=identity.session||m.envelope.revision!=revision||
               m.envelope.delivery_epoch!=identity.epoch||m.envelope.delivery_generation!=m.stamp.generation)return false;
            return true;
        }
        bool accept_data_payload(bdsp_protocol::PacketView p,std::span<const std::uint8_t>& bytes) {
            if(p.id==nd::FragmentId)return codec.assembler.accept(bytes,bytes)==Status::ok&&!bytes.empty();
            return p.id==nd::SubscribeId||p.id==nd::EventsId||p.id==nd::QueryId;
        }
        void packet(bdsp_protocol::PacketView p) {
            auto bytes=p.payload;
            if(!accept_data_payload(p,bytes))return;
            nd::Message m;const auto identity=control->identity();
            if(codec.decode(bytes,m,codec.reference_context(bindings,control->nonce(),identity.session,revision))!=Status::ok)return;
            if(!valid_data_envelope(m))return;
            const auto e=endpoint(m.endpoint);
            if(p.id==nd::SubscribeId){subscribe(m,e);return;}
            dispatch_bound_packet(p.id,m,e);
        }
        void dispatch_bound_packet(std::uint8_t id,nd::Message& m,const EndpointIdentity& e) {
            auto b=std::find_if(bindings.begin(),bindings.end(),[&](const auto& v){return v.context->endpoint==e&&v.acknowledged;});
            if(b==bindings.end())return;
            if(id==nd::QueryId){query(m,e,*b);return;}
            receive_input(m,e,*b);
        }
        void acknowledge_existing_subscription(const nd::Message& m,const EndpointIdentity& e,const Binding& old) {
                if(old.context->endpoint==e&&old.stream==m.stamp.stream_id&&
                   signal_contracts::same_contract(old.wire_owner.view(),m.contract))acknowledgements.push_back(old.id);
        }
        void install_subscription(Binding b,std::deque<Binding>::iterator old) {
            b.acknowledged=true;
            if(old!=bindings.end()){*old=std::move(b);acknowledgements.push_back(old->id);}
            else {bindings.push_back(std::move(b));acknowledgements.push_back(bindings.back().id);}
            server.update_demand();
        }
        void complete_subscription(const nd::Message& m,const EndpointIdentity& e,const domain::ProcessingPort& port,std::deque<Binding>::iterator old) {
            Binding b;if(make_binding(e,port.contract,std::uint32_t(m.envelope.request),arena,b)!=Status::ok||
                !signal_contracts::same_contract(b.wire_owner.view(),m.contract)||b.stream!=m.stamp.stream_id)return;
            if(!history_subscription_matches(m,port.contract.view()))return;
            install_subscription(std::move(b),old);
        }
        void subscribe(const nd::Message& m,const EndpointIdentity& e) {
            if(m.kind!=nd::Kind::Unavailable||m.stamp.available||!m.envelope.request||m.envelope.request>UINT32_MAX||
               m.envelope.delivery_sequence!=1||m.envelope.delivery_generation||acknowledgements.size()>=QueueSize)return;
            const auto* port=find_port(server.graph,e);if(!port)return;
            auto old=std::find_if(bindings.begin(),bindings.end(),[&](const auto& b){return b.context->endpoint==e||b.id==m.envelope.request;});
            if(old!=bindings.end()&&old->id==m.envelope.request) {
                acknowledge_existing_subscription(m,e,*old);
                return;
            }
            if(old==bindings.end()&&bindings.size()>=MaxBindings)return;
            complete_subscription(m,e,*port,old);
        }
        bool history_subscription_matches(const nd::Message& m,const signal_contracts::DataContract& c) const {
            const auto* history=std::get_if<signal_contracts::NumericContract>(&c);
            return !m.history.capacity || (history && history->shape.rank==signal_contracts::Rank::Matrix && m.history.capacity==history->shape.extents[0].maximum);
        }
        void query(const nd::Message& m,const EndpointIdentity& e,const Binding& b) {
            const auto c=b.context->contract.view();const auto* h=std::get_if<signal_contracts::NumericContract>(&c);
            if(m.kind==nd::Kind::Query&&m.envelope.request&&m.envelope.delivery_sequence>received_sequence&&
               h&&h->shape.rank==signal_contracts::Rank::Matrix&&
               signal_contracts::same_contract(m.contract,b.wire_owner.view())&&queries.size()<QueueSize) {
                queries.emplace_back(e,m.envelope.request);received_sequence=m.envelope.delivery_sequence;
            }
        }
        void receive_input(nd::Message& m,const EndpointIdentity& e,const Binding& b) {
            if(!writable)return;
            if(!m.envelope.delivery_sequence||m.envelope.delivery_sequence<=received_sequence)return;
            if(inbound.size()>=QueueSize){inbound_losses.insert(e);received_sequence=m.envelope.delivery_sequence;return;}
            const auto n=std::find_if(server.graph.nodes.begin(),server.graph.nodes.end(),[&](const auto& n){return n.id==e.node;});
            if(n==server.graph.nodes.end()||n->kind!="NetworkInput"||e.direction!=domain::PortDirection::Output||e.port!=domain::processing_output)return;
            if(!retain_input(m,e,b))return;
            received_sequence=m.envelope.delivery_sequence;
        }
        bool retain_input_snapshot(nd::Message& m,const EndpointIdentity& e,const Binding& b) {
                if(!m.envelope.request)return false;
                DeliveredNumericFrame retained;if(retain_snapshot(m,b,codec,arena,retained)!=Status::ok||!retained.frame)return false;
                if(inbound_losses.erase(e))retained.frame=retained.frame.with_discontinuity();
                inbound.push_back({e.node,std::move(retained),m.history.ordinal,m.history.filled});
            return true;
        }
        bool retain_input_frame(nd::Message& m,const EndpointIdentity& e,const Binding& b) {
                if(m.envelope.request)return false;
                Queued retained;if(retain_delivery(m,b,codec,arena,retained)!=Status::ok)return false;
                auto* frame=std::get_if<DeliveredNumericFrame>(&retained);if(!frame||!frame->frame){inbound_losses.insert(e);return false;}
                if(inbound_losses.erase(e))frame->frame=frame->frame.with_discontinuity();
                inbound.push_back({e.node,*frame,{}});
            return true;
        }
        bool retain_input(nd::Message& m,const EndpointIdentity& e,const Binding& b) {
            return m.kind==nd::Kind::Snapshot || (m.kind==nd::Kind::Unavailable && m.history.capacity)
                ?retain_input_snapshot(m,e,b):retain_input_frame(m,e,b);
        }
        void enqueue(const Queued& value) {
            const auto& f=frame(value);if(!f.context||f.delivery.graph_revision!=revision)return;
            auto found=std::find_if(bindings.begin(),bindings.end(),[&](const auto& b){return b.acknowledged&&b.context->endpoint==f.context->endpoint;});
            if(found==bindings.end())return;
            enqueue_delivery(outbound,value,outbound_losses);
        }
        void transmit() {
            if(failed||control->state()!=control::ControlSessionState::Ready)return;
            if(codec.wire_bytes){flush_wire();return;}
            if(!acknowledgements.empty()){transmit_acknowledgement();return;}
            if(!queries.empty()){transmit_query();return;}
            if(!outbound.empty())transmit_delivery();
        }
        void flush_wire() {
            const auto result=codec.flush(*control);
            if(result!=Status::ok&&result!=Status::would_block)failed=true;
        }
        void transmit_acknowledgement() {
            const auto id=acknowledgements.front();acknowledgements.pop_front();
            auto b=std::find_if(bindings.begin(),bindings.end(),[&](const auto& b){return b.id==id;});
            if(b==bindings.end())return;
            auto m=handshake(*b,control->nonce(),control->identity().session,revision,control->identity().epoch);
            if(codec.encode(m,nd::AckId)!=Status::ok)failed=true;else codec.flush(*control);
        }
        void transmit_query() {
            auto query=std::move(queries.front());queries.pop_front();
            auto b=std::find_if(bindings.begin(),bindings.end(),[&](const auto& b){return b.context->endpoint==query.first;});
            if(b==bindings.end())return;
            DeliveredNumericFrame snapshot;std::uint64_t ordinal=0,filled_count=0;bool explicit_history=false;
            const auto status=server.engine.query_numeric(query.first,snapshot,&ordinal,&filled_count,&explicit_history);
            if(status!=Status::ok)snapshot.status=status;
            if(encode_snapshot(codec,snapshot,*b,control->nonce(),control->identity().session,revision,++sequence,
                query.second,control->identity().epoch,ordinal,filled_count,nd::SnapshotId,explicit_history)==Status::ok)codec.flush(*control);
        }
        void transmit_delivery() {
            auto value=std::move(outbound.front());outbound.pop_front();const auto& f=frame(value);
            auto b=std::find_if(bindings.begin(),bindings.end(),[&](const auto& b){return b.context->endpoint==f.context->endpoint;});
            if(b==bindings.end())return;
            const auto result=encode_delivery(codec,value,*b,control->nonce(),control->identity().session,revision,++sequence,gap);
            if(result==Status::ok){gap=false;codec.flush(*control);}
            else {gap=true;server.error=QStringLiteral("Numeric DATA encode refused: status=%1 node=%2").arg(int(result)).arg(f.context->endpoint.node);}
        }
    };
    void update_demand() {
        std::vector<EndpointIdentity> endpoints;
        for(const auto& peer:peers)if(!peer->failed)for(const auto& binding:peer->bindings)if(binding.acknowledged)endpoints.push_back(binding.context->endpoint);
        std::sort(endpoints.begin(),endpoints.end());endpoints.erase(std::unique(endpoints.begin(),endpoints.end()),endpoints.end());
        engine.update_numeric_tap(tap,std::move(endpoints));
    }
    void deliver(const Queued& value){for(auto& peer:peers)if(!peer->failed)peer->enqueue(value);}
    void accept() {
        while(listener.hasPendingConnections()) {
            std::unique_ptr<QTcpSocket> socket(listener.nextPendingConnection());
            if(peers.size()>=ControlMaximumPeers||!capabilities){socket->abort();continue;}
            socket->setParent(nullptr);auto peer=std::make_unique<Peer>(*this);peer->socket=std::move(socket);
            peer->tcp=std::make_unique<desktop::QtTcpByteTransport>(*peer->socket,65536);
            engine.with_control_service([&](auto& service){peer->initialize(service,mutable_clients);});
            peers.push_back(std::move(peer));
        }
    }
    void poll_peers() {
        engine.with_control_service([&](auto&){for(auto& p:peers)poll_peer(*p);});
    }
    void service_peers() {for(auto& p:peers)service_peer(*p);}
    void pump() {
        if(pumping)return;pumping=true;
        if(!refresh()){pumping=false;return;}
        if(peers.empty()){update_demand();pumping=false;return;}
        poll_peers();
        // Service projection and input feeding remain outside the shared control lock.
        if(refresh())service_peers();
        remove_failed_peers();
        update_demand();pumping=false;
    }
    void remove_failed_peers() {
        if(std::any_of(peers.begin(),peers.end(),[](const auto& p){return p->failed;}))
            engine.with_control_service([&](auto&){std::erase_if(peers,[](const auto& p){return p->failed;});});
    }
    void fail_peer(Peer& peer,Status result) {
        error=QStringLiteral("CONTROL peer failed: status=%1 state=%2 error=%3 assembly=%4 pending=%5")
            .arg(int(result)).arg(int(peer.control->state())).arg(int(peer.control->error()))
            .arg(qulonglong(peer.control->assembly_bytes())).arg(qulonglong(peer.control->pending_bytes()));
        peer.failed=true;
    }
    void poll_peer(Peer& peer) {
        peer.synchronize();if(peer.failed)return;
        const auto result=peer.control->poll(peer.transport(),peer.pending());
        if(result!=Status::ok&&result!=Status::would_block)fail_peer(peer,result);
    }
    void feed_peer_input(Peer& peer) {
        while(!peer.inbound.empty()) {
            auto value=std::move(peer.inbound.front());peer.inbound.pop_front();
            if(value.history_ordinal)
                engine.feed_numeric_owned(value.node,value.frame.frame,value.history_ordinal,
                                          value.frame.context->retention_owner,value.filled_count);
            else engine.feed_numeric(value.node,value.frame.frame.view());
        }
    }
    void service_peer(Peer& peer) {
        peer.synchronize();if(peer.failed)return;
        feed_peer_input(peer);
        engine.with_control_service([&](auto&){drain_peer(peer);});
    }
    bool refresh_peer(Peer& peer) {
        if(!refresh()){peer.failed=true;return false;}
        peer.synchronize();return true;
    }
    bool peer_idle(const Peer& peer,std::size_t queued,std::size_t offset,std::size_t pending) const {
        if(queued==peer.outbound.size()+peer.acknowledgements.size()+peer.queries.size() &&
            offset==peer.codec.wire_offset && pending==peer.control->pending_bytes())return true;
        return peer.outbound.empty()&&peer.acknowledgements.empty()&&peer.queries.empty()&&
            !peer.codec.wire_bytes&&!peer.control->pending_bytes();
    }
    void drain_peer(Peer& peer) {
        for(unsigned step=0;step<64 && !peer.failed;++step){
            const auto queued=peer.outbound.size()+peer.acknowledgements.size()+peer.queries.size();
            const auto offset=peer.codec.wire_offset,pending=peer.control->pending_bytes();
            peer.transmit();const auto io=peer.control->poll(peer.transport(),peer.pending());
            if(io!=Status::ok&&io!=Status::would_block){fail_peer(peer,io);break;}
            if(!refresh_peer(peer))break;
            if(peer_idle(peer,queued,offset,pending))break;
        }
    }
};
NumericControlServer::NumericControlServer(NumericEngine& e,QObject* parent):QObject(parent),impl_(std::make_unique<Impl>(*this,e)){}
NumericControlServer::~NumericControlServer()=default;
bool NumericControlServer::listen(QHostAddress address,quint16 port,bool writable) {
    auto& d=*impl_;d.listener.close();d.mutable_clients=writable;
    if(!d.listener.listen(address,port)){d.error=d.listener.errorString();return false;}return true;
}
bool NumericControlServer::open_serial(const desktop::SerialEndpoint& endpoint,bool writable) {
    auto& d=*impl_;if(d.peers.size()>=ControlMaximumPeers||!d.capabilities)return false;
    auto peer=std::make_unique<Impl::Peer>(d);peer->serial=std::make_unique<desktop::QtSerialByteTransport>();
    if(peer->serial->open(endpoint)!=Status::ok){d.error=peer->serial->error_string();return false;}
    d.engine.with_control_service([&](auto& service){peer->initialize(service,writable);});
    if(peer->failed){d.engine.with_control_service([&](auto&){peer.reset();});d.error=QStringLiteral("Invalid numeric control server profile");return false;}
    d.peers.push_back(std::move(peer));return true;
}
void NumericControlServer::close(){auto& d=*impl_;d.listener.close();d.engine.with_control_service([&](auto&){d.peers.clear();});d.update_demand();}
quint16 NumericControlServer::port() const{return impl_->listener.serverPort();}
QString NumericControlServer::error_string() const{return impl_->error;}
}
