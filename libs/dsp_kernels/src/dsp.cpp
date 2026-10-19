#include "dsp_kernels/dsp.h"
#include <dsp_kernels/spectral_weights.h>
#include <algorithm>
#include <cmath>
#include <limits>
#include <climits>
#include <cstdint>
#include <functional>
#include "backend.h"
#include "rfft_split.h"
#include "fft_profile.h"
#include "fft_ae32.h"

namespace dsp_kernels {
namespace {
// Scale only large blocks; ordinary/subnormal input keeps its original precision.
int fft_scale_max(float maximum,std::size_t n) {
    int exponent=0;std::frexp(maximum,&exponent);
    int growth=0;for(auto size=n;size>1;size>>=1)++growth;
    return std::max(0,exponent+growth-120);
}
int fft_scale(std::span<const float> input,std::size_t n) {
    float maximum=0;
    if (!detail::fft_input_max(input.data(), input.size(), maximum)) return INT_MIN;
    return fft_scale_max(maximum,n);
}
float spectrum_magnitude(float re,float im,std::size_t n,double restore) {
    return float((std::hypot(double(re),double(im)) / double(n)) * restore);
}
void prepare_window_bank(std::span<float> bank,std::size_t maximum) {
    for (std::size_t index=0;index<window_function_names.size();++index) {
        const auto function=WindowFunction(index);
        window(bank.subspan(std::size_t(function)*maximum,maximum),function);
    }
}
void stage_real_fft_input(std::span<const float> input,std::span<float> scratch,int scale) noexcept {
    const float factor=scale?std::scalbn(1.f,-scale):1.f;
    for(std::size_t i=0;i<input.size();++i){scratch[2*i]=input[i]*factor;scratch[2*i+1]=0;}
}
bool stage_magnitude_bins(std::span<float> scratch,std::size_t n,std::size_t bins,double restore) noexcept {
    for(std::size_t i=0;i<bins;++i){
        const auto value=spectrum_magnitude(scratch[2*i],scratch[2*i+1],n,restore);
        if(!std::isfinite(value))return false;
        scratch[2*i]=value;
    }
    return true;
}
signal_contracts::Status load_fft_plane(const signal_contracts::NumericView& input,
    std::span<float> scratch,std::size_t plane,std::size_t n) noexcept {
    for(std::size_t i=0;i<n;++i){
        const auto status=signal_contracts::load_element(input,{plane,i,0},scratch[2*i]);
        if(status!=signal_contracts::Status::ok)return status;
        scratch[2*i+1]=0;
    }
    return signal_contracts::Status::ok;
}
signal_contracts::Status store_magnitude_plane(signal_contracts::MutableNumericView& output,
    std::span<const float> scratch,std::size_t plane,std::size_t bins) noexcept {
    for(std::size_t i=0;i<bins;++i){
        const auto status=signal_contracts::store_element(output,{plane,i,0},scratch[2*i]);
        if(status!=signal_contracts::Status::ok)return status;
    }
    return signal_contracts::Status::ok;
}
bool prepare_mel_weights(std::size_t bins,std::size_t rate,std::size_t bands,
    std::span<const double> frequencies,std::span<const double> boundaries,
    std::span<double> weights) noexcept {
    const double nyquist=double(rate)*.5;
    double previous=-1;
    for(std::size_t bin=0;bin<bins;++bin){
        const double hz=frequencies.empty()?double(bin)*double(rate)/double(bins*2):frequencies[bin];
        if(!std::isfinite(hz) || hz<0 || hz<=previous || hz>nyquist)return false;
        for(std::size_t band=0;band<bands;++band)
            weights[band*bins+bin]=triangular_weight(hz,boundaries[band],boundaries[band+1],boundaries[band+2]);
        previous=hz;
    }
    return true;
}
bool mel_band_energy(std::span<const float> input,std::span<const double> weights,
    std::size_t band,float& output) noexcept {
    double energy=0;
    for(std::size_t bin=0;bin<input.size();++bin)energy+=double(input[bin])*weights[band*input.size()+bin];
    output=float(energy);
    return std::isfinite(output);
}
void apply_stereo_window(std::span<float> left,std::span<float> right,
    std::span<const float> window) noexcept {
    for(std::size_t i=0;i<left.size();++i){left[i]*=window[i];right[i]*=window[i];}
}
void apply_stereo_weights(std::span<float> left,std::span<float> right,
    std::span<const float> weights) noexcept {
    for(std::size_t i=0;i<left.size();++i){
        left[i]*=weights[i];right[i]*=weights[i];
        if(!std::isfinite(left[i]))left[i]=0;
        if(!std::isfinite(right[i]))right[i]=0;
    }
}
double projected_spectrum_value(SpectrumProjection projection,double real,double imaginary,
    double sided,double amplitude_scale,double power_scale,double psd_scale) noexcept {
    const double squared=real*real+imaginary*imaginary;
    switch(projection){
    case SpectrumProjection::Real:return real;
    case SpectrumProjection::Imaginary:return imaginary;
    case SpectrumProjection::Phase:return squared?std::atan2(imaginary,real):0;
    case SpectrumProjection::Magnitude:return std::sqrt(squared)*amplitude_scale;
    case SpectrumProjection::PeakAmplitude:return sided*std::sqrt(squared)*amplitude_scale;
    case SpectrumProjection::Power:return sided*squared*power_scale;
    case SpectrumProjection::Psd:return sided*squared*psd_scale;
    }
    return 0;
}
}
void hann_window(std::span<float> output) { for(std::size_t i=0;i<output.size();++i)output[i]=window_coefficient(i,output.size(),WindowFunction::Hann); }
void window(std::span<float> output,WindowFunction function) { for(std::size_t i=0;i<output.size();++i)output[i]=window_coefficient(i,output.size(),function); }
PreparedWindows::PreparedWindows(std::size_t maximum) {
    (void)prepare(maximum);
}
bool PreparedWindows::prepare(std::size_t maximum) {
    std::size_t bytes = 0;
    if (!storage_bytes(maximum, bytes) || maximum > bank_.max_size() / window_function_names.size() || maximum >= rotations_.max_size())
        return false;
    std::vector<float> bank(maximum * window_function_names.size());
    std::vector<Rotation> rotations(maximum + 1);
    prepare_window_bank(bank,maximum);
    for (std::size_t length = 0; length <= maximum; ++length) {
        const double angle = length < 2 ? 0 : 6.2831853071795864769 / double(length - 1);
        rotations[length] = {std::cos(angle), std::sin(angle)};
    }
    bank_.swap(bank);
    rotations_.swap(rotations);
    maximum_ = maximum;
    prepared_ = true;
    return true;
}
bool PreparedWindows::storage_bytes(std::size_t maximum, std::size_t& bytes) noexcept {
    constexpr auto per_sample = window_function_names.size() * sizeof(float) + sizeof(Rotation);
    if (maximum > (SIZE_MAX - sizeof(Rotation)) / per_sample) return false;
    bytes = maximum * per_sample + sizeof(Rotation);
    return true;
}
std::span<const float> PreparedWindows::coefficients(WindowFunction function) const noexcept {
    if (!prepared_ || std::size_t(function) >= window_function_names.size()) return {};
    return std::span(bank_).subspan(std::size_t(function) * maximum_, maximum_);
}
bool PreparedWindows::coefficients(WindowFunction function, std::span<float> output) const noexcept {
    if (!prepared_ || output.size() > maximum_ || std::size_t(function) >= window_function_names.size()) return false;
    if (output.size() == maximum_) {
        const auto prepared = coefficients(function);
        std::copy(prepared.begin(), prepared.end(), output.begin());
        return true;
    }
    const auto rotation = rotations_[output.size()];
    double cosine = 1, sine = 0;
    std::size_t i=0;
    for (auto& value : output) {
        value=window_from_cosine(i++,output.size(),function,cosine);
        const auto next = cosine * rotation.cosine - sine * rotation.sine;
        sine = sine * rotation.cosine + cosine * rotation.sine;
        cosine = next;
    }
    return true;
}
void bark_weights(std::span<float> output, float step) {
    prepare_bark_weights(output,0,step);
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
bool MagnitudeSpectrum::prepare(std::size_t n, bool radix4) {
    scratch_.clear();
    if (n < 4 || n > max_fft_samples || (n & (n - 1)) || (radix4 && (n & 0x55555555) == 0))
        return false;
    if (!detail::prepare_backend(n, radix4))
        return false;
    radix4_ = radix4;
    return scratch_.resize(n * 2);
}
static void copy_magnitude_bins(std::span<const float> scratch,std::span<float> output) noexcept {
    for(std::size_t i=0;i<output.size();++i)output[i]=scratch[2*i];
}
static void scale_fft_plane(std::span<float> scratch,std::size_t samples,int scale) noexcept {
    if(!scale)return;
    const float factor=std::scalbn(1.f,-scale);
    for(std::size_t i=0;i<samples;++i)scratch[2*i]*=factor;
}
static signal_contracts::Status prepare_fft_plane_input(const signal_contracts::NumericView& input,
    std::span<float> scratch,std::size_t plane,std::size_t samples,int& scale) noexcept {
    using signal_contracts::Status;
    const auto loaded=load_fft_plane(input,scratch,plane,samples);
    if(loaded!=Status::ok)return loaded;
    scale=fft_scale(scratch,samples);
    if(scale==INT_MIN)return Status::invalid_argument;
    scale_fft_plane(scratch,samples,scale);
    return Status::ok;
}
bool MagnitudeSpectrum::process(std::span<const float> input, std::span<float> output) {
    const auto n = scratch_.size() / 2;
    if (!n || input.size() != n || output.size() != n / 2)
        return false;
    const auto scale=fft_scale(input,n);if(scale==INT_MIN)return false;
    stage_real_fft_input(input,{scratch_.data(),scratch_.size()},scale);
    if (!detail::transform(scratch_.data(), n, radix4_))
        return false;
    const double restore = scale ? std::scalbn(1., scale) : 1.;
    // Stage results in the consumed real scratch lane before publishing.
    if(!stage_magnitude_bins({scratch_.data(),scratch_.size()},n,output.size(),restore))return false;
    copy_magnitude_bins({scratch_.data(),scratch_.size()},output);
    return true;
}
signal_contracts::Status MagnitudeSpectrum::process_plane(const signal_contracts::NumericView& input,
    signal_contracts::MutableNumericView& output, std::size_t plane) {
    using namespace signal_contracts;
    const auto n = scratch_.size() / 2;
    if (!n || input.actual[0] != n || output.actual[0] != n / 2 ||
        plane >= input.planes.size() || plane >= output.planes.size()) return Status::invalid_argument;
    int scale=0;
    const auto prepared=prepare_fft_plane_input(input,{scratch_.data(),scratch_.size()},plane,n,scale);
    if(prepared!=Status::ok)return prepared;
    if (!detail::transform(scratch_.data(), n, radix4_)) return Status::unavailable;
    const double restore = scale ? std::scalbn(1., scale) : 1.;
    if(!stage_magnitude_bins({scratch_.data(),scratch_.size()},n,n/2,restore))return Status::overflow;
    return store_magnitude_plane(output,{scratch_.data(),scratch_.size()},plane,n/2);
}
std::optional<WindowScaling> window_scaling(std::span<const float> coefficients) noexcept {
    if (coefficients.empty()) return std::nullopt;
    WindowScaling scaling{coefficients.size(), 0, 0};
    for (const double value : coefficients) {
        if (!std::isfinite(value)) return std::nullopt;
        scaling.sum += value;
        scaling.sum_squares += value * value;
    }
    if (!std::isfinite(scaling.sum) || !std::isfinite(scaling.sum_squares)) return std::nullopt;
    return scaling;
}
bool FFT::prepare(std::size_t n, bool radix4) {
    scratch_.clear();
    if (n < 4 || n > max_fft_samples || (n & (n - 1)) ||
        (radix4 && (n & 0x55555555) == 0) || !detail::prepare_backend(n, radix4)) return false;
    if(!scratch_.resize(2 * n))return false;
    radix4_ = radix4;
    return true;
}
void FFT::reset() noexcept { detail::FftStorage().swap(scratch_); }
bool FFT::process(std::span<const float> input, std::span<float> output) noexcept {
    const auto n = samples();
    if (!n || input.size() != n || output.size() != n + 2) return false;
    DSP_FFT_BEGIN(Input);
    const auto scale = fft_scale(input, n);
    DSP_FFT_END(Input);
    if (scale == INT_MIN) return false;
    DSP_FFT_BEGIN(Pack);
    const float factor = scale ? std::scalbn(1.f, -scale) : 1.f;
    for (std::size_t i = 0; i < n; ++i) {
        scratch_[2 * i] = input[i] * factor;
        scratch_[2 * i + 1] = 0;
    }
    DSP_FFT_END(Pack);
    return publish_transform(output, scale, true);
}
bool FFT::process_complex(std::span<const float> real, std::span<const float> imaginary,
                          std::span<float> output) noexcept {
    const auto n = samples();
    if (!n || real.size() != n || imaginary.size() != n || output.size() != 2 * n) return false;
    const auto real_scale = fft_scale(real, n * 2);
    const auto imaginary_scale = fft_scale(imaginary, n * 2);
    if (real_scale == INT_MIN || imaginary_scale == INT_MIN) return false;
    const auto scale = std::max(real_scale, imaginary_scale);
    const float factor = scale ? std::scalbn(1.f, -scale) : 1.f;
    for (std::size_t i = 0; i < n; ++i) {
        scratch_[2 * i] = real[i] * factor;
        scratch_[2 * i + 1] = imaginary[i] * factor;
    }
    return publish_transform(output, scale, false);
}
bool FFT::publish_transform(std::span<float> output, int scale, bool real_input) noexcept {
    const auto n = samples();
    if (!detail::transform(scratch_.data(), n, radix4_)) return false;
    DSP_FFT_BEGIN(Validate);
    if (!restore_transform(output.size(), scale, real_input)) return false;
    DSP_FFT_END(Validate);
    DSP_FFT_BEGIN(Publish);
    detail::fft_copy(scratch_.data(), output.data(), output.size());
    DSP_FFT_END(Publish);
    return true;
}
bool FFT::restore_transform(std::size_t output_size, int scale, bool real_input) noexcept {
    const auto n=samples();
    if (real_input) scratch_[1] = scratch_[n + 1] = 0;
    const double restore = scale ? std::scalbn(1., scale) : 1.;
    if (scale) for (std::size_t i = 0; i < output_size; ++i)
        scratch_[i] = float(double(scratch_[i]) * restore);
    return detail::fft_all_finite(scratch_.data(), output_size);
}
static bool prepare_rfft_storage(detail::FftStorage& scratch,detail::FftStorage& twiddles,
                                 detail::FftStorage& mixed_storage,std::size_t n,bool mixed) {
    if(!scratch.resize(n+2))return false;
    if(!twiddles.resize(n/2+2) || !mixed_storage.resize(mixed?n:0))return false;
    return true;
}
bool PreparedRfft::prepare(std::size_t n, bool radix4, Split split, const Options* options) {
    samples_ = 0;
    platform_optimizations_ = !options || options->platform_optimizations;
    if (n < 8 || n > max_fft_samples || (n & (n - 1)) ||
        (split != Split::Automatic && split != Split::Scalar)) return false;
    const auto packed = n / 2;
    const bool mixed = radix4 && (packed & 0x55555555) == 0;
    if (!detail::prepare_backend(mixed ? packed / 2 : packed, radix4)) return false;
    // The split pairs k and N/2-k share sine and opposite cosine. Keep only
    // the first quadrant; the mixed-radix butterfly uses the same symmetry.
    if(!prepare_rfft_storage(scratch_,twiddles_,mixed_,n,mixed))return false;
    if(!prepare_twiddles(n,packed,mixed,split))return false;
    radix4_ = radix4;
    mixed_radix_ = mixed;
    split_ = split;
    samples_ = n;
    return true;
}
bool PreparedRfft::prepare_twiddles(std::size_t n,std::size_t packed,bool mixed,Split split) noexcept {
    float twiddle_scale=1.f;
#if defined(DSP_FFT_FUSED)
    if(mixed && split==Split::Automatic && platform_optimizations_) twiddle_scale=.5f;
#else
    (void)mixed;(void)split;
#endif
    for (std::size_t k = 0; k <= packed / 2; ++k) {
        const auto angle = -6.2831853071795864769 * double(k) / double(n);
        twiddles_[2 * k] = twiddle_scale*float(std::cos(angle));
        twiddles_[2 * k + 1] = twiddle_scale*float(std::sin(angle));
    }
    twiddles_[packed] = 0;
    twiddles_[packed + 1] = -twiddle_scale;
    return true;
}
void PreparedRfft::reset() noexcept {
    samples_ = 0;
    detail::FftStorage().swap(scratch_);
    detail::FftStorage().swap(twiddles_);
    detail::FftStorage().swap(mixed_);
}
const char* PreparedRfft::split_implementation() const noexcept {
#if defined(DSP_FFT_FUSED)
    if(mixed_radix_ && split_==Split::Automatic && platform_optimizations_) {
#if defined(DSP_FFT_AES3)
        return "aes3-fused-f32";
#else
        return "ae32-fused-f32";
#endif
    }
#endif
    return split_ == Split::Scalar || !platform_optimizations_ ? "scalar-f32" : detail::rfft_split_implementation();
}
bool PreparedRfft::process(std::span<const float> input, std::span<float> output) noexcept {
    const auto n = samples_;
#if defined(DSP_FFT_FUSED)
    const bool fused=mixed_radix_ && split_==Split::Automatic && platform_optimizations_;
#else
    constexpr bool fused=false;
#endif
    if (!n || input.size() != n || output.size() != n + 2) return false;
    int scale=0;
    if(!transform_input(input,fused,scale))return false;
    DSP_FFT_BEGIN(Validate);
    if (!restore_output(scale)) return false;
    DSP_FFT_END(Validate);
    DSP_FFT_BEGIN(Publish);
    detail::fft_copy(scratch_.data(), output.data(), scratch_.size());
    DSP_FFT_END(Publish);
    return true;
}
bool PreparedRfft::transform_input(std::span<const float> input,bool fused,int& scale) noexcept {
    DSP_FFT_BEGIN(Input);
    scale=input_scale(input,fused);
    DSP_FFT_END(Input);
    if (scale == INT_MIN) return false;
    DSP_FFT_BEGIN(Pack);
    const float factor = scale ? std::scalbn(1.f, -scale) : 1.f;
    if (mixed_radix_) pack_mixed(input,factor,fused);
    else pack_half(input,factor,scale);
    DSP_FFT_END(Pack);
    if (!transform_prepared()) return false;
    return true;
}
int PreparedRfft::input_scale(std::span<const float> input,bool fused) noexcept {
    const auto n=samples_;
#if defined(DSP_FFT_EXTRA)
    const bool staged_input=!mixed_radix_ && platform_optimizations_;
    if(staged_input) {
        float maximum;
        // Real pairs already have contiguous complex storage. Classify while
        // copying into private storage; caller output is still untouched.
        return dsp_rfft_copy_max_ae32(input.data(),scratch_.data(),n,&maximum)
            ? fft_scale_max(maximum,n) : INT_MIN;
    }
#endif
#if defined(DSP_FFT_FUSED)
    if(fused) {
        float maximum;
        // Stage input into private storage while classifying the same words.
        // No output is published if classification or later scaling fails.
        return dsp_rfft_pack_max_ae32(input.data(),mixed_.data(),n/4,&maximum)
            ? fft_scale_max(maximum,n) : INT_MIN;
    }
#else
    (void)fused;
#endif
    return fft_scale(input,n);
}
void PreparedRfft::pack_mixed(std::span<const float> input,float factor,bool fused) noexcept {
    const auto n=samples_,packed=n/2,half=packed/2;
#if defined(DSP_FFT_FUSED)
    if(fused) {
        if(factor!=1.f) for(auto& value:mixed_) value*=factor;
        return;
    }
#else
    (void)fused;
#endif
#if defined(DSP_FFT_AE32)
    if (factor==1.f && platform_optimizations_) {
        dsp_rfft_pack_ae32(input.data(), mixed_.data(), half);
        return;
    }
#endif
    for (std::size_t i = 0; i < half; ++i) {
        mixed_[2 * i] = input[4 * i] * factor;
        mixed_[2 * i + 1] = input[4 * i + 1] * factor;
        mixed_[packed + 2 * i] = input[4 * i + 2] * factor;
        mixed_[packed + 2 * i + 1] = input[4 * i + 3] * factor;
    }
}
void PreparedRfft::pack_half(std::span<const float> input,float factor,int scale) noexcept {
    const auto n=samples_;
#if defined(DSP_FFT_EXTRA)
    if(platform_optimizations_) {
        if(scale) for(std::size_t i=0;i<n;++i) scratch_[i]*=factor;
        return;
    }
#endif
    if (!scale) detail::fft_copy(input.data(), scratch_.data(), n);
    else for (std::size_t i = 0; i < n; ++i) scratch_[i] = input[i] * factor;
}
bool PreparedRfft::restore_output(int scale) noexcept {
    if (scale) {
        const double output_scale = std::scalbn(1., scale);
        for (auto& value : scratch_) {
            value = float(double(value) * output_scale);
            if (!std::isfinite(value)) return false;
        }
        return true;
    }
    return detail::fft_all_finite(scratch_.data(), scratch_.size());
}
bool PreparedRfft::transform_prepared() noexcept {
    const auto n=samples_, packed=n/2;
#if defined(DSP_FFT_FUSED)
    const bool fused=mixed_radix_ && split_==Split::Automatic && platform_optimizations_;
#else
    constexpr bool fused=false;
#endif
    if (mixed_radix_ && !transform_mixed(packed,fused)) return false;
    if (!mixed_radix_ &&
        !detail::transform_rfft_half(scratch_.data(), packed, radix4_, platform_optimizations_)) return false;
    if(!fused)split_spectrum(packed);
    return true;
}
bool PreparedRfft::transform_mixed(std::size_t packed,bool fused) noexcept {
    const auto half=packed/2;
    if (!detail::transform_rfft_quarter(mixed_.data(), half, platform_optimizations_) ||
        !detail::transform_rfft_quarter(mixed_.data() + packed, half, platform_optimizations_)) return false;
    DSP_FFT_BEGIN(Mixed);
#if defined(DSP_FFT_FUSED)
    if(fused) detail::rfft_mixed_split(scratch_.data(), mixed_.data(), twiddles_.data(), half);
    else
#else
    (void)fused;
#endif
    mix_quarters(packed);
    DSP_FFT_END(Mixed);
    return true;
}
void PreparedRfft::mix_quarters(std::size_t packed) noexcept {
    const auto half=packed/2;
#if defined(DSP_FFT_AE32)
    if(platform_optimizations_) {
        dsp_rfft_mixed_ae32(scratch_.data(), mixed_.data(), twiddles_.data(), half);
        return;
    }
#endif
    for (std::size_t k = 0; k < half; ++k) {
        const float er = mixed_[2 * k], ei = mixed_[2 * k + 1];
        const float ar = mixed_[packed + 2 * k], ai = mixed_[packed + 2 * k + 1];
        const auto index = 2 * k <= half ? 2 * k : packed - 2 * k;
        const float wr = (2 * k <= half ? 1.f : -1.f) * twiddles_[2 * index];
        const float wi = twiddles_[2 * index + 1];
        const auto re = wr * ar - wi * ai, im = wr * ai + wi * ar;
        scratch_[2 * k] = float(er + re);
        scratch_[2 * k + 1] = float(ei + im);
        scratch_[2 * (k + half)] = float(er - re);
        scratch_[2 * (k + half) + 1] = float(ei - im);
    }
}
void PreparedRfft::split_spectrum(std::size_t packed) noexcept {
    const auto n=samples_;
    DSP_FFT_BEGIN(Split);
    const float zero_re = scratch_[0], zero_im = scratch_[1];
    scratch_[0] = float(zero_re + zero_im);
    scratch_[1] = 0;
    scratch_[n] = float(zero_re - zero_im);
    scratch_[n + 1] = 0;
    detail::rfft_split(scratch_.data(), twiddles_.data(), packed, split_ == Split::Scalar || !platform_optimizations_);
    DSP_FFT_END(Split);
}
std::span<const SpectrumProjection> amplitude_spectrum_projections() noexcept {
    static constexpr SpectrumProjection values[]{SpectrumProjection::Magnitude,SpectrumProjection::PeakAmplitude,SpectrumProjection::Power,SpectrumProjection::Psd};
    return values;
}
std::string_view spectrum_projection_name(SpectrumProjection value) noexcept {
    switch(value){
    case SpectrumProjection::Real:return "real";
    case SpectrumProjection::Imaginary:return "imaginary";
    case SpectrumProjection::Phase:return "phase";
    case SpectrumProjection::Magnitude:return "magnitude";
    case SpectrumProjection::PeakAmplitude:return "peak_amplitude";
    case SpectrumProjection::Power:return "power";
    case SpectrumProjection::Psd:return "psd";
    }return {};
}
bool parse_spectrum_projection(std::string_view text,SpectrumProjection& result) noexcept {
    for(unsigned i=0;i<=unsigned(SpectrumProjection::Psd);++i){const auto value=SpectrumProjection(i);
        if(spectrum_projection_name(value)==text){result=value;return true;}}
    return false;
}
bool is_amplitude_spectrum_projection(SpectrumProjection value) noexcept {
    const auto values=amplitude_spectrum_projections();return std::find(values.begin(),values.end(),value)!=values.end();
}

bool PreparedSpectrumPower::prepare(std::size_t input_bins,SpectrumPowerPolicy policy) noexcept {
    ready_=false;
    if(!input_bins || policy.first_bin>1 || input_bins>SIZE_MAX-policy.first_bin ||
       !std::isfinite(policy.bin_hz) || policy.bin_hz<=0 ||
       !std::isfinite(policy.coherent_gain) || policy.coherent_gain<=0 ||
       !std::isfinite(policy.power_bandwidth_hz) || policy.power_bandwidth_hz<=0 ||
       !is_amplitude_spectrum_projection(policy.representation))return false;
    input_bins_=input_bins;policy_=policy;ready_=true;return true;
}
bool PreparedSpectrumPower::convert(float value,std::size_t index,float& output) const noexcept {
    if(!std::isfinite(value) || value<0)return false;
    const bool endpoint=(policy_.first_bin==0 && index==0) ||
                        (policy_.include_nyquist && index+1==input_bins_);
    double power=value;
    if(policy_.representation==SpectrumProjection::Magnitude){
        const double calibrated=double(value)/policy_.coherent_gain;
        power=calibrated*calibrated*(endpoint?1.:2.);
    }
    else if(policy_.representation==SpectrumProjection::PeakAmplitude)
        power=double(value)*value*(endpoint?1.:.5);
    else if(policy_.representation==SpectrumProjection::Psd)
        power*=policy_.power_bandwidth_hz;
    if(!std::isfinite(power) || power>std::numeric_limits<float>::max())return false;
    output=float(power);return true;
}
bool PreparedSpectrumPower::valid_output(std::span<const float> input,std::span<float> output) const noexcept {
    if(!ready_ || input.size()!=input_bins_ || output.size()!=output_bins())return false;
    const auto before=std::less<const float*>{};
    const bool overlap=before(input.data(),output.data()+output.size()) && before(output.data(),input.data()+input.size());
    return !overlap || (policy_.first_bin==0 && input.data()==output.data());
}
bool PreparedSpectrumPower::valid_values(std::span<const float> input) const noexcept {
    float converted=0;
    for(std::size_t index=0;index<input.size();++index)
        if(!convert(input[index],index,converted))return false;
    return true;
}
bool PreparedSpectrumPower::process(std::span<const float> input,std::span<float> output) const noexcept {
    if(!valid_output(input,output))return false;
    if(!valid_values(input))return false;
    float converted=0;
    if(policy_.first_bin)output[0]=0;
    for(std::size_t index=0;index<input.size();++index){(void)convert(input[index],index,converted);output[index+policy_.first_bin]=converted;}
    return true;
}
bool PreparedSpectrumScaling::prepare(double rate,double sum,double squares,SpectrumProjectionOptions options) noexcept {
    ready_=false;
    if(spectrum_projection_name(options.representation).empty() || !std::isfinite(rate) || rate<=0 ||
       !std::isfinite(sum) || sum<=0 || !std::isfinite(squares) || squares<=0)return false;
    options_=options;amplitude_scale_=options.compensate?1/sum:1;
    power_scale_=amplitude_scale_*amplitude_scale_;psd_scale_=options.compensate?1/(rate*squares):1;
    ready_=std::isfinite(amplitude_scale_) && amplitude_scale_>0 && std::isfinite(power_scale_) && power_scale_>0 && std::isfinite(psd_scale_) && psd_scale_>0;
    return ready_;
}
bool PreparedSpectrumScaling::project(float real,float imaginary,bool paired,float& output) const noexcept {
    if(!ready_ || !std::isfinite(real) || !std::isfinite(imaginary))return false;
    const double sided=options_.compensate && paired?2.:1.;
    const double value=projected_spectrum_value(options_.representation,real,imaginary,sided,
        amplitude_scale_,power_scale_,psd_scale_);
    if(!std::isfinite(value) || std::abs(value)>std::numeric_limits<float>::max())return false;
    output=float(value);return true;
}
bool PreparedSpectrumProjection::prepare(std::size_t n,double rate,WindowScaling window,SpectrumProjectionOptions options) {
    samples_=0;
    if(n<8 || n>max_fft_samples || (n&(n-1)) || window.samples!=n || !scaling_.prepare(rate,window.sum,window.sum_squares,options))return false;
    scratch_.resize(n/2+1);samples_=n;return true;
}
bool PreparedSpectrumProjection::process(std::span<const float> input,std::span<float> output) noexcept {
    if(!samples_ || input.size()!=samples_+2 || output.size()!=samples_/2+1)return false;
    const auto first=reinterpret_cast<std::uintptr_t>(input.data()),second=reinterpret_cast<std::uintptr_t>(output.data());
    if(first<=second?second-first<input.size_bytes():first-second<output.size_bytes())return false;
    for(std::size_t k=0;k<output.size();++k)
        if(!scaling_.project(input[2*k],input[2*k+1],k!=0 && k!=samples_/2,scratch_[k]))return false;
    std::copy(scratch_.begin(),scratch_.end(),output.begin());return true;
}
static void prepare_mel_boundaries(std::span<double> boundaries,std::size_t bands,double nyquist) noexcept {
    for(std::size_t i=0;i<boundaries.size();++i)
        boundaries[i]=mel_frequency(double(i)/double(bands+1),nyquist);
}
static bool prepare_mel_storage(std::vector<double>& boundaries,std::vector<double>& weights,
                                std::size_t bins,std::size_t rate,std::size_t bands,
                                std::span<const double> frequencies_hz) {
    if(bins>max_fft_samples/2 || bands>max_mel_bands ||
       (bands && bins>SIZE_MAX/bands) ||
       (!frequencies_hz.empty() && frequencies_hz.size()!=bins))return false;
    boundaries.assign(bands+2,0);
    weights.assign(bins*bands,0);
    return (bins && rate) || frequencies_hz.empty();
}
bool MelFilterBank::prepare(std::size_t bins, std::size_t rate, std::size_t bands,
                            std::span<const double> frequencies_hz) {
    bins_ = bands_ = 0;
    boundaries_.clear();
    weights_.clear();
    if(!prepare_mel_storage(boundaries_,weights_,bins,rate,bands,frequencies_hz))return false;
    bins_ = bins;bands_ = bands;
    if (!bins || !rate)return true; // Legacy zero-rate frames produce zero Mel.
    const double nyquist = double(rate) * .5;
    prepare_mel_boundaries(boundaries_,bands,nyquist);
    if(!prepare_mel_weights(bins,rate,bands,frequencies_hz,boundaries_,weights_)){
        bins_=bands_=0;
        return false;
    }
    return true;
}
bool MelFilterBank::process(std::span<const float> input, std::span<float> output) const {
    if (input.size() != bins_ || output.size() != bands_ || boundaries_.empty() ||
        weights_.size() != bins_*bands_)
        return false;
    for(const float value:input)if(!std::isfinite(value))return false;
    for(std::size_t band=0;band<bands_;++band)
        if(!mel_band_energy(input,weights_,band,output[band]))return false;
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
    apply_stereo_window(left,right,window);
    if (!fft_.process(left, sl) || !fft_.process(right, sr))
        return false;
    apply_stereo_weights(sl,sr,weights);
    return (ml.empty() && mr.empty()) || process_mel(sl, sr, ml, mr);
}
bool StereoProcessor::process_mel(std::span<const float> left, std::span<const float> right, std::span<float> ml,
                                  std::span<float> mr) const {
    return mel_.process(left, ml) && mel_.process(right, mr);
}
} // namespace dsp_kernels
