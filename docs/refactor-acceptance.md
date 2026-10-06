# Refactor acceptance — 2026-10-06

Baseline: `3de4de8`. Product/repository name, configuration keys, node/dock IDs,
UUIDs and unknown JSON fields remain compatible. Local commits only; no push,
PR or merge. The implementation map is [architecture-map.md](architecture-map.md).

## Coordinator evidence

| Requirement | Implementation and acceptance evidence |
| --- | --- |
| Portable signal, DSP, runtime, protocol | Separate public CMake targets under `libs/`, plus portable domain/application. C++20 SDK builds without Qt, miniaudio, OS networking or ESP-IDF; exceptions/RTTI disabled. |
| Shared executor/lifecycle | GUI and console composition use the same desktop adapter of GraphExecutor/GraphLifecycle. Real three-core source kill/restart, independent branch continuity and numeric comparisons pass. Old executor and `src/` implementation tree removed. |
| Prepared bounded memory | Explicit limits, owned pool handles, prepared buffers and continuous history ring. Independent allocation probes cover full DSP ticks and unsubscribed desktop ingress. Session has a fixed16 retired tuple FIFO, not an unlimited replay database. |
| TCP/UART | Shared typed codec/framing, exact stream subscription, partial reads/writes, overflow, reconnect and provenance/fence tests. Desktop UART endpoint selected explicitly. Original baseline codec byte comparisons pass. |
| SDK and firmware hooks | Public headers, CMake install/export, external no-Qt consumer, installed source component and synthetic ESP32 example. WiFi/socket and I2S/Bluetooth acquisition are host hooks. |
| Qt boundary and regression scope | Injected engine port/provider; endpoint composition in app entrypoint. Resize, inspector collapse, footer, slot/floating exposure, hidden rendering, configuration/theme/lifecycle tests pass. Native graphics is separately pending. |
| Independent acceptance | New reviewers read rules/reference/source and perform own probes. Architecture A1, desktop D1 and runtime R1–R7 findings repaired and independently closed; reports preserve original RED evidence. |

Final coordinator commands use Qt6.10.2, MinGW13.1 and Release with assertions
enabled for tests. Only coordinator operates shared build directories.

- `cmake --build build-gui -j4`: pass; GUI, console, registry and compatibility apps rebuilt.
- `ctest --test-dir build-gui --output-on-failure -j1`: **64/64 passed, 113.17s**; final log `build-gui/final-fence-tests.log`. Native/device QA OFF; synthetic/offscreen/loopback only.
- `cmake --build build-headless-verify -j3`: pass, GUI disabled.
- `scripts/verify-portable-sdk.ps1 -BuildDirectory build-sdk -BdspSourceDirectory <pinned-local-BDSP>`: **13/13 passed, 1.38s**; install in temporary build prefix, external consumer build/run, installed-source component **3/3 passed, 0.10s**. Log `build-sdk/final-fence-verification.log`.
- Separate C++20 FFT512/no-exceptions/no-RTTI build `build-sdk-small`: **13/13 passed, 0.32s**. Log `build-sdk-small/final-fence-tests.log`.
- `scripts/verify-desktop-runtime.ps1`: GUI profile **4 executables/33 local PE dependencies**, headless **3/12**; dynamically loaded plugins included. No SDK PATH dependency is needed to resolve deployed imports. Logs `build-gui/final-fence-dll-verification.log`, `build-headless-verify/final-fence-dll-verification.log`.

Independent reports: [architecture](review-final-architecture.md),
[runtime/protocol](review-final-runtime.md), [desktop](review-final-desktop.md).
Runtime's final own tests: 12/12 plus unchanged generation/sequence/retirement
probes, exact baseline wire comparisons and zero-allocation DSP probe.
Architecture's resource probe counts new/malloc/calloc/realloc over construction,
handshake, 48 producer changes and stop/begin. Session size on host MinGW x64
grows 776→1056 bytes; firmware ABI size is not inferred from that measurement.

User `config.json` remains untracked, SHA256
`9EE158D2264D7D1D64F54A238D347AE492548857D7495FD6B2AC7FC315FD98BE`;
user `output/` remains untracked. Test configurations/logs live in test/build
directories. No real audio playback or physical device capture was used.

## Remaining explicit acceptance

See [manual-acceptance.md](manual-acceptance.md): native Windows OpenGL/theme,
physical audio/serial/ESP32, ESP-IDF cross-build and optimized device backend.
ESP-IDF is unavailable; nothing was installed. Host component smoke is not a
firmware cross-build. I2S/Bluetooth capture, automatic serial discovery and
automatic graph partitioning were excluded from this stage. The unspecified
“tokens” issue remains in backlog. No long/30-minute stress run was performed.
Bounded retirement retains16 identities; older evicted identities are outside
that protection window. SDK public methods/wire layout unchanged, but the
private Session layout changed and consumers must rebuild from current headers.
