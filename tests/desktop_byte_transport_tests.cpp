#include <desktop/qt_byte_transport.h>
#include <QCoreApplication>
#include <QTcpServer>
#include <QElapsedTimer>
#include <QThread>
#include <stdexcept>
#include <iostream>
#include <algorithm>
#include <array>
using signal_contracts::Status;
using namespace desktop;
static void require(bool value,const char *message){if(!value)throw std::runtime_error(message);}
template<class F> bool wait(F predicate){QElapsedTimer clock;clock.start();while(!predicate()&&clock.elapsed()<2000){QCoreApplication::processEvents();QThread::msleep(1);}return predicate();}
class FakeUart final : public QIODevice {
public:
    QByteArray received,sent; qint64 chunk=3,pending=0; bool fault=false;
    FakeUart(){open(ReadWrite);}
    bool isSequential() const override{return true;}
    qint64 bytesAvailable() const override{return received.size()+QIODevice::bytesAvailable();}
    qint64 bytesToWrite() const override{return pending;}
    void inject(const QByteArray &bytes){received+=bytes;emit readyRead();}
    void writable(){pending=0;emit bytesWritten(1);}
protected:
    qint64 readData(char *out,qint64 size) override{if(fault)return -1;auto count=std::min(size,qint64(received.size()));std::copy_n(received.data(),count,out);received.remove(0,count);return count;}
    qint64 writeData(const char *bytes,qint64 size) override{if(fault)return -1;auto count=std::min(chunk,size);sent.append(bytes,count);pending+=count;return count;}
};
int main(int argc,char **argv){QCoreApplication app(argc,argv);try{
    FakeUart uart; QtDeviceByteTransport transport(uart,6); QtPacketChannel channel(uart,transport);
    int count=0; QByteArray last;
    QObject::connect(&channel,&QtPacketChannel::packetReceived,[&](quint8 id,QByteArray bytes){require(id==7,"packet id");++count;last=bytes;});
    QByteArray payload(300,'a');payload[5]=0;
    require(channel.send_packet(7,payload)==Status::ok,"accept UART packet");
    require(channel.pending_bytes()>0,"partial UART write retained");
    require(channel.send_packet(7,payload)==Status::overflow,"bounded outgoing overflow");
    for(int i=0;channel.pending_bytes()&&i<1000;++i)uart.writable();
    require(channel.pending_bytes()==0,"partial UART flush completes");
    for(char byte:uart.sent)uart.inject(QByteArray(1,byte));
    require(count==1&&last==payload,"fragmented UART framing roundtrip");
    uart.chunk=0;require(channel.send_packet(7,payload)==Status::ok,"queue blocked write");channel.reset();
    require(channel.pending_bytes()==0,"reconnect drops old queued bytes");
    uart.sent.clear();uart.chunk=3;uart.writable();require(channel.send_packet(7,QByteArray("new"))==Status::ok,"post reset send");
    while(channel.pending_bytes())uart.writable();uart.inject(uart.sent);require(count==2&&last=="new","no stale packet after reconnect");
    std::array<std::uint8_t,12> bytes{};uart.pending=6;require(transport.write(bytes).status==Status::overflow&&transport.overflow_count()>0,"device queue overflow diagnostic");
    uart.close();require(transport.read(bytes).status==Status::disconnected,"closed UART disconnected");
    uart.open(QIODevice::ReadWrite);uart.pending=0;uart.fault=true;
    require(channel.send_packet(7,QByteArray("fault"))==Status::unavailable&&channel.pending_bytes()==0,"failed device write is not reported as accepted");
    uart.received="error";require(transport.read(bytes).status==Status::unavailable,"failed device read diagnostic");uart.fault=false;uart.received.clear();
    FakeUart bounded_uart;QtDeviceByteTransport bounded_io(bounded_uart);QtPacketChannel bounded_channel(bounded_uart,bounded_io,nullptr,16);
    int overflows=0,precise_overflows=0,recovered=0;QObject::connect(&bounded_channel,&QtPacketChannel::protocolError,[&](Status status){if(status==Status::overflow)++overflows;});
    QObject::connect(&bounded_channel,&QtPacketChannel::parsingError,[&](BDSP::parse_packet_status_t status){if(status==BDSP::EXCEEDING_THE_MAXIMUM_PACKET_SIZE_ERROR)++precise_overflows;});
    QObject::connect(&bounded_channel,&QtPacketChannel::packetReceived,[&](quint8,QByteArray packet){require(packet=="new","resynchronized packet content");++recovered;});
    std::array<std::uint8_t,700> wire{};auto encoded=bdsp_protocol::encode_packet(7,{reinterpret_cast<const std::uint8_t *>(payload.data()),std::size_t(payload.size())},wire);
    require(bool(encoded),"encode overflow fixture");bounded_uart.inject(QByteArray(reinterpret_cast<const char *>(wire.data()),encoded.written_bytes));
    require(overflows==1&&precise_overflows==1&&recovered==0&&bounded_channel.overflow_count()==1,"bounded incoming overflow reported");
    encoded=bdsp_protocol::encode_packet(7,{reinterpret_cast<const std::uint8_t *>("new"),3},wire);bounded_uart.inject(QByteArray(reinterpret_cast<const char *>(wire.data()),encoded.written_bytes));require(recovered==1,"overflow resynchronizes on packet delimiter");
    QtSerialByteTransport serial;SerialEndpoint endpoint;require(serial.open(endpoint)==Status::invalid_argument,"serial requires explicit port");
    endpoint.port_name="__invalid_serial_endpoint__";require(serial.open(endpoint)==Status::unavailable&&serial.state()==bdsp_protocol::TransportState::failed,"serial failed open state");serial.close();
    QTcpServer server;require(server.listen(QHostAddress::LocalHost,0),"listen");QTcpSocket client;QtTcpByteTransport tcp(client);client.connectToHost(QHostAddress::LocalHost,server.serverPort());
    require(wait([&]{return server.hasPendingConnections()&&tcp.state()==bdsp_protocol::TransportState::connected;}),"async TCP connect");
    auto *peer=server.nextPendingConnection();QtTcpByteTransport peer_transport(*peer);QtPacketChannel tx(client,tcp),rx(*peer,peer_transport);int tcp_count=0;
    QObject::connect(&rx,&QtPacketChannel::packetReceived,[&](quint8 id,QByteArray bytes){require(id==7&&bytes==payload,"TCP packet content");++tcp_count;});
    require(tx.send_packet(7,payload)==Status::ok&&wait([&]{return tcp_count==1;}),"TCP PacketStream loopback");client.abort();require(wait([&]{return tcp.state()==bdsp_protocol::TransportState::disconnected;}),"TCP disconnect state");
    std::cout<<"Desktop fake UART fragments, bounded partial writes, reset, explicit Serial endpoint and TCP loopback passed\n";
}catch(const std::exception &e){std::cerr<<e.what()<<'\n';return 1;}}
