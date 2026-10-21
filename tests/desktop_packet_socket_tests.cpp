#include <core/network/tcp_packet_socket.h>
#include <BDSP/transceivers.h>
#include <QCoreApplication>
#include <QTcpServer>
#include <QElapsedTimer>
#include <QThread>
#include <iostream>
#include <stdexcept>
static void require(bool value,const char *message){if(!value)throw std::runtime_error(message);}
template<class F> bool wait(F predicate){QElapsedTimer clock;clock.start();while(!predicate()&&clock.elapsed()<2000){QCoreApplication::processEvents();QThread::msleep(1);}return predicate();}
static QByteArray legacy_wire(quint8 id,QByteArray payload){BDSP::COBSZPETransceiver legacy;QByteArray wire;legacy.get_write_stream().set_stream_writer([](uint8_t byte,void *ctx){static_cast<QByteArray *>(ctx)->append(char(byte));},&wire);require(legacy.send_data(id,reinterpret_cast<uint8_t *>(payload.data()),payload.size())==BDSP::SEND_PACKET_OK,"legacy encode");return wire;}
static void protocol_error_cases(TcpBDSPSocket& socket,QTcpSocket* peer,const QByteArray& payload) {
    int checksum_errors=0;QObject checksumWitness;QObject::connect(&socket,&TcpBDSPSocket::on_bdsp_packet_parsing_error,&checksumWitness,[&](BDSP::parse_packet_status_t status,TcpBDSPSocket *){if(status==BDSP::CHECKSUM_ERROR)++checksum_errors;});
    auto corrupt=legacy_wire(13,QByteArray("CRC"));const auto crc_position=corrupt.indexOf("CRC");require(crc_position>=0,"checksum fixture content");corrupt[crc_position]='D';require(peer->write(corrupt)==corrupt.size(),"checksum fixture send");require(wait([&]{return checksum_errors==1;}),"legacy CHECKSUM_ERROR diagnostic retained");
    int version_errors=0;QObject versionWitness;QObject::connect(&socket,&TcpBDSPSocket::on_bdsp_packet_parsing_error,&versionWitness,[&](BDSP::parse_packet_status_t status,TcpBDSPSocket *){if(status==BDSP::UNSUPPORTED_PROTOCOL_ERROR)++version_errors;});
    auto version=legacy_wire(13,QByteArray("VER"));require(uint8_t(version[1])==uint8_t(13|64),"version fixture header");version[1]=char(uint8_t(version[1])|128);require(peer->write(version)==version.size(),"version fixture send");require(wait([&]{return version_errors==1;}),"legacy unsupported version diagnostic retained");
    require(!socket.send_packet(32,payload)&&!socket.send_packet(13,QByteArray(65536,'x'))&&!socket.send_packet(13,{}),"invalid packet bounds");socket.abort();
}
int main(int argc,char **argv){QCoreApplication app(argc,argv);try{
    QTcpServer server;require(server.listen(QHostAddress::LocalHost,0),"listen");TcpBDSPSocket socket;socket.connectToHost(QHostAddress::LocalHost,server.serverPort());require(wait([&]{return server.hasPendingConnections()&&socket.state()==QAbstractSocket::ConnectedState;}),"connect");auto *peer=server.nextPendingConnection();
    QByteArray payload(1024,0);for(int i=0;i<payload.size();++i)payload[i]=char(i%256);auto expected=legacy_wire(13,payload);QByteArray received;
    require(socket.send_packet(13,payload),"send");require(wait([&]{received+=peer->readAll();return received.size()==expected.size();})&&received==expected,"TCP outgoing legacy wire equality");
    int borrowed=0,owned=0;QByteArray delivered;QObject witness;
    QObject::connect(&socket,&TcpBDSPSocket::on_got_packet,&witness,[&](BDSP::packet_context_t &packet,TcpBDSPSocket *source){require(source==&socket&&packet.packet_id==13&&packet.size==payload.size(),"borrowed signal metadata");require(QByteArray(reinterpret_cast<const char *>(packet.data_ptr),packet.size)==payload&&!packet.need_clear,"borrowed signal bytes");++borrowed;});
    QObject::connect(&socket,&TcpBDSPSocket::packetReceived,&witness,[&](quint8 id,QByteArray data){require(id==13,"owned signal id");++owned;delivered=data;});
    for(int i=0;i<expected.size();++i){require(peer->write(expected.mid(i,1))==1,"fragment write");peer->flush();QCoreApplication::processEvents();}
    require(wait([&]{return owned==1;})&&borrowed==1&&delivered==payload,"incoming fragmented legacy wire");
    require(peer->write(expected.left(expected.size()/2))>0,"incomplete old session");peer->flush();QCoreApplication::processEvents();socket.abort();delete peer;socket.connectToHost(QHostAddress::LocalHost,server.serverPort());require(wait([&]{return server.hasPendingConnections()&&socket.state()==QAbstractSocket::ConnectedState;}),"reconnect");peer=server.nextPendingConnection();require(peer->write(expected)==expected.size(),"fresh session write");
    require(wait([&]{return owned==2;})&&borrowed==2,"session reset discards incomplete old packet");
    protocol_error_cases(socket,peer,payload);
    std::cout<<"TcpBDSPSocket legacy wire, both packet signals, fragments, reconnect reset and packet bounds passed\n";
}catch(const std::exception &e){std::cerr<<e.what()<<'\n';return 1;}}
