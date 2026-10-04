#include "core/performance.h"
#include "analyzer/loopback/windows_loopback.h"

#include <thread>
#include <vector>
#include <cstring>
#include <chrono>
#include <format>


#include <initguid.h>  // Для DEFINE_GUID
#include <valarray>

#ifndef KSDATAFORMAT_SUBTYPE_IEEE_FLOAT_
DEFINE_GUID(KSDATAFORMAT_SUBTYPE_IEEE_FLOAT_,
            0x00000003, 0x0000, 0x0010, 0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71);
#endif

#define REFTIMES_PER_SEC  10000000


Q_LOGGING_CATEGORY(wasapi, "wasapi_loopback")

using namespace audio::loopback;


void WASAPILoopback::set_audio_handler(const audio_handler_t &handler) {
    std::lock_guard lock(_handler_mutex);
    _handler = handler;
}

void WASAPILoopback::start() {
    AbstractTask::start();
}

void WASAPILoopback::stop() {
    _running=false;
    { std::lock_guard lock(_event_mutex); if(_capture_event) SetEvent(_capture_event); }
    AbstractTask::stop();
}

void WASAPILoopback::_after_close_task() {
    if (not pAudioClient) { _reset(); return; }
    const HRESULT hr = pAudioClient->Stop();
    if (FAILED(hr)) {
        qCCritical(wasapi, "IAudioClient::Stop failed: hr = 0x%08lx\n", hr);
    }
    _reset();
}

uint64_t WASAPILoopback::_before_run_task() { return 0; }
void WASAPILoopback::set_status_handler(std::function<void(bool, const std::string &)> handler) {
    std::lock_guard lock(_handler_mutex); _status_handler = std::move(handler);
}
void WASAPILoopback::_report(bool running, const std::string &message) {
    std::function<void(bool, const std::string &)> handler;
    { std::lock_guard lock(_handler_mutex); handler = _status_handler; }
    if (handler) handler(running, message);
}
uint64_t WASAPILoopback::_task() {
    while (_running.load()) {
        auto result = _init();
        if (!result && _running.load()) {
            _report(true, {});
            result = static_cast<int>(_capture_session());
        }
        _reset();
        if (!_running.load()) break;
        _report(false, std::format("Audio device unavailable (HRESULT 0x{:08x}); retrying", static_cast<uint32_t>(result)));
        _sleep(500); // AbstractTask sleep is interruptible by stop().
    }
    return 0;
}

void WASAPILoopback::_handle_audio(const uint8_t *samples, const size_t count, const size_t sample_rate) const {
    core::perf::Scope trace(core::perf::Stage::CaptureDecode);
    audio_handler_t handler;
    { std::lock_guard lock(_handler_mutex); handler = _handler; }
    if (!handler) return;
    std::vector<float> planar(count * pwfx->nChannels, 0.0f);
    if (samples) {
        const size_t bytes = pwfx->wBitsPerSample / 8;
        for (size_t ch = 0; ch < pwfx->nChannels; ++ch) {
            for (size_t i = 0; i < count; ++i) {
                const auto *sample = samples + i * pwfx->nBlockAlign + ch * bytes;
                float value = 0;
                if (_float_samples) std::memcpy(&value, sample, sizeof(value));
                else if (bytes == 2) {
                    int16_t pcm; std::memcpy(&pcm, sample, sizeof(pcm));
                    value = pcm / 32768.0f;
                } else if (bytes == 3) {
                    int32_t pcm = sample[0] | (sample[1] << 8) | (sample[2] << 16);
                    if (pcm & 0x800000) pcm -= 0x1000000;
                    value = pcm / 8388608.0f;
                } else {
                    int32_t pcm; std::memcpy(&pcm, sample, sizeof(pcm));
                    value = static_cast<float>(pcm / 2147483648.0);
                }
                planar[ch * count + i] = value;
            }
        }
    }
    handler(loopback_audio{sample_rate, planar.data(), count, pwfx->nChannels, _discontinuity});
}

uint64_t WASAPILoopback::_capture_session() {
    UINT32 packet_size;
    uint8_t *data;
    uint32_t frames;
    DWORD flags;

    auto next_check = std::chrono::steady_clock::now();
    while (_running.load()) {
        if (std::chrono::steady_clock::now() >= next_check) {
            next_check = std::chrono::steady_clock::now() + std::chrono::seconds(1);
            IMMDevice *current = nullptr;
            const auto check = pEnumerator->GetDefaultAudioEndpoint(_source == CaptureSource::Microphone ? eCapture : eRender, eConsole, &current);
            if (FAILED(check)) return check;
            LPWSTR current_id = nullptr, active_id = nullptr;
            const auto current_result = current->GetId(&current_id);
            const auto active_result = pDevice->GetId(&active_id);
            const bool changed = FAILED(current_result) || FAILED(active_result) || wcscmp(current_id, active_id) != 0;
            CoTaskMemFree(current_id); CoTaskMemFree(active_id); current->Release();
            if (changed) return AUDCLNT_E_DEVICE_INVALIDATED;
        }
        const auto waiting=WaitForSingleObject(_capture_event,50);
        if(!_running.load()) break;
        if(waiting==WAIT_FAILED) return HRESULT_FROM_WIN32(GetLastError());
        if(waiting==WAIT_TIMEOUT) continue;
        HRESULT hr = pCaptureClient->GetNextPacketSize(&packet_size);
        if (FAILED(hr)) {
            return _show_error_and_return("IAudioCaptureClient::GetNextPacketSize failed: hr = 0x%08lx\n", hr);
        }
        while (packet_size && _running.load()) {
            hr = pCaptureClient->GetBuffer(&data, &frames, &flags, nullptr, nullptr);

            if (FAILED(hr)) {
                return _show_error_and_return("IAudioCaptureClient::GetBuffer failed: hr = 0x%08lx\n", hr);
            }

            if (flags & AUDCLNT_BUFFERFLAGS_SILENT) {
                data = nullptr; // Буфер молчания
            }
            _discontinuity = (flags & AUDCLNT_BUFFERFLAGS_DATA_DISCONTINUITY) != 0;
            if (frames) {
                _handle_audio(data, frames, size_t(pwfx->nSamplesPerSec));
            }

            hr = pCaptureClient->ReleaseBuffer(frames);
            if (FAILED(hr)) {
                return _show_error_and_return("IAudioCaptureClient::ReleaseBuffer failed: hr = 0x%08lx\n", hr);
            }
            hr = pCaptureClient->GetNextPacketSize(&packet_size);
            if (FAILED(hr)) return _show_error_and_return("GetNextPacketSize failed: 0x%08lx", hr);
        }

    }
    return 0;
}


int WASAPILoopback::_init() {
    HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (FAILED(hr)) {
        return _show_error_and_return("CoInitializeEx failed: hr = 0x%08lx\n", hr);
    }

    _com_initialized = true;
    hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                          __uuidof(IMMDeviceEnumerator), reinterpret_cast<void **>(&pEnumerator));
    if (FAILED(hr)) return _show_error_and_return("CoCreateInstance failed: 0x%08lx", hr);

    hr = pEnumerator->GetDefaultAudioEndpoint(_source == CaptureSource::Microphone ? eCapture : eRender, eConsole, &pDevice);
    if (FAILED(hr)) {
        return _show_error_and_return("GetDefaultAudioEndpoint failed: hr = 0x%08lx\n", hr);
    }

    hr = pDevice->Activate(::__uuidof(IAudioClient), CLSCTX_ALL, nullptr, reinterpret_cast<void **>(&pAudioClient));
    if (FAILED(hr)) {
        return _show_error_and_return("IMMDevice::Activate failed: hr = 0x%08lx\n", hr);
    }

    hr = pAudioClient->GetMixFormat(&pwfx);
    if (FAILED(hr)) {
        return _show_error_and_return("IAudioClient::GetMixFormat failed: hr = 0x%08lx\n", hr);
    }
    bool pcm = pwfx->wFormatTag == WAVE_FORMAT_PCM;
    _float_samples = pwfx->wFormatTag == WAVE_FORMAT_IEEE_FLOAT;
    if (pwfx->wFormatTag == WAVE_FORMAT_EXTENSIBLE && pwfx->cbSize >= 22) {
        const auto *extended = reinterpret_cast<WAVEFORMATEXTENSIBLE *>(pwfx);
        _float_samples = IsEqualGUID(extended->SubFormat, KSDATAFORMAT_SUBTYPE_IEEE_FLOAT_);
        static const GUID pcm_guid = {0x00000001, 0x0000, 0x0010, {0x80,0x00,0x00,0xaa,0x00,0x38,0x9b,0x71}};
        pcm = IsEqualGUID(extended->SubFormat, pcm_guid);
    }
    if (!pwfx->nChannels || !pwfx->nSamplesPerSec ||
        (_float_samples && pwfx->wBitsPerSample != 32) ||
        (! _float_samples && (!pcm || (pwfx->wBitsPerSample != 16 && pwfx->wBitsPerSample != 24 && pwfx->wBitsPerSample != 32))) ||
        pwfx->nBlockAlign < pwfx->nChannels * (pwfx->wBitsPerSample / 8)) {
        return _show_error_and_return("Unsupported audio mix format: 0x%08lx", E_INVALIDARG);
    }

    hr = pAudioClient->Initialize(
        AUDCLNT_SHAREMODE_SHARED,
        AUDCLNT_STREAMFLAGS_EVENTCALLBACK | (_source == CaptureSource::Microphone ? 0 : AUDCLNT_STREAMFLAGS_LOOPBACK),
        REFTIMES_PER_SEC,
        0,
        pwfx,
        nullptr);
    if (FAILED(hr)) {
        return _show_error_and_return("IAudioClient::Initialize failed: hr = 0x%08lx\n", hr);
    }

    hr = pAudioClient->GetService(::__uuidof(IAudioCaptureClient), reinterpret_cast<void **>(&pCaptureClient));
    if (FAILED(hr)) {
        return _show_error_and_return("IAudioClient::GetService failed: hr = 0x%08lx\n", hr);
    }
    { std::lock_guard lock(_event_mutex); _capture_event=CreateEventW(nullptr,FALSE,FALSE,nullptr); }
    if(!_capture_event) return HRESULT_FROM_WIN32(GetLastError());
    hr=pAudioClient->SetEventHandle(_capture_event);
    if(FAILED(hr)) return _show_error_and_return("SetEventHandle failed: 0x%08lx",hr);
    hr = pAudioClient->Start();
    if (FAILED(hr)) {
        return _show_error_and_return("IAudioClient::Start failed: hr = 0x%08lx\n", hr);

    }
    return 0;
}

HRESULT WASAPILoopback::_show_error_and_return(const std::string &msg, const HRESULT result) {
    qCCritical(wasapi, msg.c_str(), result);
    return result;
}

void WASAPILoopback::_reset() {
    if (pCaptureClient) {
        pCaptureClient->Release();
        pCaptureClient = nullptr;
    }
    if (pwfx) {
        CoTaskMemFree(pwfx);
        pwfx = nullptr;
    }
    if (pAudioClient) {
        pAudioClient->Release();
        pAudioClient = nullptr;
    }

    { std::lock_guard lock(_event_mutex); if(_capture_event) { CloseHandle(_capture_event); _capture_event=nullptr; } }

    if (pDevice) {
        pDevice->Release();
        pDevice = nullptr;
    }

    if (pEnumerator) {
        pEnumerator->Release();
        pEnumerator = nullptr;
    }
    if (_com_initialized) { CoUninitialize(); _com_initialized = false; }
}

WASAPILoopback::~WASAPILoopback() {
    stop();
}
