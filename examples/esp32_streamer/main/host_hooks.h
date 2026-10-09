#pragma once
#include <signal_contracts/data_frame.h>
#include <application/processing_control.h>
// Firmware extension points; transport/DSP task access as documented below.
extern "C" {
// app_main control plane: initialize WiFi/I2S/Bluetooth outside this SDK.
// Return false on setup error. Default supplies the synthetic source only.
bool streamer_host_setup();
// Actual bytes made available to this composition, after other firmware owners.
// Default samples currently free 8-bit-capable heap; configured KiB is a ceiling.
std::size_t streamer_host_memory_budget();
// Transport/control task only: nonblocking; return a connected/accepted descriptor
// or -1 immediately. It never shares executor/acquisition ownership.
// The SDK switches it nonblocking. Host retains ownership and closes on release.
int streamer_host_acquire_socket();
void streamer_host_release_socket(int descriptor);
// Acquisition/DSP owner task extension: nonblocking copied planar PCM, exactly the configured
// block size/sample rate. ISR/capture callbacks only fill a bounded owner queue
// and notify a task; no graph processing, allocation or transport in an ISR.
// Enable STREAMER_EXTERNAL_PCM to select this hook. Default would_block waits
// for device firmware; preserve metadata/source clock explicitly.
signal_contracts::Status streamer_host_read_pcm(signal_contracts::DataFrameView *frame);
// Transport task telemetry hook, at most once per second. Measured cumulative
// processing elapsed microseconds and maximum invocation duration; no inferred CPU %.
void streamer_host_report_processing(std::uint64_t frames,std::uint64_t elapsed_us,std::uint64_t maximum_us,std::uint64_t dropped_output_frames);
// Optional host-owned persistence override. Stock entrypoint otherwise opens
// NVS without erasing, restores an admitted last-known-good graph and applies it
// through the sole application service. Only explicit Persist writes flash.
application::ProcessingPersistencePort* streamer_host_persistence();
signal_contracts::Status streamer_host_restore_processing(domain::NumericProcessingGraph* graph);
}
