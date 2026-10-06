#pragma once
#include <signal_contracts/frame.h>
// Firmware extension points; transport/DSP task access as documented below.
extern "C" {
// app_main control plane: initialize WiFi/I2S/Bluetooth outside this SDK.
// Return false on setup error. Default supplies the synthetic source only.
bool streamer_host_setup();
// Transport task only: return a connected/accepted descriptor or -1, promptly.
// The SDK switches it nonblocking. Host retains ownership and closes on release.
int streamer_host_acquire_socket();
void streamer_host_release_socket(int descriptor);
// DSP task extension: nonblocking copied planar PCM, exactly the configured
// block size/sample rate. ISR/capture callbacks only fill a bounded owner queue
// and notify a task; no graph processing, allocation or transport in an ISR.
// Enable STREAMER_EXTERNAL_PCM to select this hook. Default would_block waits
// for device firmware; preserve metadata/source clock explicitly.
signal_contracts::Status streamer_host_read_pcm(signal_contracts::FrameView *frame);
}
