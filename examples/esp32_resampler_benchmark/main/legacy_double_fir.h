// Frozen pre-audit numerical/processing baseline, benchmark-only. A cold
// cooperative yield keeps its original slow coefficient design off the watchdog.
#pragma once
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <span>
#include <utility>
#include <vector>

namespace benchmark_legacy {
// Causal, bounded windowed-sinc interpolation on an exact rational output grid.
// Timestamps describe delivery time; the FIR delays content by 63 input samples.
// Equal rates are an exact identity, with no filter delay. No allocation in process.
class PcmResampler {
  public:
    static constexpr std::size_t taps = 128, phases = 128;
    static bool supported(std::uint32_t input, std::uint32_t output) noexcept {
        return input >= 8000 && input <= 192000 && output >= 8000 && output <= 192000 &&
               std::uint64_t(input) <= std::uint64_t(output) * 8 && std::uint64_t(output) <= std::uint64_t(input) * 8;
    }
    static std::size_t maximum_output(std::size_t input, std::uint32_t in, std::uint32_t out) noexcept {
        if (!in || !out)
            return 0;
        if (input > (UINT64_MAX - (in - 1)) / out)
            return SIZE_MAX;
        const auto count = (std::uint64_t(input) * out + in - 1) / in;
        return count >= SIZE_MAX ? SIZE_MAX : std::size_t(count) + 1;
    }
    static std::size_t storage_bytes(std::size_t channels, std::uint32_t in, std::uint32_t out) noexcept {
        return in == out ? 0 : (phases * taps + channels * taps) * sizeof(float);
    }
    bool prepare(std::uint32_t input, std::uint32_t output, std::size_t channels) {
        if (!supported(input, output) || !channels || channels > 2)
            return false;
        input_ = input;
        output_ = output;
        channels_ = channels;
        if (input != output) {
            weights_.resize(phases * taps);
            history_.resize(channels * taps);
            prepare_weights();
        } else {
            weights_.clear();
            history_.clear();
        }
        reset();
        return true;
    }
    void reset() noexcept {
        std::fill(history_.begin(), history_.end(), 0.f);
        position_ = 0;
        until_ = 0;
        fraction_ = 0;
        produced_ = 0;
        anchored_ = false;
        input_expected_ = false;
    }
    bool copy_state_from(const PcmResampler &other) noexcept {
        if (input_ != other.input_ || output_ != other.output_ || channels_ != other.channels_ ||
            history_.size() != other.history_.size())
            return false;
        std::copy(other.history_.begin(), other.history_.end(), history_.begin());
        position_ = other.position_;
        until_ = other.until_;
        fraction_ = other.fraction_;
        produced_ = other.produced_;
        anchor_ = other.anchor_;
        anchored_ = other.anchored_;
        next_input_ = other.next_input_;
        input_expected_ = other.input_expected_;
        return true;
    }
    bool input_contiguous(std::int64_t ticks) const noexcept {
        return !input_expected_ || ticks == next_input_;
    }
    bool advance_input(std::int64_t ticks, std::size_t count) noexcept {
        if (!std::in_range<std::int64_t>(count))
            return false;
        if (ticks > INT64_MAX - std::int64_t(count))
            return false;
        next_input_ = ticks + std::int64_t(count);
        input_expected_ = true;
        return true;
    }
    bool anchored() const noexcept {
        return anchored_;
    }
    void anchor(std::int64_t ticks) noexcept {
        anchor_ = ticks;
        anchored_ = true;
    }
    std::int64_t anchor_ticks() const noexcept {
        return anchor_;
    }
    std::uint64_t produced() const noexcept {
        return produced_;
    }
    // Planar staging uses explicit strides; partitioning does not affect phase/state.
    bool process(std::span<const float> input, std::size_t count, std::size_t input_stride, std::span<float> output,
                 std::size_t output_stride, std::size_t &written) noexcept {
        written = 0;
        if (!valid_input(input, count, input_stride, output, output_stride))
            return false;
        if (input_ == output_) {
            for (std::size_t c = 0; c < channels_; ++c)
                std::copy_n(input.begin() + c * input_stride, count, output.begin() + c * output_stride);
            written = count;
            produced_ += count;
            return true;
        }
        for (std::size_t i = 0; i < count; ++i) {
            for (std::size_t c = 0; c < channels_; ++c)
                history_[c * taps + position_] = input[c * input_stride + i];
            while (until_ == 0) {
                // Output time lies fraction_/output_ after the current input.
                // It is emitted at ceil(time), thus the backwards fraction is
                // zero for integral positions and 1-fraction otherwise.
                if (!write_sample(output, output_stride, written))
                    return false;
            }
            --until_;
            position_ = (position_ + 1) % taps;
        }
        return true;
    }

  private:
    bool write_sample(std::span<float> output, std::size_t output_stride, std::size_t &written) noexcept {
        const auto backwards = fraction_ ? output_ - fraction_ : 0u;
        const auto phase = std::min(phases - 1, std::size_t(std::uint64_t(backwards) * phases / output_));
        for (std::size_t c = 0; c < channels_; ++c) {
            double value = 0;
            for (std::size_t k = 0; k < taps; ++k)
                value += double(history_[c * taps + (position_ + taps - k) % taps]) * weights_[phase * taps + k];
            if (!std::isfinite(value) || std::abs(value) > 3.4028234663852886e38) {
                reset();
                return false;
            }
            output[c * output_stride + written] = float(value);
        }
        ++written;
        ++produced_;
        // Distance between ceil(previous time) and ceil(next time).
        advance_output_grid();
        return true;
    }
    void advance_output_grid() noexcept {
        const auto previous_fraction = fraction_;
        const auto total = std::uint64_t(fraction_) + input_;
        until_ = std::size_t(total / output_) + (total % output_ != 0) - (previous_fraction != 0);
        fraction_ = std::uint32_t(total % output_);
    }
    bool valid_input(std::span<const float> input, std::size_t count, std::size_t input_stride, std::span<float> output,
                     std::size_t output_stride) const noexcept {
        if (!input_ || !channels_ || input_stride < count || input_stride > input.size() / channels_ ||
            output_stride > output.size() / channels_ || output_stride < maximum_output(count, input_, output_) ||
            produced_ > UINT64_MAX - maximum_output(count, input_, output_))
            return false;
        return finite_input(input, count, input_stride);
    }
    bool finite_input(std::span<const float> input, std::size_t count, std::size_t input_stride) const noexcept {
        for (std::size_t c = 0; c < channels_; ++c)
            for (std::size_t i = 0; i < count; ++i)
                if (!std::isfinite(input[c * input_stride + i]))
                    return false;
        return true;
    }
    void prepare_weights() {
        for (std::size_t phase = 0; phase < phases; ++phase)
            prepare_phase(phase);
    }
    void prepare_phase(std::size_t phase) {
        constexpr double pi = 3.14159265358979323846;
        const double cutoff = .45 * std::min(1., double(output_) / input_);
        double sum = 0;
        if ((phase & 7) == 0)
            vTaskDelay(1); // Benchmark driver scheduling only.
        const double fraction = double(phase) / phases;
        for (std::size_t k = 0; k < taps; ++k) {
            const double distance = double(k) - 63. - fraction;
            const double x = 2. * cutoff * distance;
            const double sinc = x == 0 ? 1. : std::sin(pi * x) / (pi * x);
            const double window = .5 - .5 * std::cos(2. * pi * double(k) / (taps - 1));
            const auto value = 2. * cutoff * sinc * window;
            weights_[phase * taps + k] = float(value);
            sum += value;
        }
        for (std::size_t k = 0; k < taps; ++k)
            weights_[phase * taps + k] = float(weights_[phase * taps + k] / sum);
    }
    std::vector<float> weights_, history_;
    std::uint32_t input_ = 0, output_ = 0, fraction_ = 0;
    std::size_t channels_ = 0, position_ = 0, until_ = 0;
    std::uint64_t produced_ = 0;
    std::int64_t anchor_ = 0, next_input_ = 0;
    bool anchored_ = false, input_expected_ = false;
};
} // namespace benchmark_legacy
