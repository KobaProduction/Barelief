#include "native_export_io.h"
#include "aubio_fourier_profile.h"
#include <rhythm_reference_ports/pcm_onset_sequence.h>
#include <rhythm_reference_ports/pcm_context_tempo.h>
#include <array>
namespace native_export {
namespace {
namespace r=rhythm_reference_ports;
constexpr std::array methods={r::OnsetRecipe::Energy,r::OnsetRecipe::FrequencyContent,r::OnsetRecipe::Complex,
    r::OnsetRecipe::Phase,r::OnsetRecipe::WeightedPhase,r::OnsetRecipe::SpectralDifference,
    r::OnsetRecipe::KullbackLeibler,r::OnsetRecipe::ModifiedKullbackLeibler,r::OnsetRecipe::SpectralFlux};
struct Analysis {
    r::PreparedHopSpectrum spectrum;
    std::array<r::PreparedOnsetDescriptor,9> descriptor;
    std::array<r::PreparedPcmOnsetSequence,9> onset;
    r::PreparedPcmContextTempo tempo;
    std::vector<float> magnitude,phase;
};
void defaults(const r::PreparedPcmOnsetSequence& onset,Output& out) {
    out.values<float>("onset_defaults.f32",{onset.threshold(),onset.silence_db(),onset.compression(),
        float(onset.whitening_enabled()),float(onset.minimum_interval_samples()),float(onset.delay_samples())});
}
void prepare(Analysis& a,const Case& c,Output& out) {
    const auto* factory=source_fourier_profile::aubio();
    r::HopSpectrumConfig spectrum{c.frame,c.hop};
    r::PcmContextTempoConfig tempo;tempo.spectrum=spectrum;tempo.sample_rate=c.rate;
    require(a.spectrum.prepare(spectrum,{},factory) && a.tempo.prepare(tempo,factory),"aubio frontend prepare");
    a.magnitude.resize(a.spectrum.bins());a.phase.resize(a.spectrum.bins());
    for(std::size_t i=0;i<methods.size();++i){
        require(a.descriptor[i].prepare(a.spectrum.bins(),methods[i]),"aubio descriptor prepare");
        require(a.onset[i].prepare({spectrum,c.rate,methods[i]},factory),"aubio onset prepare");
        defaults(a.onset[i],out);
    }
    out.row<float>("window.f32",a.spectrum.window_coefficients());
}
void onset_frame(Analysis& a,std::span<const float> input,Output& out) {
    for(std::size_t i=0;i<methods.size();++i){
        float value=0;r::PcmOnsetObservation observation;
        require(a.descriptor[i].process(a.magnitude,a.phase,value),"aubio descriptor process");
        out.values<float>("odf.f32",{value});
        require(a.onset[i].process_hop(input,observation),"aubio onset process");
        out.values<float>("onsets.f32",{observation.descriptor,observation.peak.thresholded,observation.onset.peak_position});
        out.values<std::uint64_t>("onset_last.u64",{a.onset[i].last_onset_sample()});
    }
}
void tempo_frame(Analysis& a,std::span<const float> input,Output& out) {
    r::PcmContextTempoObservation observation;
    require(a.tempo.process_hop(input,observation),"aubio tempo process");
    out.values<float>("tempo.f32",{observation.beat.source_tactus,a.tempo.bpm(),a.tempo.confidence(),
        a.tempo.period_samples(),a.tempo.last_tatum_sample_float()});
    out.values<std::uint64_t>("tempo_last.u64",{std::uint64_t(a.tempo.last_beat_sample())});
    out.values<std::uint64_t>("tatum.u64",{a.tempo.poll_tatum()});
}
}
void aubio(const Case& c) {
    Output output(c.prefix);Analysis analysis;prepare(analysis,c,output);
    for(std::size_t position=0;position+c.hop<=c.pcm.size();position+=c.hop){
        const auto input=std::span(c.pcm).subspan(position,c.hop);
        require(analysis.spectrum.process_hop(input,analysis.magnitude,analysis.phase),"aubio spectrum process");
        output.row<float>("norm.f32",analysis.magnitude);output.row<float>("phase.f32",analysis.phase);
        onset_frame(analysis,input,output);tempo_frame(analysis,input,output);
    }
}
}
