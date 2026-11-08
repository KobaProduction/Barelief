# Finite WAV reference runner

The optional `rhythm_reference_runner` target also processes a complete, finite
WAV on one prepared graph. Build it explicitly with the `release` preset; it is
absent from the default build and SDK.

```powershell
./cmake-build-release/bin/rhythm_reference_runner.exe --whole-input --preset essentia-finite-multifeature --input ./input.wav --output ./fresh-result --packet-samples 4096
```

Presets are `essentia-finite-degara` and `essentia-finite-multifeature` (native
44100 Hz). The Madmom combined graph publishes rolling snapshots; it has no
whole-input/EOF contract and is not offered by this finite runner.
Channel policy is an explicit
arithmetic mean. Other rates are rejected. Packet sizes 97 and 4096 select the
actual PCM ingress partition, including its final partial packet.

Supported containers are little endian RIFF WAVE with ordinary PCM (8/16/24/32
bits) or IEEE float (32/64 bits), one fmt chunk and one data chunk. RF64, RIFX,
extensible/compressed formats and format extensions are explicitly unsupported.
RIFF and chunk boundaries, odd-byte padding, block alignment and byte rate are
checked against physical EOF before the decoder can clamp a truncated input.
The WAV declares N. Its decoder-reported and actual decoded length must equal N. Cold preparation
checks the graph for that N before allocating/decoding its PCM. The offline
profile declares 4,194,304 elements and 16 MiB per frame/kernel input/output,
32 MiB plan, 512 MiB payload, 512 MiB scratch, 1 MiB stack and 2 GiB process peak.
320 MiB of that peak is reserved for input/collector, bounded final captures,
decoder and serialization ownership; final captures themselves have a combined
128 MiB ceiling. The admitted graph geometry can impose a smaller N. These are
resource limits, not a fixed recording duration or an equivalence claim beyond
the inputs actually compared.

Essentia collects the actual identity-resampler PCM at port 2, requires A=N,
and submits all A samples once at ingress 3. The final submission happens after
all real PCM processing calls have returned. No EOF PCM is invented.
Callbacks copy into cold-owned storage and perform no file I/O or reentry.

The output directory must be new. `graph.json`, `recipe.json`, `pcm.f32`, required
stable-port payloads and `capture.json` record the actual run. Payloads use little
endian primitive storage with explicit dtype/shape in the manifest. The manifest
records WAV/PCM/graph/executable hashes, sample and publication counts, boundary
markers and errors.

`execution_complete` requires every mandatory endpoint to publish exactly once
after the final batch. `result=no_result` requires successful, available typed
empty beat output; a missing endpoint or error remains an error. Final captures
require finite values. `numeric_parity=not_evaluated` deliberately
requires a separate pinned-source comparison. Source-copy safety corrections
and raw upstream results remain separately identified by those comparison tools.

Whole-input options cannot be mixed with capture, CONTROL, duration, observation,
project-load or export options. Existing live preset identities remain separate.
The optional `rhythm_whole_input_tests` target exercises real WAV decoding,
9-second Essentia runs with both packet sizes, exact final payload equality,
and cold/lifecycle rejection paths. Removed Madmom whole-input preset IDs are
rejected before an output directory is created. The obsolete Madmom whole-builder
frontend diagnostic has been removed with that path.
