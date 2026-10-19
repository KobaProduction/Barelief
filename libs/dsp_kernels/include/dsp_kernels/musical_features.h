#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace dsp_kernels {

// Equal temperament with explicit tuning; callers validate their MIDI/tuning bounds.
double note_frequency_hz(double midi,double a4_hz) noexcept;

enum class SpectralReduction { PassThrough, Mean, Rms, WeightedMean, Maximum };
enum class PeakWeight { Value, SquaredValue };
struct SpectralRange {
    double low_hz = 0, high_hz = 0;
    // PassThrough uses all admitted bins and requires zero (automatic count).
    std::size_t output_count = 0;
    SpectralReduction reduction = SpectralReduction::PassThrough;
};
struct MusicalSpectrogramConfig {
    bool include_broadband = true, include_notes = true;
    SpectralRange low{40, 250, 0, SpectralReduction::PassThrough};
    SpectralRange mid{250, 4000, 16, SpectralReduction::Mean};
    SpectralRange high{4000, 16000, 8, SpectralReduction::Maximum};
    int first_midi = 21, last_midi = 108;
    int assessment_first_midi = 0, assessment_last_midi = 127;
    double a4_hz = 440, cents_tolerance = 20, minimum_peak_level = 0;
    PeakWeight peak_weight = PeakWeight::SquaredValue;
};
struct PeakGridAgreement {
    double full_percent = 0, selected_percent = 0;
    bool full_defined = false, selected_defined = false;
    std::size_t peak_count = 0;
};

// Article projection: [low][mid][high][linearly interpolated note samples].
// Input Y is a finite nonnegative LINEAR spectral quantity. RMS here means
// sqrt(mean(Y^2)), never PCM RMS. No harmonic salience or hidden Y conversion.
// Ranges admit bin centers [low,high); the final range high==last input center
// includes that endpoint (Nyquist). Aggregation partitions consecutive bins equally,
// with remainder distributed by floor(j*K/Q), never duplicating a bin.
// WeightedMean requires explicit nonnegative input-sized weights at prepare;
// every resulting cell must have positive total weight. Other modes ignore them.
// Notes outside the available frequency span have value zero. Frequencies must
// strictly increase; interpolation supports explicit nonuniform coordinates.
// Full score uses ALL distinct non-DC local peaks as denominator. Selected score
// uses peaks inside selected MIDI extent expanded +/-50 cents. Each numerator
// matches its respective MIDI grid; changing selected notes cannot affect full.
// Interior peak frequency uses a three-point parabola only on uniform neighbors;
// edge peaks use their actual center. Flat plateaus count once at the first bin.
// Peak weights use Y at that bin (or its square), not an interpolated amplitude.
// prepare may allocate; process does not, and failures leave caller outputs intact.
class MusicalSpectrogram {
  public:
    bool prepare(std::span<const double> frequencies_hz,
        const MusicalSpectrogramConfig& config,
        std::span<const float> weights = {});
    bool process(std::span<const float> spectrum, std::span<float> row,
        PeakGridAgreement& agreement) noexcept;
    std::size_t output_size() const noexcept { return staging_.size(); }
    // Four counts/offsets, in low,mid,high,notes order.
    std::span<const std::size_t> counts() const noexcept { return counts_; }
    std::span<const std::size_t> offsets() const noexcept { return offsets_; }
    std::span<const double> frequencies_hz() const noexcept { return centers_; }
    std::size_t workspace_bytes() const noexcept;
  private:
    struct Cell { std::size_t begin, end; SpectralReduction reduction; double weight; };
    struct Note { std::size_t left, right; double fraction; bool available; };
    MusicalSpectrogramConfig config_;
    std::vector<double> input_frequencies_, centers_;
    std::vector<float> weights_, staging_;
    std::vector<Cell> cells_;
    std::vector<Note> notes_;
    std::size_t counts_[4]{}, offsets_[4]{};
};

enum class BandStatistic { SumEnergy, MeanPower, Rms, Maximum, Flux };

// Input is finite, nonnegative, one-sided coherent-corrected power, not PSD,
// magnitude, dB or raw complex FFT. Uniform bin centers are k * frequency_step_hz.
// SumEnergy sums bin powers; MeanPower averages bin powers; Rms is sqrt(sum).
// Maximum is the maximum bin power. Flux sums positive changes in bin power.
// Bands are [low, high), with the final upper boundary inclusive. An empty band
// produces zero. No implicit bandwidth/ENBW or loudness correction is applied.
// prepare() may allocate; process/reset/copy_state_from never allocate.
class BroadbandFeatures {
  public:
    bool prepare(std::size_t bins, double frequency_step_hz,
        std::span<const double> boundaries_hz, BandStatistic statistic);
    bool process(std::span<const float> power, std::span<float> output,
        bool discontinuity = false) noexcept;
    void reset() noexcept;
    bool copy_state_from(const BroadbandFeatures& other) noexcept;
    std::size_t workspace_bytes() const noexcept;
    std::span<const double> centers_hz() const noexcept { return centers_; }
  private:
    struct Band { std::size_t begin, end; };
    std::size_t bins_ = 0;
    double step_ = 0;
    BandStatistic statistic_ = BandStatistic::SumEnergy;
    bool has_previous_ = false;
    std::vector<Band> bands_;
    std::vector<double> boundaries_, centers_;
    std::vector<float> previous_, staging_;
};

// Experimental note salience, not note/chord recognition. Local peaks of
// sqrt(power) use three-bin parabolic interpolation and triangular pitch-grid
// agreement (+/-50 cents). Harmonics contribute with 1/h weights, normalized by
// the available weights. Partial harmonics above Nyquist are excluded. Resolution
// still depends on FFT/window geometry; interpolation adds no missing resolution.
class NoteSalience {
  public:
    bool prepare(std::size_t bins, double frequency_step_hz, int first_midi,
        std::size_t notes, double a4_hz = 440, std::size_t harmonics = 3);
    bool process(std::span<const float> power, std::span<float> output) noexcept;
    std::size_t workspace_bytes() const noexcept;
    std::span<const double> frequencies_hz() const noexcept { return frequencies_; }
  private:
    struct Harmonic { double bin, weight; std::size_t begin, end; };
    struct Cell { std::size_t begin, end; double total_weight; };
    std::size_t bins_ = 0;
    std::vector<double> frequencies_;
    std::vector<Cell> cells_;
    std::vector<Harmonic> harmonics_;
    std::vector<float> magnitude_, staging_;
};

// Fixed affine mapping q=round(clamp((x-min)/(max-min),0,1)*(2^bits-1)).
// Round ties upward; decode=min+q*(max-min)/(2^bits-1). No per-frame autoscale,
// dB transform or hidden reference. Nonfinite inputs fail before writing output.
// Caller owns disjoint typed input/output buffers. Only bits 8 and 16 accepted.
class SpectralQuantizer {
  public:
    bool prepare(double minimum, double maximum, unsigned bits) noexcept;
    bool process(std::span<const float> input, std::span<std::uint8_t> output) const noexcept;
    bool process(std::span<const float> input, std::span<std::uint16_t> output) const noexcept;
    bool decode(std::span<const std::uint8_t> input, std::span<float> output) const noexcept;
    bool decode(std::span<const std::uint16_t> input, std::span<float> output) const noexcept;
    std::size_t workspace_bytes() const noexcept { return 0; }
  private:
    double minimum_ = 0, range_ = 0;
    unsigned bits_ = 0;
};
} // namespace dsp_kernels
