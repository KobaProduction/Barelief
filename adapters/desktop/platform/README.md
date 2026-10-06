# Desktop platform adapters

Canonical platform headers live in `adapters/desktop/include`, retaining the
existing `core/`, `pipeline/`, `config/`, `aggregator/` and `wled/` public names.
Old `src/*/include` headers only forward to these files. Implementations live in
this directory, with one implementation per class. Target names remain stable;
the composition/build layer supplies their Qt, miniaudio and OS dependencies.

`desktop/qt_byte_transport.h` adapts Qt devices to the portable
`bdsp_protocol::ByteTransport`. `QtTcpByteTransport` references an existing
QTcpSocket; `QtSerialByteTransport` owns a QSerialPort. Neither opens an endpoint
implicitly or discovers ports. All methods run on the device's owning Qt thread,
and no method waits for bytes, connection or write completion.

`QtPacketChannel` uses the same bounded `bdsp_protocol::PacketStream` for TCP and
UART. It retains an outgoing packet's unsent suffix and reports overflow when
another packet cannot be accepted. The Qt write queue has a byte budget too.
Incoming parsing uses a fixed buffer; overflow discards through the next packet
delimiter. Packet signals own their payload. Device close and TCP reconnect fence
parser state and pending writes. Serial errors fence pending writes as well.

The desktop composition root can explicitly create a serial packet session:

```cpp
desktop::QtSerialPacketSession session;
desktop::SerialEndpoint endpoint;
endpoint.port_name = configured_port; // e.g. COM4, never an implicit default
endpoint.baud_rate = configured_baud; // default 115200, 8N1, no flow control
QObject::connect(&session.channel(), &desktop::QtPacketChannel::packetReceived,
                 owner, packet_handler);
const auto status = session.open(endpoint);
// When connected, enqueue the same existing BDSP session/control/data packets:
session.channel().send_packet(packet_id, payload);
// Explicit stop cancels both OS I/O and pending framing state:
session.close();
```

Data bits, parity, stop bits and flow control are explicit endpoint fields. The
caller selects the transport and owns reconnect policy and session handshakes.
This adapter does not rewrite graphs, assign devices or register UART endpoints
with the TCP registry. Existing graph routing and TCP session behavior stay in
the corresponding desktop services.

Device-free verification covers fake UART one-byte input, partial/blocked/failing
output, bounded queues, overflow recovery, session reset, invalid explicit Serial
endpoints and real localhost TCP. Physical COM hardware, capture, WLED hardware
and Linux drivers require separate verification.
