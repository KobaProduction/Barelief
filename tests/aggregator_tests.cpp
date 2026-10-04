#include "aggregator/aggregator_client.h"
#include "aggregator/aggregator_server.h"
#include "aggregator/packet_codec.h"
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QtTest/QTest>
#include <QDebug>
#include <stdexcept>
static void require(bool value,const char *message) { if (!value) throw std::runtime_error(message); }
template<class Predicate> bool wait_for(Predicate predicate,int timeout=2500) {
    QElapsedTimer timer; timer.start();
    while (!predicate() && timer.elapsed()<timeout) QTest::qWait(5);
    return predicate();
}
int main(int argc,char **argv) {
    QCoreApplication app(argc,argv);
    try {
        const auto id=QUuid("00112233-4455-6677-8899-aabbccddeeff");
        registration_request_packet_t registration{id.toBytes(),ComponentType::core,0x1234,0xabcd};
        const auto wire=aggregator_wire::encode(registration);
        require(wire.toHex()=="00112233445566778899aabbccddeeff043412cdab","Registration wire layout changed");
        registration_request_packet_t decoded{};
        require(aggregator_wire::decode(wire,decoded) && decoded.component_server_port==0x1234 && decoded.session_token==0xabcd,"Registration roundtrip failed");
        require(!aggregator_wire::decode(wire.left(20),decoded),"Truncated packet accepted");
        auto invalid=wire; invalid[16]=char(255);
        require(!aggregator_wire::decode(invalid,decoded),"Invalid component type accepted");
        component_update_full_packet_t full{id.toBytes(),ComponentType::core,{},0x1234,ComponentState::active};
        full.address=QHostAddress(QHostAddress::LocalHost).toIPv6Address();
        component_update_full_packet_t full_decoded{};
        require(aggregator_wire::decode(aggregator_wire::encode(full),full_decoded) && full_decoded.component_server_port==0x1234,"Registry snapshot codec failed");
        AggregatorServer server; require(server.start(0,500),"Server start failed");
        const auto port=server.serverPort();
        AggregatorClientConfig first_cfg{QUuid::createUuid(),ComponentType::data_source,10001};
        AggregatorClientConfig second_cfg{QUuid::createUuid(),ComponentType::core,10002};
        AggregatorClient first(first_cfg),second(second_cfg);
        bool first_saw_second=false,second_saw_first=false;
        QObject::connect(&first,&AggregatorClient::on_update_component_info,&app,[&](const QUuid &id,const component_info_t &info){ if(id==second_cfg.uuid && info.state==ComponentState::registered) first_saw_second=true; });
        QObject::connect(&second,&AggregatorClient::on_update_component_info,&app,[&](const QUuid &id,const component_info_t &info){ if(id==first_cfg.uuid && info.state==ComponentState::registered) second_saw_first=true; });
        require(first.connect_to_aggregator_server(QHostAddress::LocalHost,port),"Connect request rejected");
        require(wait_for([&]{return first.is_registered();}),"First registration failed");
        second.connect_to_aggregator_server(QHostAddress::LocalHost,port);
        require(wait_for([&]{return second.is_registered()&&first_saw_second&&second_saw_first;}),"New client lacks registry snapshot or broadcast");
        require(first.runtime_state()==core::RuntimeState::Running,"Registration not mapped to runtime state");
        AggregatorClient impostor(first_cfg); impostor.set_reconnect_mode(false,0);
        impostor.connect_to_aggregator_server(QHostAddress::LocalHost,port);
        require(wait_for([&]{return impostor.runtime_state()==core::RuntimeState::Failed;}),"Duplicate identity not rejected");
        require(impostor.error().contains("Registration rejected")&&!impostor.is_registered(),"Missing explicit registration error");
        server.stop();
        require(wait_for([&]{return !first.is_registered()&&!second.is_registered();}),"Disconnect did not clear registration");
        require(server.start(port,500),"Server restart failed");
        require(wait_for([&]{return first.is_registered()&&second.is_registered();}),"Clients did not reconnect and register");
        second.disconnect_from_aggregator_server();
        QTest::qWait(300);
        require(second.runtime_state()==core::RuntimeState::Stopped&&!second.is_connected(),"Cancelled client reconnected");
        TcpBDSPSocket malformed; bool explicit_error=false;
        QObject::connect(&malformed,&TcpBDSPSocket::on_got_packet,&app,[&](BDSP::packet_context_t &packet){ if(packet.packet_id==static_cast<uint8_t>(PacketType::error)) explicit_error=true; });
        malformed.connectToHost(QHostAddress::LocalHost,port);
        require(wait_for([&]{return malformed.state()==QAbstractSocket::ConnectedState;}),"Raw socket connect failed");
        uint8_t byte=0; malformed.send_data(static_cast<uint8_t>(PacketType::component_registration),&byte,1);
        require(wait_for([&]{return explicit_error;}),"Malformed registration has no error response");
        QTcpServer silent; require(silent.listen(QHostAddress::LocalHost,0),"Silent server start failed");
        AggregatorClient timeout({QUuid::createUuid(),ComponentType::core,0}); timeout.set_reconnect_mode(false,0);
        timeout.connect_to_aggregator_server(QHostAddress::LocalHost,silent.serverPort());
        require(wait_for([&]{return timeout.runtime_state()==core::RuntimeState::Failed;}),"Registration timeout never expires");
        require(timeout.error().contains("Registration timeout"),"Registration timeout not reported");
        AggregatorClient cancelled({QUuid::createUuid(),ComponentType::core,0});
        QElapsedTimer begin;begin.start();cancelled.connect_to_aggregator_server(QHostAddress::LocalHost,1,100);
        require(begin.elapsed()<50,"Connection blocks caller");
        QTest::qWait(100);cancelled.disconnect_from_aggregator_server();QTest::qWait(300);
        require(cancelled.runtime_state()==core::RuntimeState::Stopped,"Retry cancellation failed");
        first.disconnect_from_aggregator_server(); server.stop();
        // Fill real socket output buffers without pumping: deterministic slow-peer
        // backpressure, without relying on OS TCP window size or timing.
        AggregatorServer overflow_server;
        require(overflow_server.start(0,500),"Overflow server start failed");
        AggregatorClientConfig healthy_cfg{QUuid::createUuid(),ComponentType::core,10003};
        AggregatorClient healthy(healthy_cfg);
        healthy.set_reconnect_mode(false,0);
        int disconnected_events=0,active_events=0;
        QList<TcpBDSPSocket *> accepted;
        QObject::connect(&overflow_server,&TcpBDSPServer::tcp_bdsp_socket_ready,&app,
                         [&](TcpBDSPSocket *socket){accepted.append(socket);});
        healthy.connect_to_aggregator_server(QHostAddress::LocalHost,overflow_server.serverPort());
        require(wait_for([&]{return healthy.is_registered();}),"Healthy overflow peer registration failed");
        const auto slow_id=QUuid::createUuid(),other_slow_id=QUuid::createUuid();
        TcpBDSPSocket slow,other_slow;
        auto register_slow=[&](TcpBDSPSocket &socket,const QUuid &uuid){
            socket.connectToHost(QHostAddress::LocalHost,overflow_server.serverPort());
            require(wait_for([&]{return socket.state()==QAbstractSocket::ConnectedState;}),"Slow peer connection failed");
            socket.send_packet(static_cast<quint8>(PacketType::component_registration),
                               aggregator_wire::encode(registration_request_packet_t{uuid.toBytes(),ComponentType::data_sink,0,1}));
            bool observed=false;
            auto connection=QObject::connect(&healthy,&AggregatorClient::on_update_component_info,&app,
                [&](const QUuid &id,const component_info_t &info){if(id==uuid&&info.state==ComponentState::registered)observed=true;});
            require(wait_for([&]{return observed;}),"Slow peer registration failed");
            QObject::disconnect(connection);
        };
        register_slow(slow,slow_id); register_slow(other_slow,other_slow_id);
        QObject::connect(&healthy,&AggregatorClient::on_update_component_state,&app,
            [&](const QUuid &id,ComponentState state){
                if ((id==slow_id||id==other_slow_id)&&state==ComponentState::disconnected) ++disconnected_events;
            });
        // The public client intentionally ignores its own state updates.
        auto *healthy_receiver=healthy.findChild<TcpBDSPSocket *>();
        require(healthy_receiver!=nullptr,"Healthy client socket missing");
        QObject::connect(healthy_receiver,&TcpBDSPSocket::packetReceived,&app,[&](quint8 id,const QByteArray &bytes){
            component_update_state_packet_t update{};
            if(id==static_cast<quint8>(PacketType::component_state_change)&&aggregator_wire::decode(bytes,update)&&
               QUuid::fromBytes(&update.uuid)==healthy_cfg.uuid&&update.new_state==ComponentState::active) ++active_events;
        });
        TcpBDSPSocket *healthy_socket=nullptr;
        for (auto *socket:accepted) {
            if(socket->peerPort()==slow.localPort()||socket->peerPort()==other_slow.localPort()) {
                require(socket->write(QByteArray(3*1024*1024,'x'))==3*1024*1024,"Cannot fill slow peer queue");
                require(socket->bytesToWrite()>2*1024*1024+65536,"Slow peer queue did not reach overflow");
            } else healthy_socket=socket;
        }
        require(healthy_socket!=nullptr,"Healthy server socket missing");
        bool healthy_disconnected=false;
        QObject::connect(healthy_socket,&QTcpSocket::disconnected,&app,[&]{healthy_disconnected=true;});
        component_update_state_packet_t state{healthy_cfg.uuid.toBytes(),ComponentState::active};
        auto state_bytes=aggregator_wire::encode(state);
        BDSP::packet_context_t context{};
        context.packet_id=static_cast<uint8_t>(PacketType::component_state_change);
        context.data_ptr=reinterpret_cast<uint8_t *>(state_bytes.data()); context.size=state_bytes.size();
        healthy_socket->on_got_packet(context,healthy_socket);
        require(wait_for([&]{return disconnected_events==2&&active_events==1;}),"Overflow lost or duplicated broadcast events");
        require(!healthy_disconnected&&healthy.is_registered()&&healthy.is_connected(),"Overflow retired the healthy peer");
        QTest::qWait(30);
        require(disconnected_events==2,"Overflow repeated disconnect notifications");
        healthy.disconnect_from_aggregator_server(); overflow_server.stop();
        qInfo("Aggregator async registration, snapshot, refusal, timeout, reconnect and cancellation passed");
    } catch(const std::exception &error) { qCritical("Aggregator test: %s",error.what());return 1; }
    return 0;
}
