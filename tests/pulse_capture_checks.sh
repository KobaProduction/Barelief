#!/usr/bin/env bash
# Requires a running PulseAudio/pipewire-pulse server permitting null sinks.
set -euo pipefail
binary=${1:?Pass the build bin directory}/native_capture_checks
saved=$(pactl get-default-sink)
module=''
pid=''
log=$(mktemp)
cleanup() {
    if [[ -n "$pid" ]]; then kill "$pid" 2>/dev/null || true; wait "$pid" 2>/dev/null || true; fi
    pactl set-default-sink "$saved" || true
    if [[ -n "$module" ]]; then pactl unload-module "$module" || true; fi
    rm -f "$log"
}
trap cleanup EXIT
module=$(pactl load-module module-null-sink sink_name=qt_visualization_qa rate=48000 channels=2)
timeout 30 "$binary" 9000 > "$log" 2>&1 & pid=$!
sleep 2
pactl set-default-sink qt_visualization_qa
sleep 3
# Removing the selected endpoint must recover to the remaining default device.
pactl unload-module "$module"; module=''
pactl set-default-sink "$saved"
wait "$pid"; pid=''
cat "$log"
grep -q 'qt_visualization_qa.monitor' "$log"
grep -q "$saved" "$log"
module=$(pactl load-module module-null-sink sink_name=qt_visualization_mono_qa rate=44100 channels=1 channel_map=mono)
pactl set-default-sink qt_visualization_mono_qa
pactl list short sources | grep 'qt_visualization_mono_qa.monitor'
"$binary" 4000 mono-tone > "$log" 2>&1
cat "$log"
grep -q 'qt_visualization_mono_qa.monitor.*44100' "$log"
pactl set-default-sink "$saved"
pactl unload-module "$module"; module=''
"$binary" 2000 microphone
