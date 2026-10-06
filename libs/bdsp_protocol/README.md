# Portable BDSP protocol

The C++20 target depends on signal_contracts and the existing pinned BDSP library
(`2c68056dfaeffba2f34fc64eee9bc9e331e83c7c`). It has no Qt, OS transport or DSP
dependency. Encode/decode use caller spans and explicit status/size results,
without mandatory exceptions or RTTI. Desktop pipeline allocating wrappers are
temporary compatibility adapters and still accept the historical wire contracts.
Signal semantic validation is separate from wire validation: new runtime ports
must use signal_contracts::validate/compatible.

stream_codec preserves the 112-byte v2 header, little-endian float bits, flags,
planar stereo and logical row-major history. Decode validates the complete
payload before writing samples, ID or the output view. Insufficient buffers
report required sample and ID capacities without touching outputs. Encoded and
decoded stream IDs are opaque bytes as in the existing codec. Caller input and
output spans must not overlap. Decoded views borrow both destination buffers.

PacketStream reuses upstream COBS-ZPE reader/writer algorithms, v1 packet header,
transmitter and checksum. The writer adapter supplies its small scratch array
to the upstream core instead of changing the process-global allocator. The
receiver collects decoded envelope bytes in a bounded caller span and checks
the upstream checksum; it does not use the upstream allocating packet receiver.
This adapter relies on the pinned core's protected buffer/configuration API;
upgrade it together with boundary byte compatibility tests.

Receive accepts arbitrary byte fragments, invokes a synchronous borrowed packet
callback and recovers at a delimiter after malformed/oversized input. Raw storage
needs payload size plus up to four envelope bytes (65539 for maximum packets).
Outgoing storage is a bounded single-packet queue: enqueue snapshots bytes,
rejects overflow without replacing pending data, and flush advances only by the
transport's reported partial write count. Both storage spans must be disjoint.
Callbacks must not reenter the same PacketStream. Reset clears pending delivery;
overflow counters remain cumulative. ByteTransport exposes nonblocking read/write
counts and connection state; TCP/UART implementations belong to outer adapters.

stream_fragments preserves BDSP data ID 13's uint32 total/offset prefix and 60000
byte chunks. FrameAssembler uses one caller buffer, enforces the 2 MiB frame limit
and contiguous offsets, resets on errors and returns a borrowed complete frame.
Offset zero restarts assembly, matching the current desktop receiver. Application
subscription, unavailable-marker priority, timeouts and retry policy remain
outside this codec/framing library.
