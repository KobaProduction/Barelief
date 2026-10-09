#include "dsp_kernels/dsp.h"
#include <algorithm>
#include <cmath>
#include <limits>
#include <climits>
#include "backend.h"

namespace dsp_kernels {
namespace {
// Scale only large blocks; ordinary/subnormal input keeps its original precision.
int fft_scale(std::span<const float> input,std::size_t n) {
    float maximum=0;for(float x:input){if(!std::isfinite(x))return INT_MIN;maximum=std::max(maximum,std::abs(x));}
    int exponent=0;std::frexp(maximum,&exponent);
    int growth=0;for(auto size=n;size>1;size>>=1)++growth;
    return std::max(0,exponent+growth-120);
}
float fft_magnitude(float re,float im,std::size_t n,int scale,bool hartley=false) {
    const double magnitude=hartley?std::abs(double(re)-double(im)):std::hypot(double(re),double(im));
    return float(std::scalbn(magnitude/double(n),scale));
}
}
void hann_window(std::span<float> output) {
    for (size_t i = 0; i < output.size(); ++i)
        output[i] = output.size() < 2 ? 1 : 0.5 * (1 - std::cos(float(i * 2 * 3.14159265358979323846 * (1 / float(output.size() - 1)))));
}
void window(std::span<float> output, WindowFunction function) {
    if (function == WindowFunction::Hann) { hann_window(output); return; }
    for (size_t i = 0; i < output.size(); ++i) {
        const float angle = output.size() < 2 ? 0 : float(i * 2 * 3.14159265358979323846 * (1 / float(output.size() - 1)));
        output[i] = output.size() < 2 || function == WindowFunction::Rectangular ? 1 :
            function == WindowFunction::Hamming ? 0.54f - 0.46f * std::cos(angle) :
            0.42f - 0.5f * std::cos(angle) + 0.08f * std::cos(2 * angle);
    }
}
void bark_weights(std::span<float> output, float step) {
    for (size_t i = 0; i < output.size(); ++i) {
        const float base = i * (step / 650);
        output[i] = 7 * std::log(base + std::sqrt(1 + base * base));
    }
}
std::optional<MemoryRequirements> estimate_memory(std::size_t n, std::size_t bands, std::size_t queue_slots,
                                                  std::size_t history_rows) {
    if (n < 512 || n > max_fft_samples || (n & (n - 1)) || bands > 50 || queue_slots > 16 || history_rows > 2000)
        return std::nullopt;
    MemoryRequirements memory;
    // Bounds above make every multiplication and sum fit even 32-bit size_t.
    memory.fft_scratch_bytes = 2 * n * sizeof(float);
    memory.mel_boundaries_bytes = (bands + 2) * sizeof(double);
    memory.window_bytes = n * sizeof(float);
    memory.weights_bytes = (n / 2) * sizeof(float);
    memory.stereo_input_bytes = 2 * n * sizeof(float);
    memory.stereo_spectrum_bytes = n * sizeof(float);
    memory.stereo_mel_bytes = 2 * bands * sizeof(float);
    memory.queue_payload_bytes = queue_slots * 2 * n * sizeof(float);
    memory.history_payload_bytes = history_rows * bands * sizeof(float);
    // Shared tables are prepared for the build profile, independent of instance N.
    memory.shared_radix2_tables_bytes = detail::shared_table_bytes(false);
    memory.shared_radix4_tables_bytes = detail::shared_table_bytes(true);
    return memory;
}
bool FFT::prepare(std::size_t n, bool radix4) {
    scratch_.clear();
    if (n < 4 || n > max_fft_samples || (n & (n - 1)) || (radix4 && (n & 0x55555555) == 0))
        return false;
    if (!detail::prepare_backend(n, radix4))
        return false;
    radix4_ = radix4;
    scratch_.resize(n * 2);
    return true;
}
bool FFT::process(std::span<const float> input, std::span<float> output, bool hartley) {
    const auto n = scratch_.size() / 2;
    if (!n || input.size() != n || output.size() != n / 2)
        return false;
    const auto scale=fft_scale(input,n);if(scale==INT_MIN)return false;
    for (std::size_t i = 0; i < n; ++i) {
        scratch_[2 * i] = std::scalbn(input[i],-scale);
        scratch_[2 * i + 1] = 0;
    }
    if (!detail::transform(scratch_.data(), n, radix4_))
        return false;
    for (std::size_t i = 0; i < output.size(); ++i) {
        const auto re = scratch_[2 * i], im = scratch_[2 * i + 1];
        const auto value=fft_magnitude(re,im,n,scale,hartley);
        if(!std::isfinite(value))return false;
        // Stage results in the consumed real scratch lane before publishing.
        scratch_[2*i]=value;
    }
    for(std::size_t i=0;i<output.size();++i)output[i]=scratch_[2*i];
    return true;
}
signal_contracts::Status FFT::process_plane(const signal_contracts::NumericView& input,
    signal_contracts::MutableNumericView& output, std::size_t plane) {
    using namespace signal_contracts;
    const auto n = scratch_.size() / 2;
    if (!n || input.actual[0] != n || output.actual[0] != n / 2 ||
        plane >= input.planes.size() || plane >= output.planes.size()) return Status::invalid_argument;
    for (std::size_t i = 0; i < n; ++i) {
        auto status = load_element(input, {plane, i, 0}, scratch_[2 * i]);
        if (status != Status::ok) return status;
        scratch_[2 * i + 1] = 0;
    }
    float maximum=0;for(std::size_t i=0;i<n;++i){const float x=scratch_[2*i];if(!std::isfinite(x))return Status::invalid_argument;maximum=std::max(maximum,std::abs(x));}
    int exponent=0;std::frexp(maximum,&exponent);int growth=0;for(auto size=n;size>1;size>>=1)++growth;
    const int scale=std::max(0,exponent+growth-120);
    for(std::size_t i=0;i<n;++i)scratch_[2*i]=std::scalbn(scratch_[2*i],-scale);
    if (!detail::transform(scratch_.data(), n, radix4_)) return Status::unavailable;
    for (std::size_t i = 0; i < n / 2; ++i) {
        const auto value = fft_magnitude(scratch_[2 * i], scratch_[2 * i + 1],n,scale);
        if (!std::isfinite(value)) return Status::overflow;
        scratch_[2*i]=value;
    }
    for(std::size_t i=0;i<n/2;++i){
        auto status = store_element(output, {plane, i, 0}, scratch_[2*i]);
        if (status != Status::ok) return status;
    }
    return Status::ok;
}
bool MelFilterBank::prepare(std::size_t bins, std::size_t rate, std::size_t bands) {
    bins_ = bands_ = 0;
    boundaries_.clear();
    frequency_step_=0;
    if (bins > max_fft_samples / 2 || bands > max_mel_bands)
        return false;
    bins_ = bins;
    bands_ = bands;
    boundaries_.assign(bands + 2, 0);
    if (!bins || !rate)
        return true; // Legacy zero-rate frames produce zero Mel.
    frequency_step_=double(rate)/double(bins*2);
    const double mel_max = 2595.0 * std::log10(1.0 + (double(rate) * 0.5) / 700.0);
    for (std::size_t i = 0; i < boundaries_.size(); ++i) {
        const double point = mel_max * double(i) / double(bands + 1);
        boundaries_[i] = 700.0 * std::expm1(point/2595.0*std::log(10.0));
    }
    return true;
}
bool MelFilterBank::process(std::span<const float> input, std::span<float> output) const {
    if (input.size() != bins_ || output.size() != bands_ || boundaries_.empty())
        return false;
    for (std::size_t m = 1; m <= bands_; ++m) {
        const auto left = boundaries_[m - 1], center = boundaries_[m], right = boundaries_[m + 1];
        double weighted_magnitude = 0;
        for(std::size_t k=0;k<bins_;++k){const double hz=double(k)*frequency_step_;
            const double weight=hz<=left || hz>=right || center<=left || right<=center?0:
                hz<center?(hz-left)/(center-left):(right-hz)/(right-center);
            if(!std::isfinite(input[k]))return false;
            weighted_magnitude+=double(input[k])*weight;}
        const float result=float(weighted_magnitude);if(!std::isfinite(result))return false;
        output[m - 1] = result;
    }
    return true;
}
bool StereoProcessor::prepare(std::size_t n, std::size_t rate, std::size_t bands) {
    // Invalidate both plans on invalid geometry, avoiding stale Mel state.
    const bool fft_ready = fft_.prepare(bands <= max_mel_bands ? n : 0, (n & 0x55555555) != 0);
    const bool mel_ready = mel_.prepare(fft_ready ? n / 2 : max_fft_samples, rate, bands);
    return fft_ready && mel_ready;
}
bool StereoProcessor::process(std::span<float> left, std::span<float> right, std::span<const float> window,
                              std::span<const float> weights, std::span<float> sl, std::span<float> sr,
                              std::span<float> ml, std::span<float> mr) {
    if (left.size() != right.size() || left.size() != window.size() || sl.size() != sr.size() ||
        sl.size() != weights.size() || sl.size() * 2 != left.size())
        return false;
    for (std::size_t i = 0; i < left.size(); ++i) {
        left[i] *= window[i];
        right[i] *= window[i];
    }
    if (!fft_.process(left, sl) || !fft_.process(right, sr))
        return false;
    for (std::size_t i = 0; i < sl.size(); ++i) {
        sl[i] *= weights[i];
        sr[i] *= weights[i];
        if (!std::isfinite(sl[i]))
            sl[i] = 0;
        if (!std::isfinite(sr[i]))
            sr[i] = 0;
    }
    return (ml.empty() && mr.empty()) || process_mel(sl, sr, ml, mr);
}
bool StereoProcessor::process_mel(std::span<const float> left, std::span<const float> right, std::span<float> ml,
                                  std::span<float> mr) const {
    return mel_.process(left, ml) && mel_.process(right, mr);
}
} // namespace dsp_kernels
