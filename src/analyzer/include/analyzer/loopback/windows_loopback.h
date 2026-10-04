#ifndef QT_VISUALIZATION_WINDOWS_LOOPBACK_H
#define QT_VISUALIZATION_WINDOWS_LOOPBACK_H

#include <functional>
#include <mutex>
#include <audioclient.h>
#include <mmdeviceapi.h>
#include <string>

#include <QLoggingCategory>

#include "core/abstract/abs_task.h"




namespace audio::loopback {
    typedef struct {
        size_t sample_rate;
        float *data;
        size_t samples;
        size_t channels;
        bool discontinuity = false;
    } loopback_audio;

    typedef std::function<void(loopback_audio)> audio_handler_t;


    enum class CaptureSource { SystemOutput = 0, Microphone = 1 };

    class WASAPILoopback final : public AbstractTask {
    public:
        explicit WASAPILoopback(CaptureSource source = CaptureSource::SystemOutput) : _source(source) {}
        CaptureSource source() const { return _source; }
        void set_audio_handler(const audio_handler_t &handler);
        void set_status_handler(std::function<void(bool, const std::string &)> handler);

        void start() override;

        void stop() override;

        ~WASAPILoopback() override;

    private:
        const CaptureSource _source;
        bool _com_initialized = false;
        HANDLE _capture_event = nullptr;
        std::mutex _event_mutex;

        IAudioClient *pAudioClient = nullptr;
        IAudioCaptureClient *pCaptureClient = nullptr;
        IMMDevice *pDevice = nullptr;
        IMMDeviceEnumerator *pEnumerator = nullptr;
        WAVEFORMATEX *pwfx = nullptr;

        mutable std::mutex _handler_mutex;
        bool _float_samples = true;
        audio_handler_t _handler = nullptr;
        std::function<void(bool, const std::string &)> _status_handler;
        bool _discontinuity = false;
        uint64_t _capture_session();
        void _report(bool running, const std::string &message);

        static HRESULT _show_error_and_return(const std::string& msg, HRESULT result);

    protected:
        uint64_t _before_run_task() override;

        uint64_t _task() override;

        void _after_close_task() override;

        void _handle_audio(const uint8_t *samples, size_t count, size_t sample_rate) const;

        int _init();

        void _reset();
    };
}
#endif //QT_VISUALIZATION_WINDOWS_LOOPBACK_H
