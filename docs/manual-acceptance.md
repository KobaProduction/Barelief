# Manual acceptance after automated checks

Automated evidence and independent review gates are recorded in
[refactor-execution.md](refactor-execution.md). This checklist does not claim
that the scenarios below have passed. Use a separate test configuration and
synthetic source; preserve the user's `config.json` and `output/`.

## Native Windows desktop

- Launch the rebuilt `cmake-build-release/bin/core_app.exe` with an explicit test config;
  confirm that launching from a fresh shell does not need the Qt SDK in PATH.
- Check native OpenGL surface rendering, palette/contrast, grid, shadows,
  selection marker, time/frequency slice frame and free camera in Light/Dark.
- Check System theme against actual Windows Light/Dark settings, including
  settings menus, native widget popups, floating docks and custom renderers.
- Shrink and expand the workspace: controls stay within their panel, footer
  does not overlap content, a single area has no unused splitter, and the right
  inspector collapses/restores its width without shifting the workspace.
- Switch graph slots and floating windows; hidden graphs stop drawing while
  the shared processing/history and configured outputs continue.
- Confirm real device capture, switching default device, disconnect/reconnect
  and shutdown. These hardware operations are not proved by synthetic tests.

## ESP32 / real UART and TCP

- Use an existing ESP-IDF environment; do not treat the host component smoke
  build as a firmware cross-build. Build the ordinary ESP32 ANSI example first.
- Link/build the optional externally supplied optimized ESP-DSP backend
  separately. Verify its SDK/version/table lifetime with actual firmware.
- Provision WiFi/socket hooks in host firmware and select the explicit UART
  port/pins/baud or connected TCP descriptor. Do not share logging and BDSP UART.
- Verify raw PCM and Window512/FFT/Mel8 delivery to the desktop NetworkInput,
  handshake, fragmented/partial I/O, disconnect/reconnect, queue overflow,
  unavailable marker cancellation and session epoch changes on the real device.
- Measure actual heap, task stack margin, ISR timing, CPU and transport bandwidth.
  Admission calculations are configured bounds, not hardware measurements.
- I2S/Bluetooth acquisition is an integration hook; this migration does not
  implement or claim a working physical Bluetooth audio source.

## Explicit remaining backlog

The previously reported issue called “tokens” remains unspecified. Do not infer
its meaning or mark it fixed from unrelated protocol/session changes. Existing
project backlog records that ambiguity. No long or 30-minute stress run is part
of this migration's automatic gate.
