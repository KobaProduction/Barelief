# Desktop analysis compatibility adapter

Canonical public headers are under `adapters/desktop/include/analyzer/analyze`;
the four implementations are under `adapters/desktop/analysis/src/analyze`.
`adapters/desktop/analysis/sources.cmake` supplies `DESKTOP_ANALYSIS_HEADERS`,
`DESKTOP_ANALYSIS_SOURCES` and `DESKTOP_ANALYSIS_INCLUDE_DIR`. The composition
root owns target creation, dependency wiring and application builds. Obsolete
`src/analyzer/include` forwarding headers have been removed; targets expose the
canonical include directory. There is one compiled implementation of each class. Public include names, global class
names, enums, callbacks, signatures and data layout remain compatible.

## Boundary and dependencies

`Analyzer` is a desktop worker/queue adapter built on `AbstractTask`, desktop
telemetry and the desktop Qt dependency boundary. It owns the existing recursive
mutex, condition-variable wakeup, 16-slot pending queue, cadence bookkeeping,
epoch/ingress fences and callback delivery. This move preserves locking, overflow,
timestamps, stop wakeup and public snapshot behavior. It does not make this queue
a replacement for the capture adapter's realtime-safe SPSC queue.

Actual desktop capture configures `Analyzer::set_pcm_only(true)`. It delivers PCM
to the shared graph executor; the graph determines window/FFT/Mel/history stages.
The compatibility `window_only` and full-analysis paths remain available for old
callers and characterization tests, rather than becoming another graph executor.

Numerical implementation belongs to `dsp_kernels`. FFT, Mel and stereo processing
delegate to its existing prepared processors. Window generation now calls
`dsp_kernels::hann_window` instead of including the vendor Hann API directly.
The Analyzer window is still 4096 samples and the preserved formula gives exactly
the same float values on the tested Windows/MinGW profile. No kernel, weighting,
FFT normalization, merge formula or numerical parameter changed.

Canonical code names `dsp_kernels` and includes `dsp_kernels/dsp.h`. `fft.h` also
retains `dsp_core/dsp.h` solely to preserve the historical transitive `dsp_core`
namespace alias for public desktop consumers. The canonical `dsp_kernels` public
include tree and installed SDK now provide that compatibility header;
`portable_dsp` only preserves the historical target name. The adapter no longer needs a
direct vendor `dsp` target or vendor include directories; the backend remains a
private kernel dependency. The unused `qlogging.h` include was removed from
Spectrogram. No implementation was proven safe to delete by the reference audit.

## Retained compatibility and deferred cleanup

`AudioWindow` retains its ring and linear snapshot API. `Spectrogram` retains its
legacy `[channel][time]` storage and forward/reversed/vector snapshot methods.
These APIs are not the portable runtime's `[frame][band]` typed history. Changing
their layout would break existing visualization consumers.

Repository references include `tests/portable_dsp_cadence_tests.cpp` (Analyzer),
`tests/runtime_tests.cpp` (FFT/FHT and Spectrogram), `apps/compatibility_gui/app.h`, presentation
`core_app/types.h`, and desktop pipeline headers and history snapshots. The
public `fft_to_mel`, scale/window helpers, public sample/amplitude fields,
legacy full-analysis path and all history accessors are intentionally retained.
Even public helpers with no current repository call site may have external
compatibility consumers; absence of an internal call is not deletion evidence.

Removing these APIs or the public alias header/target or the
legacy visualization fallback requires a separate caller migration and review.
This extraction neither expands the fallback algorithms nor edits graph runtime,
AudioPipeline, application lifecycle, presentation or platform implementation.

## Verification (2026-10-06)

Private probes live in `%TEMP%/qt-analysis-adapter-phase4`, outside production
sources and shared build directories. CMake 3.30, Qt 6.10.2 and MinGW GCC 13.1
Release built the four old implementation snapshots and the manifest's four
canonical implementations as separate libraries. The canonical library compiled
without vendor include directories/direct vendor linkage.

Both libraries passed the unchanged `portable_dsp_cadence_tests.cpp`: idle stop
and restart, exactly 60 frames at 44100/48000/44101 Hz, allocation-free legacy
hop enqueue and bounded overflow (16 delivered, 44 dropped). A private numerical
characterization compared binary float outputs for the 4096-sample Hann window,
all four FFT/FHT modes, Mel, three spectrum merge modes, averaging, AudioWindow
wrap/clear, Spectrogram resize/order/clear and PCM-only worker delivery including
explicit epoch, ingress serial and timestamp. The input is eight in-memory tones
without playback. The 164220-byte outputs have the same SHA-256:

`E8E9EAA2A4F2AB76DD61EAC39105ADF36C88F78DDDE24C92219B50F9DEA4E922`.

Private commands are `cmake -S <probe> -B <probe>/build -G Ninja` with the above
compiler/Qt paths, `cmake --build <probe>/build --parallel 2`, followed by
`cadence_baseline.exe`, `cadence_canonical.exe`, and
`characterize_{baseline,canonical}.exe <output.bin>` plus `Get-FileHash`.
The baseline probe uses saved original cpp files; it cannot be regenerated from
the removed source paths without restoring a pre-move snapshot.

The coordinator separately reported the canonical Analyzer application build and
`audio_sample_cadence` passing after shared CMake integration (1/1, 1.17 s).
Broader application integration and fresh independent primary review remain
coordinator gates. No GUI, native graphics, physical capture, speakers, Linux or
long-running checks were performed for this extraction.
