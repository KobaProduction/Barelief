#pragma once
#include <QString>
#include <dsp_kernels/frequency_profile.h>
#include <cstdint>
#include <memory>
#include <span>
namespace desktop_analysis::offline {
// Decode native channels and apply the explicitly supplied channel selection/mix.
// Decoder never opens an audio device. Native sample rate is resampled separately by FIR.
class AudioDecoder {
 public:
    AudioDecoder();
    ~AudioDecoder();
    bool open(const QString& path, QString& error,
              dsp_kernels::ChannelPolicy policy=dsp_kernels::ChannelPolicy::Mean,
              std::uint32_t channel_index=0);
    bool read(std::span<float> mono, std::size_t& frames, QString& error);
    std::uint32_t sample_rate() const;
    std::uint64_t length() const;
 private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
