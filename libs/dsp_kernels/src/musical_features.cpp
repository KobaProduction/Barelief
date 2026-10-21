#include "dsp_kernels/musical_features.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>

namespace dsp_kernels {
double note_frequency_hz(double midi,double a4_hz) noexcept {
    return a4_hz*std::exp2((midi-69.0)/12);
}
namespace {
constexpr std::size_t maximum_bins = 1u << 20;
constexpr std::size_t maximum_bands = 4096;
bool valid_geometry(std::size_t bins, double step) noexcept {
    return bins >= 2 && bins <= maximum_bins && std::isfinite(step) && step > 0 &&
        std::isfinite(double(bins - 1) * step);
}
bool valid_power(std::span<const float> power) noexcept {
    return std::all_of(power.begin(), power.end(), [](float v) { return std::isfinite(v) && v >= 0; });
}
bool valid_statistic(BandStatistic statistic) noexcept {
    switch (statistic) {
    case BandStatistic::SumEnergy: case BandStatistic::MeanPower: case BandStatistic::Rms:
    case BandStatistic::Maximum: case BandStatistic::Flux: return true;
    }
    return false;
}
template<class UInt>
bool quantize(std::span<const float> input, std::span<UInt> output,
    double minimum, double range, unsigned bits) noexcept {
    if (bits != sizeof(UInt) * 8 || input.size() != output.size() ||
        !std::all_of(input.begin(), input.end(), [](float v) { return std::isfinite(v); })) return false;
    constexpr double levels = std::numeric_limits<UInt>::max();
    for (std::size_t i = 0; i < input.size(); ++i) {
        const auto normalized = std::clamp((double(input[i]) - minimum) / range, 0.0, 1.0);
        output[i] = static_cast<UInt>(std::floor(normalized * levels + 0.5));
    }
    return true;
}
template<class UInt>
bool dequantize(std::span<const UInt> input, std::span<float> output,
    double minimum, double range, unsigned bits) noexcept {
    if (bits != sizeof(UInt) * 8 || input.size() != output.size()) return false;
    constexpr double levels = std::numeric_limits<UInt>::max();
    for (std::size_t i = 0; i < input.size(); ++i)
        output[i] = static_cast<float>(minimum + double(input[i]) / levels * range);
    return true;
}
} // namespace

namespace {
bool valid_musical_settings(std::span<const double> frequencies,const MusicalSpectrogramConfig& config) noexcept {
    if (frequencies.size() < 2 || frequencies.size() > maximum_bins ||
        config.first_midi < 0 || config.last_midi > 127 || config.first_midi > config.last_midi ||
        config.assessment_first_midi < 0 || config.assessment_last_midi > 127 ||
        config.assessment_first_midi > config.assessment_last_midi ||
        !std::isfinite(config.a4_hz) || config.a4_hz <= 0 ||
        !std::isfinite(config.cents_tolerance) || config.cents_tolerance < 0 || config.cents_tolerance > 50 ||
        !std::isfinite(config.minimum_peak_level) || config.minimum_peak_level < 0 ||
        (config.peak_weight != PeakWeight::Value && config.peak_weight != PeakWeight::SquaredValue)) return false;
    return true;
}

}
namespace {
bool valid_musical_coordinates(std::span<const double> frequencies,std::span<const float> weights) noexcept {
    for (std::size_t i = 0; i < frequencies.size(); ++i)
        if (!std::isfinite(frequencies[i]) || frequencies[i] < 0 ||
            (i && frequencies[i] <= frequencies[i - 1])) return false;
    if (!weights.empty() && (weights.size() != frequencies.size() || !valid_power(weights))) return false;
    return true;
}

}
namespace {
bool valid_range_reduction(const SpectralRange& r,std::span<const float> weights) noexcept {
        switch (r.reduction) {
        case SpectralReduction::PassThrough: if (r.output_count) return false; break;
        case SpectralReduction::WeightedMean: if (weights.empty()) return false; [[fallthrough]];
        case SpectralReduction::Mean: case SpectralReduction::Rms: case SpectralReduction::Maximum:
            if (!r.output_count) return false;
            break;
        default: return false;
        }
    return true;
}

}
namespace {
bool valid_musical_ranges(const MusicalSpectrogramConfig& config,std::span<const float> weights,std::span<const SpectralRange> ranges) noexcept {
    for (std::size_t i = 0; config.include_broadband && i < 3; ++i) {
        const auto& r = ranges[i];
        if (!std::isfinite(r.low_hz) || !std::isfinite(r.high_hz) || r.low_hz < 0 ||
            r.low_hz >= r.high_hz || (i && r.low_hz < ranges[i - 1].high_hz) ||
            r.output_count > maximum_bands) return false;
        if(!valid_range_reduction(r,weights))return false;
    }
    return true;
}

}
bool MusicalSpectrogram::prepare_cells(std::span<const double> frequencies,std::span<const float> weights,const SpectralRange& r,std::size_t begin,std::size_t size,std::size_t count) {
        for (std::size_t j = 0; j < count; ++j) {
            const auto lo = begin + j * size / count, hi = begin + (j + 1) * size / count;
            double total = 0;
            if (r.reduction == SpectralReduction::WeightedMean) {
                for (auto k = lo; k < hi; ++k) total += weights[k];
                if (lo != hi && !(total > 0)) return false;
            }
            cells_.push_back({lo, hi, r.reduction, total});
            centers_.push_back(lo < hi ? frequencies[lo] + (frequencies[hi - 1] - frequencies[lo]) / 2 :
                r.low_hz + (r.high_hz - r.low_hz) * (double(j) + .5) / double(count));
        }
    return true;
}
bool MusicalSpectrogram::prepare_broadband(std::span<const double> frequencies,std::span<const float> weights,std::span<const SpectralRange> ranges) {
    for (std::size_t section = 0; config_.include_broadband && section < 3; ++section) {
        const auto& r = ranges[section];
        const auto begin = std::size_t(std::lower_bound(frequencies.begin(), frequencies.end(), r.low_hz) - frequencies.begin());
        const auto end = std::size_t((section == 2 && r.high_hz == frequencies.back() ?
            std::upper_bound(frequencies.begin(), frequencies.end(), r.high_hz) :
            std::lower_bound(frequencies.begin(), frequencies.end(), r.high_hz)) - frequencies.begin());
        const auto size = end - begin;
        const auto count = r.reduction == SpectralReduction::PassThrough ? size : r.output_count;
        // Empty groups yield zero; fixed output counts remain stable even when
        // the available FFT resolution is lower than the requested detail.
        offsets_[section] = cells_.size();
        counts_[section] = count;
        if(!prepare_cells(frequencies,weights,r,begin,size,count))return false;
    }
    return true;
}
bool MusicalSpectrogram::prepare_note(std::span<const double> frequencies,int midi) {
        const auto f = note_frequency_hz(midi,config_.a4_hz);
        if (!std::isfinite(f) || f <= 0) return false;
        centers_.push_back(f);
        if (f < frequencies.front() || f > frequencies.back()) {
            notes_.push_back({0, 0, 0, false}); return true;
        }
        auto hi = std::size_t(std::lower_bound(frequencies.begin(), frequencies.end(), f) - frequencies.begin());
        if (frequencies[hi] == f) notes_.push_back({hi, hi, 0, true});
        else notes_.push_back({hi - 1, hi, (f - frequencies[hi - 1]) / (frequencies[hi] - frequencies[hi - 1]), true});
    return true;
}
bool MusicalSpectrogram::prepare_notes(std::span<const double> frequencies) {
    offsets_[3] = cells_.size();
    counts_[3] = config_.include_notes ? std::size_t(config_.last_midi - config_.first_midi + 1) : 0;
    for (int midi = config_.first_midi; config_.include_notes && midi <= config_.last_midi; ++midi) {
        if(!prepare_note(frequencies,midi))return false;
    }
    return true;
}
bool MusicalSpectrogram::prepare_plan(std::span<const double> frequencies,const MusicalSpectrogramConfig& config,std::span<const float> weights,std::span<const SpectralRange> ranges) {
    config_ = config;
    input_frequencies_.assign(frequencies.begin(), frequencies.end());
    if (!weights.empty()) weights_.assign(weights.begin(), weights.end());
    if(!prepare_broadband(frequencies,weights,ranges))return false;
    if(!prepare_notes(frequencies))return false;
    staging_.resize(cells_.size() + notes_.size());
    return true;
}
bool MusicalSpectrogram::prepare(std::span<const double> frequencies,
    const MusicalSpectrogramConfig& config, std::span<const float> weights) {
    if(!valid_musical_settings(frequencies,config))return false;
    if(!valid_musical_coordinates(frequencies,weights))return false;
    const SpectralRange ranges[]{config.low, config.mid, config.high};
    if(!valid_musical_ranges(config,weights,ranges))return false;
    // All mutations target a candidate: failed validation preserves the old plan.
    MusicalSpectrogram c;
    if(!c.prepare_plan(frequencies,config,weights,ranges))return false;
    *this = std::move(c);
    return true;
}

double MusicalSpectrogram::accumulate_cell(std::span<const float> y,const Cell& cell) const noexcept {
        double value = 0;
        for (auto k = cell.begin; k < cell.end; ++k) {
            const double v = y[k];
            switch (cell.reduction) {
            case SpectralReduction::Maximum: value = std::max(value, v); break;
            case SpectralReduction::Rms: value += v * v; break;
            case SpectralReduction::WeightedMean: value += v * weights_[k]; break;
            default: value += v; break;
            }
        }
    return value;
}
double MusicalSpectrogram::normalize_cell(double value,const Cell& cell) const noexcept {
        if (cell.begin != cell.end) {
            if (cell.reduction == SpectralReduction::Mean || cell.reduction == SpectralReduction::Rms)
                value /= double(cell.end - cell.begin);
            if (cell.reduction == SpectralReduction::Rms) value = std::sqrt(value);
            if (cell.reduction == SpectralReduction::WeightedMean) value /= cell.weight;
        }
    return value;
}
bool MusicalSpectrogram::process_cells(std::span<const float> y) noexcept {
    for (std::size_t i = 0; i < cells_.size(); ++i) {
        const auto& cell = cells_[i];
        double value=accumulate_cell(y,cell);
        value=normalize_cell(value,cell);
        if (!std::isfinite(value) || value > std::numeric_limits<float>::max()) return false;
        staging_[i] = float(value);
    }
    return true;
}
void MusicalSpectrogram::process_notes(std::span<const float> y) noexcept {
    for (std::size_t i = 0; i < notes_.size(); ++i) {
        const auto& n = notes_[i];
        staging_[cells_.size() + i] = n.available ?
            float((1 - n.fraction) * y[n.left] + n.fraction * y[n.right]) : 0;
    }

}
namespace {
double interpolated_peak_frequency(std::span<const float> y,std::span<const double> input_frequencies_,std::size_t k,double frequency) noexcept {
        if (k && k + 1 < y.size() && y[k] > y[k + 1]) {
            const auto left_step = frequency - input_frequencies_[k - 1];
            const auto right_step = input_frequencies_[k + 1] - frequency;
            if (std::abs(left_step - right_step) <= 1e-9 * std::max(left_step, right_step)) {
                const double denominator = double(y[k - 1]) - 2 * double(y[k]) + y[k + 1];
                if (denominator < 0) frequency += left_step * std::clamp(
                    .5 * (double(y[k - 1]) - y[k + 1]) / denominator, -.5, .5);
            }
        }
    return frequency;
}

}
bool MusicalSpectrogram::is_grid_peak(std::span<const float> y,std::size_t k) const noexcept {
        if (input_frequencies_[k] == 0 || y[k] <= config_.minimum_peak_level) return false;
        // Strict rise and non-strict fall: select the leftmost flat top once.
        if (k && y[k] <= y[k - 1]) return false;
        auto plateau_end = k + 1;
        while (plateau_end < y.size() && y[plateau_end] == y[k]) ++plateau_end;
        if (!k && plateau_end == y.size()) return false; // constant spectrum is not a peak
        if (plateau_end < y.size() && y[k] < y[plateau_end]) return false;
    return true;
}
namespace {
struct GridWeights {double full_weight=0,selected_weight=0,full_match=0,selected_match=0;};
double nearest_grid_note(double m,int first,int last) noexcept {return std::clamp(std::round(m),double(first),double(last));}
void score_grid_peak(const MusicalSpectrogramConfig& config_,float level,double frequency,GridWeights& totals) noexcept {
        const double weight = config_.peak_weight == PeakWeight::Value ? double(level) : double(level) * level;
        const double midi = 69 + 12 * std::log2(frequency / config_.a4_hz);
        totals.full_weight += weight;
        if (std::abs(midi - nearest_grid_note(midi, config_.assessment_first_midi, config_.assessment_last_midi)) * 100 <= config_.cents_tolerance + 1e-9)
            totals.full_match += weight;
        if (midi >= config_.first_midi - .5 && midi <= config_.last_midi + .5) {
            totals.selected_weight += weight;
            if (std::abs(midi - nearest_grid_note(midi, config_.first_midi, config_.last_midi)) * 100 <= config_.cents_tolerance + 1e-9)
                totals.selected_match += weight;
        }
}
}
namespace {
void finish_grid_agreement(const GridWeights& totals,PeakGridAgreement& result) noexcept {
    result.full_defined = totals.full_weight > 0;
    result.selected_defined = totals.selected_weight > 0;
    if (result.full_defined) result.full_percent = 100 * totals.full_match / totals.full_weight;
    if (result.selected_defined) result.selected_percent = 100 * totals.selected_match / totals.selected_weight;
}
}
PeakGridAgreement MusicalSpectrogram::grid_agreement(std::span<const float> y) const noexcept {
    PeakGridAgreement result;
    GridWeights totals;
    for (std::size_t k = 0; config_.include_notes && k < y.size(); ++k) {
        if(!is_grid_peak(y,k))continue;
        double frequency = input_frequencies_[k];
        frequency=interpolated_peak_frequency(y,input_frequencies_,k,frequency);
        if (frequency <= 0) continue;
        score_grid_peak(config_,y[k],frequency,totals);
        ++result.peak_count;
    }
    finish_grid_agreement(totals,result);
    return result;
}
bool MusicalSpectrogram::process(std::span<const float> y, std::span<float> row,
    PeakGridAgreement& agreement) noexcept {
    if (input_frequencies_.empty() || y.size() != input_frequencies_.size() ||
        row.size() != staging_.size() || !valid_power(y)) return false;
    if(!process_cells(y))return false;
    process_notes(y);
    const auto result=grid_agreement(y);
    std::copy(staging_.begin(), staging_.end(), row.begin());
    agreement = result;
    return true;
}

std::size_t MusicalSpectrogram::workspace_bytes() const noexcept {
    return (input_frequencies_.capacity() + centers_.capacity()) * sizeof(double) +
        (weights_.capacity() + staging_.capacity()) * sizeof(float) +
        cells_.capacity() * sizeof(Cell) + notes_.capacity() * sizeof(Note);
}

namespace {
bool valid_band_boundaries(std::size_t bins,double frequency_step_hz,std::span<const double> boundaries_hz) noexcept {
    const auto nyquist = double(bins - 1) * frequency_step_hz;
    for (std::size_t i = 0; i < boundaries_hz.size(); ++i)
        if (!std::isfinite(boundaries_hz[i]) || boundaries_hz[i] < 0 || boundaries_hz[i] > nyquist ||
            (i && boundaries_hz[i] <= boundaries_hz[i - 1])) return false;
    return true;
}

}
void BroadbandFeatures::prepare_bands(std::size_t bins,double frequency_step_hz,std::span<const double> boundaries_hz) {
    std::size_t bin = 0;
    for (std::size_t i = 0; i < bands_.size(); ++i) {
        while (bin < bins && double(bin) * frequency_step_hz < boundaries_hz[i]) ++bin;
        const auto begin = bin;
        const bool last = i + 1 == bands_.size();
        while (bin < bins && (last ? double(bin) * frequency_step_hz <= boundaries_hz[i + 1] :
                                    double(bin) * frequency_step_hz < boundaries_hz[i + 1])) ++bin;
        bands_[i] = {begin, bin};
        centers_[i] = boundaries_hz[i] + (boundaries_hz[i + 1] - boundaries_hz[i]) / 2;
    }

}
bool BroadbandFeatures::prepare(std::size_t bins, double frequency_step_hz,
    std::span<const double> boundaries_hz, BandStatistic statistic) {
    if (!valid_geometry(bins, frequency_step_hz) || !valid_statistic(statistic) ||
        boundaries_hz.size() < 2 || boundaries_hz.size() > maximum_bands + 1) return false;
    if(!valid_band_boundaries(bins,frequency_step_hz,boundaries_hz))return false;

    BroadbandFeatures candidate;
    candidate.bins_ = bins;
    candidate.step_ = frequency_step_hz;
    candidate.statistic_ = statistic;
    candidate.boundaries_.assign(boundaries_hz.begin(), boundaries_hz.end());
    candidate.bands_.resize(boundaries_hz.size() - 1);
    candidate.centers_.resize(candidate.bands_.size());
    candidate.staging_.resize(candidate.bands_.size());
    if (statistic == BandStatistic::Flux) candidate.previous_.resize(bins);
    // Compare actual bin centers rather than rounding frequency ratios near an
    // exact boundary. Each admitted bin belongs to at most one band.
    candidate.prepare_bands(bins,frequency_step_hz,boundaries_hz);
    *this = std::move(candidate);
    return true;
}

void BroadbandFeatures::accumulate_band(std::span<const float> power,std::size_t k,bool discontinuity,bool flux,double& value) const noexcept {
            if (statistic_ == BandStatistic::Maximum) value = std::max(value, double(power[k]));
            else if (flux) {
                if (has_previous_ && !discontinuity) value += std::max(0.0, double(power[k]) - previous_[k]);
            } else value += power[k];
}
bool BroadbandFeatures::process_band(std::span<const float> power,std::size_t i,bool discontinuity,bool flux) noexcept {
        const auto [begin, end] = bands_[i];
        double value = 0;
        for (std::size_t k = begin; k < end; ++k) {
            accumulate_band(power,k,discontinuity,flux,value);
        }
        if (statistic_ == BandStatistic::MeanPower && end > begin) value /= double(end - begin);
        if (statistic_ == BandStatistic::Rms) value = std::sqrt(value);
        if (value > std::numeric_limits<float>::max()) return false;
        staging_[i] = static_cast<float>(value);
    return true;
}
bool BroadbandFeatures::process(std::span<const float> power, std::span<float> output,
    bool discontinuity) noexcept {
    if (!bins_ || power.size() != bins_ || output.size() != bands_.size() || !valid_power(power)) return false;
    const bool flux = statistic_ == BandStatistic::Flux;
    for (std::size_t i = 0; i < bands_.size(); ++i) {
        if(!process_band(power,i,discontinuity,flux))return false;
    }
    if (flux) {
        std::copy(power.begin(), power.end(), previous_.begin());
        has_previous_ = true;
    }
    std::copy(staging_.begin(), staging_.end(), output.begin());
    return true;
}

void BroadbandFeatures::reset() noexcept { has_previous_ = false; }

bool BroadbandFeatures::copy_state_from(const BroadbandFeatures& other) noexcept {
    if (!bins_ || bins_ != other.bins_ || step_ != other.step_ || statistic_ != other.statistic_ ||
        boundaries_ != other.boundaries_) return false;
    std::copy(other.previous_.begin(), other.previous_.end(), previous_.begin());
    has_previous_ = other.has_previous_;
    return true;
}

std::size_t BroadbandFeatures::workspace_bytes() const noexcept {
    return bands_.capacity() * sizeof(Band) + (boundaries_.capacity() + centers_.capacity()) * sizeof(double) +
        (previous_.capacity() + staging_.capacity()) * sizeof(float);
}

void NoteSalience::prepare_harmonics(double frequency,double frequency_step_hz,std::size_t harmonics,Cell& cell) {
    constexpr double half_semitone = 1.029302236643492;
        for (std::size_t h = 1; h <= harmonics; ++h) {
            const auto target = frequency * double(h) / frequency_step_hz;
            if (!std::isfinite(target) || target > double(bins_ - 1)) break;
            const auto low = std::max(1.0, std::floor(target / half_semitone) - 1);
            const auto high = std::min(double(bins_ - 1), std::ceil(target * half_semitone) + 1);
            const auto weight = 1.0 / double(h);
            harmonics_.push_back({target, weight, std::size_t(low), std::size_t(high) + 1});
            cell.total_weight += weight;
        }

}
bool NoteSalience::prepare_notes(std::size_t notes,int first_midi,double a4_hz,double nyquist,double frequency_step_hz,std::size_t harmonics) {
    for (std::size_t i = 0; i < notes; ++i) {
        const auto frequency = note_frequency_hz(double(first_midi)+double(i),a4_hz);
        if (!std::isfinite(frequency) || frequency <= 0 || frequency > nyquist) return false;
        frequencies_[i] = frequency;
        Cell cell{harmonics_.size(), 0, 0};
        prepare_harmonics(frequency,frequency_step_hz,harmonics,cell);
        cell.end = harmonics_.size();
        cells_[i] = cell;
    }
    return true;
}
bool NoteSalience::prepare(std::size_t bins, double frequency_step_hz, int first_midi,
    std::size_t notes, double a4_hz, std::size_t harmonics) {
    if (!valid_geometry(bins, frequency_step_hz) || first_midi < 0 || first_midi > 127 ||
        notes == 0 || notes > std::size_t(128 - first_midi) || harmonics == 0 || harmonics > 32 ||
        !std::isfinite(a4_hz) || a4_hz <= 0) return false;
    const auto nyquist = double(bins - 1) * frequency_step_hz;
    NoteSalience candidate;
    candidate.bins_ = bins;
    candidate.frequencies_.resize(notes);
    candidate.cells_.resize(notes);
    candidate.magnitude_.resize(bins);
    candidate.staging_.resize(notes);
    candidate.harmonics_.reserve(notes * harmonics);
    if(!candidate.prepare_notes(notes,first_midi,a4_hz,nyquist,frequency_step_hz,harmonics))return false;
    *this = std::move(candidate);
    return true;
}

double NoteSalience::harmonic_peak(std::size_t k,double bin) const noexcept {
                const double peak = magnitude_[k], left = magnitude_[k - 1];
                const double right = k + 1 < bins_ ? magnitude_[k + 1] : 0;
                if (peak <= left || peak < right || peak == 0) return 0;
                double shift = 0, amplitude = peak;
                if (k + 1 < bins_) {
                    const auto denominator = left - 2 * peak + right;
                    if (denominator < 0) {
                        shift = std::clamp(0.5 * (left - right) / denominator, -0.5, 0.5);
                        amplitude = peak - 0.25 * (left - right) * shift;
                    }
                }
                const auto cents = std::abs(1200 * std::log2((double(k) + shift) / bin));
                const auto agreement = std::max(0.0, 1 - cents / 50);
    return amplitude * agreement;
}
float NoteSalience::cell_salience(const Cell& cell) const noexcept {
        double salience = 0;
        for (std::size_t h = cell.begin; h < cell.end; ++h) {
            const auto& harmonic = harmonics_[h];
            double best = 0;
            for (std::size_t k = harmonic.begin; k < harmonic.end; ++k) {
                best = std::max(best,harmonic_peak(k,harmonic.bin));
            }
            salience += best * harmonic.weight;
        }
    return static_cast<float>(salience / cell.total_weight);
}
bool NoteSalience::process(std::span<const float> power, std::span<float> output) noexcept {
    if (!bins_ || power.size() != bins_ || output.size() != cells_.size() || !valid_power(power)) return false;
    for (std::size_t k = 0; k < bins_; ++k) magnitude_[k] = std::sqrt(power[k]);
    for (std::size_t i = 0; i < cells_.size(); ++i) {
        staging_[i]=cell_salience(cells_[i]);
    }
    std::copy(staging_.begin(), staging_.end(), output.begin());
    return true;
}

std::size_t NoteSalience::workspace_bytes() const noexcept {
    return frequencies_.capacity() * sizeof(double) + cells_.capacity() * sizeof(Cell) +
        harmonics_.capacity() * sizeof(Harmonic) + (magnitude_.capacity() + staging_.capacity()) * sizeof(float);
}

bool SpectralQuantizer::prepare(double minimum, double maximum, unsigned bits) noexcept {
    constexpr auto limit = double(std::numeric_limits<float>::max());
    if ((bits != 8 && bits != 16) || !std::isfinite(minimum) || !std::isfinite(maximum) ||
        minimum < -limit || maximum > limit || minimum >= maximum) return false;
    minimum_ = minimum;
    range_ = maximum - minimum;
    bits_ = bits;
    return true;
}
bool SpectralQuantizer::process(std::span<const float> input, std::span<std::uint8_t> output) const noexcept {
    return quantize(input, output, minimum_, range_, bits_);
}
bool SpectralQuantizer::process(std::span<const float> input, std::span<std::uint16_t> output) const noexcept {
    return quantize(input, output, minimum_, range_, bits_);
}
bool SpectralQuantizer::decode(std::span<const std::uint8_t> input, std::span<float> output) const noexcept {
    return dequantize(input, output, minimum_, range_, bits_);
}
bool SpectralQuantizer::decode(std::span<const std::uint16_t> input, std::span<float> output) const noexcept {
    return dequantize(input, output, minimum_, range_, bits_);
}
} // namespace dsp_kernels
