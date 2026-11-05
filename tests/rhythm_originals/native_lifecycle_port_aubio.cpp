#include "native_lifecycle_io.h"
#include "aubio_fourier_profile.h"
#include <rhythm_reference_ports/pcm_onset_sequence.h>
#include <rhythm_reference_ports/pcm_context_tempo.h>

namespace native_lifecycle {
namespace {
namespace r=rhythm_reference_ports;
constexpr std::array onset_methods{r::OnsetRecipe::Energy,r::OnsetRecipe::FrequencyContent,r::OnsetRecipe::Complex,
    r::OnsetRecipe::Phase,r::OnsetRecipe::WeightedPhase,r::OnsetRecipe::SpectralDifference,
    r::OnsetRecipe::KullbackLeibler,r::OnsetRecipe::ModifiedKullbackLeibler,r::OnsetRecipe::SpectralFlux};
r::OnsetRecipe onset_method(std::size_t index) {
    require(index<onset_methods.size(),"explicit source onset method index");return onset_methods[index];
}
class Onset final:public Operation {
public:
    explicit Onset(Plan plan):plan_(std::move(plan)) {prepare();}
    std::size_t hop() const noexcept override {return plan_.hop;}
    Memory memory() const noexcept override {return {bound_,kernel_.workspace_bytes(),true};}
    std::unique_ptr<Operation> fresh() const override {
        auto result=std::make_unique<Onset>(plan_);require(result->controls(controls_),"fresh onset controls");return result;
    }
    bool apply(const Action& action) override;
    bool process(std::span<const float> input,Row& output) override;
private:
    bool profile(std::size_t method) {const bool ok=kernel_.apply_default_parameters(onset_method(method));capture();return ok;}
    bool geometry(const Action& action) {
        const auto saved=controls_;plan_.hop=unsigned_argument(action.values[0]);plan_.frame=unsigned_argument(action.values[1]);prepare();return controls(saved);
    }
    void prepare() {
        const auto* factory=source_fourier_profile::aubio();
        const r::PcmOnsetSequenceConfig config{{std::uint32_t(plan_.frame),std::uint32_t(plan_.hop)},std::uint32_t(plan_.rate),onset_method(plan_.method)};
        require(decltype(kernel_)::storage_bytes(config,bound_,factory) && kernel_.prepare(config,factory) && kernel_.workspace_bytes()<=bound_,"prepare/query lifecycle onset");
        capture();
    }
    void capture() {
        controls_={kernel_.threshold(),kernel_.silence_db(),double(kernel_.whitening_enabled()),kernel_.compression(),
            double(kernel_.delay_samples()),double(kernel_.minimum_interval_samples()),0,0};
    }
    bool controls(const std::array<double,8>& values) {
        const bool ok=kernel_.set_silence(float(values[1])) && kernel_.set_threshold(float(values[0])) &&
            kernel_.set_whitening(values[2]!=0) && kernel_.set_compression(float(values[3])) &&
            kernel_.set_delay_samples(unsigned_argument(values[4])) && kernel_.set_minimum_interval_samples(unsigned_argument(values[5]));
        capture();return ok;
    }
    Plan plan_;std::array<double,8> controls_{};r::PreparedPcmOnsetSequence kernel_;std::size_t bound_=0;
};
bool Onset::apply(const Action& action) {
    if(action.command==Command::ClockReset){kernel_.reset();return true;}
    if(action.command==Command::Restart){kernel_.restart();return true;}
    if(action.command==Command::OnsetControls)return controls(action.values);
    if(action.command==Command::DefaultProfile)return profile(unsigned_argument(action.values[0]));
    return action.command==Command::Geometry && geometry(action);
}
bool Onset::process(std::span<const float> input,Row& output) {
    r::PcmOnsetObservation value;if(!kernel_.process_hop(input,value))return false;
    output.value_count=9;output.integer_count=1;
    output.values[0]=value.descriptor;output.values[1]=value.peak.thresholded;output.values[2]=value.onset.peak_position;
    for(std::size_t i=0;i<6;++i)output.values[3+i]=float(controls_[i]);
    output.integers[0]=kernel_.last_onset_sample();return true;
}
class Tempo final:public Operation {
public:
    explicit Tempo(Plan plan):plan_(std::move(plan)) {prepare();}
    std::size_t hop() const noexcept override {return plan_.hop;}
    Memory memory() const noexcept override {return {bound_,kernel_.workspace_bytes(),true};}
    std::unique_ptr<Operation> fresh() const override {
        auto result=std::make_unique<Tempo>(plan_);require(result->controls(controls_),"fresh tempo controls");return result;
    }
    bool apply(const Action& action) override;
    bool process(std::span<const float> input,Row& output) override;
private:
    void prepare() {
        const auto* factory=source_fourier_profile::aubio();
        r::PcmContextTempoConfig config;config.spectrum={std::uint32_t(plan_.frame),std::uint32_t(plan_.hop)};config.sample_rate=std::uint32_t(plan_.rate);
        require(decltype(kernel_)::storage_bytes(config,bound_,factory) && kernel_.prepare(config,factory) && kernel_.workspace_bytes()<=bound_,"prepare/query lifecycle tempo");capture();
    }
    void capture() {controls_={kernel_.threshold(),kernel_.silence_db(),double(kernel_.delay_samples()),double(kernel_.tatum_signature()),0,0,0,0};}
    bool controls(const std::array<double,8>& values) {
        const bool ok=kernel_.set_threshold(float(values[0])) && kernel_.set_silence(float(values[1])) &&
            kernel_.set_delay_samples(signed_argument(values[2])) && kernel_.set_tatum_signature(unsigned_argument(values[3]));
        capture();return ok;
    }
    Plan plan_;std::array<double,8> controls_{};r::PreparedPcmContextTempo kernel_;std::size_t bound_=0;
};
bool Tempo::apply(const Action& action) {
    if(action.command==Command::Restart){kernel_.restart();return true;}
    if(action.command==Command::TempoControls)return controls(action.values);
    if(action.command!=Command::Geometry)return false;
    const auto saved=controls_;plan_.hop=unsigned_argument(action.values[0]);plan_.frame=unsigned_argument(action.values[1]);
    prepare();return controls(saved);
}
bool Tempo::process(std::span<const float> input,Row& output) {
    r::PcmContextTempoObservation value;if(!kernel_.process_hop(input,value))return false;
    output.value_count=8;output.integer_count=2;
    output.values[0]=value.beat.source_tactus;output.values[1]=kernel_.bpm();output.values[2]=kernel_.confidence();
    output.values[3]=kernel_.period_samples();output.values[4]=kernel_.last_tatum_sample_float();
    for(std::size_t i=0;i<3;++i)output.values[5+i]=float(controls_[i]);
    output.integers[0]=std::uint64_t(kernel_.last_beat_sample());output.integers[1]=kernel_.poll_tatum();return true;
}
}
std::unique_ptr<Operation> aubio(const Plan& plan) {
    if(plan.component==Component::Onset)return std::make_unique<Onset>(plan);
    if(plan.component==Component::Tempo)return std::make_unique<Tempo>(plan);
    return {};
}
}
