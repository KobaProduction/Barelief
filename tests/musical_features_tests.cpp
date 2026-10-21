#include "dsp_kernels/dsp.h"
#include "dsp_kernels/musical_features.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <limits>
#include <new>
#include <numbers>
#include <stdexcept>

namespace {
std::atomic<bool> watch_allocations{false};
std::atomic<unsigned> allocations{0};
void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
void near(double actual, double expected, double tolerance, const char* message) {
    require(std::abs(actual - expected) <= tolerance, message);
}

// Independent direct summation, no production FFT or scaling helper.
std::vector<float> dft_power(std::span<const float> input) {
    std::vector<float> power(input.size() / 2 + 1);
    for (std::size_t k = 0; k < power.size(); ++k) {
        const auto angle = -2 * std::numbers::pi * double(k) / double(input.size());
        const auto wr = std::cos(angle), wi = std::sin(angle);
        double re = 0, im = 0, c = 1, s = 0;
        for (const auto value : input) {
            re += value * c;
            im += value * s;
            const auto next = c * wr - s * wi;
            s = s * wr + c * wi;
            c = next;
        }
        power[k] = float((k && k + 1 < power.size() ? 2 : 1) * (re * re + im * im) /
            (double(input.size()) * double(input.size())));
    }
    return power;
}

void band_statistic(const auto& power,const auto& edges,dsp_kernels::BandStatistic statistic){
    using namespace dsp_kernels;
        BroadbandFeatures processor;
        require(processor.prepare(power.size(), 16, edges, statistic), "Band prepare failed");
        std::array<float,8> result{};
        allocations = 0;
        watch_allocations = true;
        const auto success = processor.process(power, result);
        watch_allocations = false;
        require(success && allocations == 0, "Band process allocates/fails");
        for (std::size_t b = 0; b < result.size(); ++b) {
            double sum = 0, maximum = 0;
            std::size_t count = 0;
            for (std::size_t k = 0; k < power.size(); ++k) {
                if (double(k * 16) < edges[b] || (b + 1 == result.size() ? double(k * 16) > edges[b + 1] : double(k * 16) >= edges[b + 1])) continue;
                sum += power[k];
                maximum = std::max(maximum, double(power[k]));
                ++count;
            }
            const auto reference = statistic == BandStatistic::Rms ? std::sqrt(sum) : statistic == BandStatistic::Maximum ? maximum :
                statistic == BandStatistic::MeanPower && count ? sum / double(count) : sum;
            near(result[b], reference, 1e-7, "Broadband statistic differs from independent bin oracle");
            near(processor.centers_hz()[b], (edges[b] + edges[b + 1]) / 2, 0, "Hz centers incorrect");
        }
}
void band_oracle() {
    using namespace dsp_kernels;
    constexpr std::size_t n = 512;
    constexpr std::array<std::size_t, 7> tone_bins{3, 9, 19, 31, 53, 83, 113};
    constexpr std::array<double, 7> amplitudes{.11, .15, .2, .13, .17, .08, .12};
    constexpr std::array<double, 9> edges{0, 64, 160, 320, 512, 864, 1344, 1824, 4096};
    std::array<float, n> input{};
    for (std::size_t i = 0; i < n; ++i) {
        double value = .07 + (i % 2 ? -.04 : .04);
        for (std::size_t t = 0; t < tone_bins.size(); ++t)
            value += amplitudes[t] * std::sin(2 * std::numbers::pi * double(tone_bins[t] * i) / double(n) + .13 * double(t));
        input[i] = float(value);
    }
    const auto power = dft_power(input);
    double energy = 0;
    for (auto value : power) energy += value;
    double expected = .07 * .07 + .04 * .04;
    for (const auto amplitude : amplitudes) expected += amplitude * amplitude / 2;
    near(energy, expected, 1e-7, "DFT power/Parseval oracle disagrees with known tones");

    for (const auto statistic : {BandStatistic::SumEnergy, BandStatistic::MeanPower, BandStatistic::Rms, BandStatistic::Maximum}) {
        band_statistic(power,edges,statistic);
    }

    BroadbandFeatures flux, copy;
    require(flux.prepare(3, 10, std::array<double, 2>{0, 20}, BandStatistic::Flux) &&
        copy.prepare(3, 10, std::array<double, 2>{0, 20}, BandStatistic::Flux), "Flux prepare failed");
    std::array<float, 1> result{};
    require(flux.process(std::array<float, 3>{1, 2, 3}, result) && result[0] == 0, "First flux is not zero");
    allocations = 0;
    watch_allocations = true;
    const auto copied = copy.copy_state_from(flux);
    const auto processed = copy.process(std::array<float, 3>{2, 1, 5}, result);
    watch_allocations = false;
    require(copied && processed && allocations == 0 && result[0] == 3, "Positive flux/cold plan state transfer incorrect");
    require(copy.process(std::array<float, 3>{20, 10, 50}, result, true) && result[0] == 0, "Discontinuity flux not reset");
    copy.reset();
    require(copy.process(std::array<float, 3>{40, 20, 100}, result) && result[0] == 0, "Explicit flux reset failed");
    require(!copy.prepare(3, 10, std::array<double, 2>{0, 21}, BandStatistic::Flux), "Out-of-Nyquist boundary accepted");
    require(!copy.process(std::array<float, 3>{0, -1, 0}, result) && result[0] == 0, "Negative power accepted/wrote output");
    require(copy.process(std::array<float, 3>{41, 21, 101}, result) && result[0] == 3, "Invalid prepare/frame destroyed flux state");

    BroadbandFeatures overflow;
    require(overflow.prepare(3, 10, std::array<double, 2>{0, 20}, BandStatistic::SumEnergy), "Overflow plan failed");
    result[0] = 19;
    const auto large = std::numeric_limits<float>::max();
    require(!overflow.process(std::array<float, 3>{large, large, large}, result) && result[0] == 19, "Overflow output is not atomic");
    require(!overflow.prepare(3, 10, std::array<double, 3>{0, 10, 10}, BandStatistic::Rms), "Repeated boundary accepted");
}

double note_tone(dsp_kernels::PreparedRfft& rfft,dsp_kernels::NoteSalience& salience,std::vector<float>& input,std::vector<float>& raw,std::vector<float>& power,std::vector<float>& result,const auto& midi,std::size_t t){
    constexpr std::size_t n=4096;constexpr double fs=32768;
        const auto frequency = 440 * std::pow(2.0, (midi[t] - 69.0) / 12);
        const auto amplitude = .2 + .07 * double(t);
        double window_sum = 0;
        for (std::size_t i = 0; i < n; ++i) {
            const auto window = .5 - .5 * std::cos(2 * std::numbers::pi * double(i) / double(n - 1));
            window_sum += window;
            input[i] = float(amplitude * window * std::sin(2 * std::numbers::pi * frequency * double(i) / fs));
        }
        require(rfft.process(input, raw), "Tone RFFT failed");
        for (std::size_t k = 0; k < power.size(); ++k)
            power[k] = float((k && k + 1 < power.size() ? 2 : 1) * (double(raw[2 * k]) * raw[2 * k] + double(raw[2 * k + 1]) * raw[2 * k + 1]) /
                (window_sum * window_sum));
        allocations = 0;
        watch_allocations = true;
        const auto success = salience.process(power, result);
        watch_allocations = false;
        require(success && allocations == 0, "Note salience allocates/fails");
        const auto target = std::size_t(midi[t] - 48);
        const auto peak = std::size_t(std::max_element(result.begin(), result.end()) - result.begin());
        require(peak == target && result[target] > 0, "Known tone does not peak at expected note cell");
        const auto neighbor = std::max(result[target - 1], result[target + 1]);
        return double(neighbor / result[target]);
}
void note_oracle() {
    using namespace dsp_kernels;
    constexpr std::size_t n = 4096;
    constexpr double fs = 32768;
    PreparedRfft rfft;
    require(rfft.prepare(n), "Note RFFT prepare failed");
    std::vector<float> input(n), raw(n + 2), power(n / 2 + 1), result(48);
    NoteSalience salience;
    require(salience.prepare(power.size(), fs / n, 48, result.size(), 440, 3), "Note prepare failed");
    for (std::size_t i = 0; i < result.size(); ++i)
        near(salience.frequencies_hz()[i], 440 * std::pow(2.0, (double(i) + 48 - 69) / 12), 1e-10, "Note frequencies incorrect");
    // Seven sequential, changing test tones. No audio playback or recording.
    constexpr std::array<int, 7> midi{60, 64, 67, 69, 72, 76, 81};
    double worst_neighbor_ratio = 0;
    for (std::size_t t = 0; t < midi.size(); ++t) {
        worst_neighbor_ratio=std::max(worst_neighbor_ratio,note_tone(rfft,salience,input,raw,power,result,midi,t));
    }
    // Analytic parabolic magnitude peak at fractional bin 55.25; interpolation
    // must recover its location, independently of an FFT/window implementation.
    std::fill(power.begin(), power.end(), 0);
    for (std::size_t k = 54; k <= 56; ++k) {
        const double magnitude = 1 - .2 * std::pow(double(k) - 55.25, 2);
        power[k] = float(magnitude * magnitude);
    }
    NoteSalience interpolated;
    require(interpolated.prepare(power.size(), 8, 69, 1, 442, 1), "Interpolation plan failed");
    std::array<float, 1> single{};
    require(interpolated.process(power, single), "Interpolation failed");
    near(single[0], 1, 2e-6, "Parabolic peak interpolation does not recover peak");
    power[55] = std::numeric_limits<float>::quiet_NaN();
    require(!interpolated.process(power, single) && single[0] > .999, "Nonfinite note input not rejected atomically");
    require(!salience.prepare(power.size(), 8, 125, 48) && !salience.prepare(power.size(), 8, 48, 0), "Invalid note range accepted");
    std::fill(power.begin(), power.end(), 0);
    require(salience.process(power, result) && std::all_of(result.begin(), result.end(), [](auto v) { return v == 0; }), "Silence has note salience");
    std::cout << "note_grid seven_tones_peak_match=7/7 max_neighbor_ratio=" << worst_neighbor_ratio << '\n';
}

void article_reduction(dsp_kernels::MusicalSpectrogramConfig& c,const auto& axis,const auto& y,const auto& weights,dsp_kernels::SpectralReduction mode){
    using namespace dsp_kernels;
        c.mid.reduction = c.high.reduction = mode;
        MusicalSpectrogram plan;
        require(plan.prepare(axis, c, weights), "Article reduction prepare failed");
        require(plan.counts()[0] == 3 && plan.counts()[1] == 1 && plan.counts()[2] == 1 && plan.counts()[3] == 1,
            "Article four-section geometry incorrect");
        std::vector<float> row(plan.output_size(), -99);
        PeakGridAgreement score;
        allocations = 0; watch_allocations = true;
        const auto ok = plan.process(y, row, score);
        watch_allocations = false;
        require(ok && allocations == 0, "Article projection process allocates/fails");
        require(row[0] == 1 && row[1] == 3 && row[2] == 5, "Low passthrough changed values/duplicated edge");
        const auto expected_mid = mode == SpectralReduction::Mean ? 14.0/3 :
            mode == SpectralReduction::Rms ? std::sqrt(84.0/3) : mode == SpectralReduction::Maximum ? 8 : 34.0/6;
        const auto expected_high = mode == SpectralReduction::Mean ? 8 :
            mode == SpectralReduction::Rms ? std::sqrt(136.0/2) : mode == SpectralReduction::Maximum ? 10 : 74.0/9;
        near(row[3], expected_mid, 1e-6, "Article mid oracle mismatch");
        near(row[4], expected_high, 1e-6, "Article high/Nyquist oracle mismatch");
        near(row[5], 2 + 2 * (440.0-250)/750, 1e-6, "Article note is not linear interpolation of Y");
        const auto saved = row;
        auto invalid = y; invalid[1] = std::numeric_limits<float>::quiet_NaN();
        const auto saved_score = score;
        require(!plan.process(invalid, row, score) && row == saved && score.full_percent == saved_score.full_percent,
            "Invalid article frame modified outputs");
        auto bad = c; bad.mid.low_hz = 100;
        require(!plan.prepare(axis, bad, weights) && plan.process(y, row, score), "Failed prepare destroyed old article plan");
        require(!plan.process(y, std::span<float>(row).first(row.size()-1), score), "Bad output size accepted");
}
void article_projection_oracle() {
    using namespace dsp_kernels;
    // Explicit irregular axis, with a gap: no hidden assumption of bin index Hz.
    const std::array<double, 9> axis{0, 40, 100, 200, 250, 1000, 2000, 4000, 16000};
    const std::array<float, 9> y{99, 1, 3, 5, 2, 4, 8, 6, 10};
    const std::array<float, 9> weights{1, 1, 1, 1, 1, 2, 3, 4, 5};
    MusicalSpectrogramConfig c;
    c.mid.output_count = 1; c.high.output_count = 1;
    c.first_midi = c.last_midi = 69; c.a4_hz = 440;
    for (auto mode : {SpectralReduction::Mean, SpectralReduction::Rms,
        SpectralReduction::WeightedMean, SpectralReduction::Maximum}) {
        article_reduction(c,axis,y,weights,mode);
    }
    MusicalSpectrogram plan;
    c.mid.reduction = SpectralReduction::WeightedMean;
    require(!plan.prepare(axis, c), "Weighted reduction accepted absent explicit weights");
    auto zero_weights = weights; zero_weights[4] = zero_weights[5] = zero_weights[6] = 0;
    require(!plan.prepare(axis, c, zero_weights), "Zero-total weighted cell accepted");
    auto bad_axis = axis; bad_axis[3] = bad_axis[2];
    require(!plan.prepare(bad_axis, c, weights), "Duplicate coordinate accepted");
    c = {}; c.first_midi = c.last_midi = 69; c.low = {10, 20, 0, SpectralReduction::PassThrough};
    c.mid = {250, 4000, 8, SpectralReduction::Mean};
    require(plan.prepare(axis, c), "Empty cells / count greater than bins rejected");
    std::vector<float> row(plan.output_size()); PeakGridAgreement score;
    require(plan.counts()[0] == 0 && plan.process(y, row, score), "Empty low section rejected");
    require(std::count(row.begin(), row.begin()+8, 0) == 5, "Empty aggregate groups do not yield zero");
    // NoDC and note exactly at the last input coordinate: preserve endpoint.
    const std::array<double, 3> short_axis{220, 330, 440};
    const std::array<float, 3> short_y{1, 2, 3};
    c = {}; c.first_midi = 69; c.last_midi = 70;
    require(plan.prepare(short_axis, c), "Short no-DC spectrum rejected");
    row.resize(plan.output_size());
    require(plan.process(short_y, row, score) && row[row.size()-2] == 3 && row.back() == 0,
        "Last-coordinate interpolation or out-of-range note incorrect");
}

void changing_article_tones(dsp_kernels::MusicalSpectrogram& plan,dsp_kernels::MusicalSpectrogramConfig& c,std::vector<float>& row,dsp_kernels::PeakGridAgreement& score,std::array<double,101>& uniform,std::array<float,101>& values){
    using namespace dsp_kernels;
    // Seven changing synthetic spectra with note samples tested against an
    // independent piecewise linear oracle, not the old harmonic salience.
    c = {}; c.low = {40,250,0,SpectralReduction::PassThrough};
    c.first_midi=60; c.last_midi=84; c.minimum_peak_level=.01;
    require(plan.prepare(uniform,c), "Changing tone plan failed"); row.resize(plan.output_size());
    constexpr std::array<int,7> midi{60,64,67,69,72,76,81};
    for(std::size_t t=0;t<midi.size();++t) {
        const double frequency=440*std::pow(2.,(midi[t]-69.)/12);
        for(std::size_t k=0;k<values.size();++k)
            values[k]=float((.2+.1*double(t))*std::exp(-.5*std::pow((uniform[k]-frequency)/12,2)));
        allocations=0; watch_allocations=true;
        const auto ok=plan.process(values,row,score);
        watch_allocations=false;
        require(ok && allocations==0,"Changing seven-tone article projection allocated");
        for(std::size_t n=0;n<plan.counts()[3];++n) {
            const double f=440*std::pow(2.,(double(n)+60-69)/12);
            double expected=0;
            for(std::size_t k=1;k<uniform.size();++k) if(f>=uniform[k-1] && f<=uniform[k])
                expected=values[k-1]+(values[k]-values[k-1])*(f-uniform[k-1])/(uniform[k]-uniform[k-1]);
            near(row[plan.offsets()[3]+n],expected,1e-6,"Changing tone note interpolation oracle mismatch");
        }
    }
}
void pcm_article_tones(dsp_kernels::MusicalSpectrogram& plan,dsp_kernels::MusicalSpectrogramConfig& c,std::vector<float>& row,dsp_kernels::PeakGridAgreement& score){
    using namespace dsp_kernels;
    constexpr std::array<int,7> midi{60,64,67,69,72,76,81};
    // Independent DFT of seven actual PCM tones, changing pitch and level.
    constexpr std::size_t pcm_n = 256;
    std::array<float, pcm_n> pcm{};
    std::array<double, pcm_n/2+1> pcm_axis{};
    for (std::size_t k=0;k<pcm_axis.size();++k) pcm_axis[k]=double(k)*16;
    require(plan.prepare(pcm_axis,c), "PCM-derived article prepare failed");
    row.resize(plan.output_size());
    for(std::size_t t=0;t<midi.size();++t) {
        const double frequency=440*std::pow(2.,(midi[t]-69.)/12);
        for(std::size_t i=0;i<pcm.size();++i)
            pcm[i]=float((.2+.05*double(t))*std::sin(2*std::numbers::pi*frequency*double(i)/4096));
        const auto spectral_power=dft_power(pcm);
        allocations=0; watch_allocations=true;
        const auto ok=plan.process(spectral_power,row,score);
        watch_allocations=false;
        require(ok && allocations==0,"PCM-derived article process allocated/failed");
        for(std::size_t note=0;note<plan.counts()[3];++note) {
            const double f=440*std::pow(2.,(double(note)+60-69)/12);
            const auto k=std::size_t(std::floor(f/16));
            const double expected=spectral_power[k]+(spectral_power[k+1]-spectral_power[k])*(f/16-double(k));
            near(row[plan.offsets()[3]+note],expected,1e-7,"PCM DFT note interpolation oracle mismatch");
        }
    }
}
void article_peak_grid_oracle() {
    using namespace dsp_kernels;
    MusicalSpectrogramConfig c;
    c.first_midi = 69; c.last_midi = 81; c.minimum_peak_level = .05;
    c.cents_tolerance = 10;
    // Sparse, irregular spectrum isolates exact centers: no parabola across gaps.
    const std::array<double, 11> axis{0, 439, 440, 450, 600, 880, 890, 1000, 1300, 1400, 1760};
    std::array<float, 11> y{20, 0, 2, 0, 0, 3, 0, 0, 4, 0, 5};
    MusicalSpectrogram plan;
    require(plan.prepare(axis, c), "Grid plan failed");
    std::vector<float> row(plan.output_size()); PeakGridAgreement score;
    require(plan.process(y, row, score), "Grid process failed");
    // 440,880,1760 are grid matches;1300 is not within10c. DC never counted.
    near(score.full_percent, 100.0 * (4+9+25)/(4+9+16+25), 1e-9, "Full weighted all-peak oracle mismatch");
    near(score.selected_percent, 100, 1e-9, "Selected denominator includes out-of-range peaks");
    require(score.full_defined && score.selected_defined && score.peak_count == 4, "Grid flags/count wrong");
    const auto full = score.full_percent;
    c.first_midi = c.last_midi = 60;
    require(plan.prepare(axis,c), "Alternate selected range rejected"); row.resize(plan.output_size());
    require(plan.process(y,row,score) && score.full_percent == full && !score.selected_defined,
        "Displayed notes change full score or empty subset defined");
    c.first_midi = 69; c.last_midi = 81; c.peak_weight = PeakWeight::Value;
    require(plan.prepare(axis,c), "Value peak weights rejected"); row.resize(plan.output_size());
    require(plan.process(y,row,score), "Value score failed");
    near(score.full_percent, 100.0*10/14, 1e-9, "Value peak weighting not applied");
    // Every peak is considered, including between-note / out-of-selected peaks.
    y.fill(0); require(plan.process(y,row,score) && !score.full_defined && !score.selected_defined && score.peak_count == 0,
        "Silence score must be undefined");
    y[2] = .04f; require(plan.process(y,row,score) && !score.full_defined, "Below-threshold peak counted");
    y[2] = y[3] = 1; require(plan.process(y,row,score) && score.peak_count == 1, "Plateau counted repeatedly");
    // Fractional peak: parabolic refinement recovers bin44.25 =442.5Hz.
    std::array<double, 101> uniform{}; std::array<float,101> values{};
    for (std::size_t k=0;k<uniform.size();++k) uniform[k]=double(k)*10;
    for (std::size_t k=43;k<=45;++k) values[k]=float(1-.3*std::pow(double(k)-44.25,2));
    c.a4_hz=442.5; c.first_midi=c.last_midi=69; c.cents_tolerance=.001;
    require(plan.prepare(uniform,c), "Refined peak prepare failed"); row.resize(plan.output_size());
    require(plan.process(values,row,score) && score.full_percent == 100 && score.selected_percent == 100,
        "Uniform parabolic frequency refinement incorrect");
    changing_article_tones(plan,c,row,score,uniform,values);
    pcm_article_tones(plan,c,row,score);
    std::cout<<"article four_sections seven_changing_tones=7/7 noalloc grid_oracles=passed\n";
}

template<class UInt>
void quantization_oracle(unsigned bits) {
    using namespace dsp_kernels;
    SpectralQuantizer quantizer;
    require(quantizer.prepare(-.5, 1.5, bits), "Quantizer prepare failed");
    std::vector<float> input(1001), decoded(input.size());
    std::vector<UInt> encoded(input.size());
    for (std::size_t i = 0; i < input.size(); ++i) input[i] = float(-.5 + 2 * double(i) / double(input.size() - 1));
    allocations = 0;
    watch_allocations = true;
    const auto processed = quantizer.process(input, encoded);
    const auto restored = quantizer.decode(encoded, decoded);
    watch_allocations = false;
    require(processed && restored && allocations == 0, "Quantization allocates/fails");
    double error = 0;
    constexpr auto levels = std::numeric_limits<UInt>::max();
    for (std::size_t i = 0; i < input.size(); ++i) {
        const auto exact = std::floor((double(input[i]) + .5) / 2 * levels + .5);
        require(encoded[i] == exact, "Fixed affine code incorrect");
        error = std::max(error, std::abs(double(decoded[i]) - input[i]));
        near(decoded[i], input[i], 1.0 / levels + 2e-7, "Quantization exceeds half-step error bound");
    }
    std::array<float, 3> extremes{-100, .5, 100}, restore{};
    std::array<UInt, 3> values{};
    require(quantizer.process(extremes, values) && values[0] == 0 && values[1] == (levels + 1u) / 2 && values[2] == levels,
        "Saturation or tie rounding incorrect");
    require(quantizer.decode(values, restore) && restore[0] == -.5 && restore[2] == 1.5, "Decoded endpoints incorrect");
    const auto saved = values;
    extremes[1] = std::numeric_limits<float>::infinity();
    require(!quantizer.process(extremes, values) && values == saved, "Nonfinite quantization not atomic");
    require(!quantizer.prepare(1, 1, bits) && !quantizer.prepare(0, 1, 12), "Invalid quantizer accepted");
    std::cout << "quantize bits=" << bits << " max_decode_abs_error=" << error << " half_step=" << 1.0 / levels << '\n';
}

void resources() {
    using namespace dsp_kernels;
    BroadbandFeatures broad;
    NoteSalience note;
    SpectralQuantizer quantizer;
    std::array<double, 17> edges{};
    for (std::size_t i = 0; i < edges.size(); ++i) edges[i] = double(i * 1024);
    std::array<float, 2049> power{};
    for (std::size_t k = 0; k < power.size(); ++k) power[k] = float((k % 17) * .001);
    std::array<float, 16> bands{};
    std::array<float, 48> notes{};
    std::array<std::uint8_t, 48> encoded{};
    require(broad.prepare(power.size(), 8, edges, BandStatistic::Flux) && note.prepare(power.size(), 8, 48, 48) && quantizer.prepare(0, 1, 8), "Resource prepare failed");
    constexpr std::size_t iterations = 200;
    std::array<double, iterations> durations{};
    allocations = 0;
    watch_allocations = true;
    bool success = true;
    for (std::size_t i = 0; i < iterations; ++i) {
        const auto start = std::chrono::steady_clock::now();
        success = success && broad.process(power, bands) && note.process(power, notes) && quantizer.process(notes, encoded);
        durations[i] = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - start).count();
    }
    watch_allocations = false;
    require(success && allocations == 0, "Combined prepared features allocate/fail");
    std::sort(durations.begin(), durations.end());
    std::cout << "features N=4096 broad16_flux_workspace_bytes=" << broad.workspace_bytes()
        << " note48_harmonics3_workspace_bytes=" << note.workspace_bytes()
        << " quantizer_workspace_bytes=" << quantizer.workspace_bytes()
        << " history400_u8_payload_bytes=" << 400 * (16 + 48)
        << " history400_u16_payload_bytes=" << 400 * (16 + 48) * 2
        << " combined_p50_us=" << durations[iterations / 2] << " combined_p95_us=" << durations[iterations * 95 / 100]
        << " processing_allocations=" << allocations << '\n';
}
} // namespace

void* operator new(std::size_t bytes) {
    if (watch_allocations.load()) ++allocations;
    if (auto* memory = std::malloc(bytes ? bytes : 1)) return memory;
    throw std::bad_alloc();
}
void operator delete(void* memory) noexcept { std::free(memory); }
void operator delete(void* memory, std::size_t) noexcept { std::free(memory); }
int main() {
    try {
        std::cout << std::setprecision(9);
        band_oracle();
        note_oracle();
        article_projection_oracle();
        article_peak_grid_oracle();
        quantization_oracle<std::uint8_t>(8);
        quantization_oracle<std::uint16_t>(16);
        resources();
        std::cout << "Musical features numerical/resource regressions passed\n";
    } catch (const std::exception& error) {
        watch_allocations = false;
        std::cerr << error.what() << '\n';
        return 1;
    }
}
