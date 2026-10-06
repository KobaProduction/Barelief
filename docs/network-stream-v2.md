# Typed direct streams (BDSP v2)

The registry remains a control-plane directory. PCM and DSP frames travel directly
between core stream endpoints, normally port 8214. The engine v1 control endpoint
remains port 8213 and the aggregator normally uses 8212. All addresses and ports
are configurable. No compression, deployment or graph partitioning is performed.

## Composition and configuration

`NetworkRuntime` owns a Qt network worker independent of the GUI event loop. It
subscribes to runtime output ports with bounded dispatch and binds inputs with the
receiver epoch. Replacing the graph or stopping/restarting the analyzer cancels
bindings. Local graphs without connected network nodes create no stream sockets
or registry connection. Registry registration additionally requires
`aggregator.enabled`, except an explicitly enabled engine control service can
register itself without a data stream.

Core config fields:

```json
{
  "device": {"uuid": "a stable UUID"},
  "engine_server": {
    "enabled": false, "listen_address": "127.0.0.1", "port": 8213,
    "allow_mutations": false
  },
  "network": {"listen_address": "127.0.0.1", "stream_port": 8214},
  "aggregator": {"enabled": false, "host": "127.0.0.1", "port": 8212}
}
```

NetworkOutput parameters: `stream_id`, `data_type`. NetworkInput parameters:
`stream_id`, `data_type`, `provider_uuid`; explicit `host`/`port` provide standalone
addressing. A configured registry resolves a provider UUID to its advertised
stream port and address. Node data types are `audio`, `window`, `windowed`,
`stereo-spectrum`, `spectrum`, `mel`, `history`.

A runtime derives a stable registry session token from its configured UUID, or
accepts explicit `aggregator.session_token`. Unrelated `AggregatorClient` callers
retain legacy random tokens unless they supply `AggregatorClientConfig.session_token`.
Active duplicate UUID registrations are rejected. The CRC/session token is not
authentication or encryption.

CLI uses the same `configuration::load/resolve` as GUI. `--config` accepts a full
document or standalone graph; `--preset` selects a stable ID or unique name. An
invalid explicit choice exits with code 2. CLI does not rewrite the document.

## Wire layout

Existing IDs 2–10 and protocol-v1 QDataStream framing retain their formats. New
stream messages use BDSP IDs 11 subscribe, 12 acknowledgement, 13 data fragment,
14 explicit error, 16 registry capabilities, 17 per-node engine views. All BDSP
payloads stay within the uint16 framing limit.

Subscribe/Ack: bytes `00 02`, one byte FrameType (0–6), followed by UTF-8 stream ID
(1–256 bytes). Unknown versions, streams and incompatible types return ID 14 and
close the connection. Subscriptions are read-only and do not grant graph mutation
permission.

Data fragment: uint32 little-endian total frame size, uint32 offset, up to 60,000
bytes of contiguous data. One bounded assembly per connection; fragments cannot
interleave. A typed frame has a 112-byte little-endian header followed by UTF-8
stream ID and IEEE-754 float32 samples. `portable_stream_codec` and TypedFrame
have no Qt dependency.

Header in order: uint16 version (2), uint8 type, uint8 flags, seven uint64 fields
(epoch, producer_epoch, generation, sequence, source_sequence, graph_revision,
timestamp_ns interpreted as signed int64), four uint32 fields (sample_rate,
fft_size, bands, history_capacity), three float32 fields (frequency_step, peak_hz,
magnitude), three uint32 fields (columns, rows, stream ID byte length), then
12 reserved zero bytes. Flags 1 = discontinuity, 2 = Bark already applied, 4 = unavailable. An unavailable
control marker has sequence/columns/rows zero and no payload; sample rate may be zero.
It clears dependent branches across live downstream connections. Pending data for
that stream is purged before delivery; independent stream queues remain active.
A saturated subscriber is disconnected if the marker cannot be queued. Ordinary
data has a nonzero sequence and dimensions.
PCM/window/windowed/stereo-spectrum samples are planar left then right; spectrum
and Mel are one vector; history is row-major. Only the payload for the declared
type is transmitted. Source timestamps retain their clock domain.

Capabilities are a bounded version-2 JSON object: uuid, address, state,
stream_port, streams [{id,data_type}], supported node kinds, graph version,
protocol versions and explicit mutation permission. The registry validates the
sender identity, supplies peer address, and retains disconnected devices with
availability state. Existing registry registration bytes are unchanged.

Per-node engine views use ID 17 only when subscription flag `TypedViews` (32) is
requested. The container starts with uint16 version2, uint64 engine epoch,
uint64 graph revision, uint32 count, then node ID/length uint32 pairs and portable
frames. V1 clients retain legacy View ID8. Apply replies retain request ID and
snapshot epoch/revision for acknowledgement fencing. Aggregate remote view snapshots
are limited to 2 MiB; an invalid or oversized snapshot returns an explicit error
and disconnects that subscriber instead of leaving a stale display.

## Bounds and recovery

Max decoded frame and assembly: 2 MiB. Producer data mailbox: 4 MiB or 64 frames, plus one latest unavailable marker per
configured output (at most 128).
Each subscriber socket has a 2 MiB write budget. A slow receiver drops complete
frames, marks the next delivery discontinuous, and does not block other peers.
Publisher accepts at most 16 concurrent subscribers. Handshake timeout is 5s;
stream retry delays are 1, 2, then 5s. Stop cancels retries and delivery.
`NetworkRuntime::diagnostics` exposes mailbox usage and loss counters.

## Windows verification

The segmented network/console suites include typed frame goldens and malformed
sizes, seven frame types, fragmented multicast, a stalled receiver, cancellation,
registry capability snapshots/availability, legacy wire goldens and active UUID
rejection, preset selection and raw graphs, and a blocked main event loop for 5s.
`distributed_three_cores` starts three real CLI processes, resolves their UUIDs
through an aggregator, sends 8 changing silent tones through PCM → window/Hann →
stereo FFT/Bark/RMS/Mel → history, compares with a local graph by source sequence,
and restarts the source process. A disconnect propagates unavailable through live
intermediate cores and clears dependent history while an independent source stream
continues; old data cannot revive that history after its marker. These tests do not play audio or verify physical
capture devices, WLED hardware, Linux drivers or an extended soak.
