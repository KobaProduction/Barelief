#pragma once
#include "pipeline/test_signal.h"
#include <utility>

namespace test_fixture {
// Retain the selected port identity while using only the existing eight-tone
// in-memory worker. No branch of this factory delegates to a physical driver.
class SyntheticCapture final : public pipeline::IAudioSource {
public:
    SyntheticCapture(pipeline::AudioSourceKind kind, bool deliver_audio, bool distinct_capture=false,unsigned native_channels=1)
        : _kind(kind), _deliver_audio(deliver_audio), _distinct_capture(distinct_capture), _native_channels(native_channels), _worker(pipeline::make_test_signal_source()) {}
    void start() override { _worker->start(); }
    void configure(const QJsonObject &parameters) override { auto configured=parameters;if(_distinct_capture)configured["amplitude"]=0.002;const auto requested=parameters.value("channel_count").toInt();_channels=requested>0?unsigned(requested):_native_channels;_frame.samples.reserve(std::size_t(std::max(8000,parameters.value("sample_rate").toInt(48000))/4)*_channels);_worker->configure(configured); }
    bool reconfigure_live(const QJsonObject &parameters) override { auto configured=parameters;if(_distinct_capture)configured["amplitude"]=0.002;return _worker->reconfigure_live(configured); }
    void stop() override { _worker->stop(); }
    bool is_running() const override { return _worker->is_running(); }
    pipeline::AudioSourceKind source() const override { return _kind; }
    pipeline::AudioSourceDiagnostics diagnostics() const override { auto result=_worker->diagnostics();result.native_channels=result.delivered_channels=_channels;return result; }
    void set_audio_handler(AudioHandler handler) override {
        if(!_deliver_audio){_worker->set_audio_handler({});return;}
        _worker->set_audio_handler([this,handler=std::move(handler)](const pipeline::AudioFrame& input){
            if(_channels==1){handler(input);return;}
            deliver_multichannel_frame(input,handler);
        });
    }
    void set_status_handler(StatusHandler handler) override { _worker->set_status_handler(std::move(handler)); }
    void set_lifecycle_handler(LifecycleHandler handler) override { _worker->set_lifecycle_handler(std::move(handler)); }
private:
    void deliver_multichannel_frame(const pipeline::AudioFrame& input,const AudioHandler& handler) {
        if(input.frame_count*_channels>_frame.samples.capacity())return;
        _frame=input;
        _frame.channel_count=_channels;
        _frame.samples.resize(input.frame_count*_channels);
        for(unsigned lane=0;lane<_channels;++lane)
            for(std::size_t i=0;i<input.frame_count;++i)
                _frame.samples[lane*input.frame_count+i]=input.samples[i]/float(lane+1);
        handler(_frame);
    }
    pipeline::AudioSourceKind _kind;
    bool _deliver_audio;
    bool _distinct_capture;
    unsigned _native_channels;
    unsigned _channels=1;pipeline::AudioFrame _frame;
    std::unique_ptr<pipeline::IAudioSource> _worker;
};

class UnavailableCapture final : public pipeline::IAudioSource {
public:
    void start() override {if(_lifecycle)_lifecycle(false,1,"Injected loopback unavailable");if(_status)_status(false,"Injected loopback unavailable");}
    void stop() override {}
    bool is_running() const override {return false;}
    pipeline::AudioSourceKind source() const override {return pipeline::AudioSourceKind::SystemOutput;}
    void set_audio_handler(AudioHandler) override {}
    void set_status_handler(StatusHandler handler) override {_status=std::move(handler);}
    void set_lifecycle_handler(LifecycleHandler handler) override {_lifecycle=std::move(handler);}
private:
    StatusHandler _status;LifecycleHandler _lifecycle;
};

inline pipeline::AudioSourceFactory startup_capture_factory(bool unavailable=false,unsigned native_channels=1) {
    return [unavailable,native_channels](pipeline::AudioSourceKind kind)->std::unique_ptr<pipeline::IAudioSource> {
        if(kind==pipeline::AudioSourceKind::SystemOutput && unavailable)return std::make_unique<UnavailableCapture>();
        return std::make_unique<SyntheticCapture>(kind,true,kind==pipeline::AudioSourceKind::SystemOutput,native_channels);
    };
}

inline pipeline::AudioSourceFactory synthetic_capture_factory(bool deliver_audio = true) {
    return [deliver_audio](pipeline::AudioSourceKind kind) {
        return std::make_unique<SyntheticCapture>(kind, deliver_audio);
    };
}
} // namespace test_fixture
