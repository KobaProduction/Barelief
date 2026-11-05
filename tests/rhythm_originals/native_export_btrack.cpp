#include "native_export_io.h"
#include "btrack_frontend_profile.h"
#include <rhythm_reference_ports/pcm_predictive_beat_tracker.h>
#include <sinc_backend.h>
#include <array>
#include <cstdlib>
#include <string_view>
namespace native_export {
namespace {
namespace r=rhythm_reference_ports;
struct Analysis {
    std::array<r::PreparedPcmPredictiveOnset,10> onset;
    r::PreparedPcmPredictiveBeatTracker beat;
};
void reference_workspace(const Analysis& a,r::PcmPredictiveOnsetConfig config,Output& output) {
    if(!source_fourier_profile::btrack_reference())return;
    config.method=r::PredictiveOnsetMethod::PositiveComplexDifference;std::size_t frontend=0,whole=0;
    require(r::PreparedPcmPredictiveOnset::storage_bytes(config,frontend,source_fourier_profile::btrack_factory()) &&
        r::PreparedPcmPredictiveBeatTracker::storage_bytes(desktop_resampling::sinc_backend_factory(),a.beat.frontend().config(),whole,
            source_fourier_profile::btrack_factory()),"BTrack source query");
    output.values<std::uint64_t>("reference_workspace.u64",{frontend,a.onset[6].workspace_bytes(),whole,a.beat.workspace_bytes()});
    output.row<double>("reference_window.f64",a.onset[6].reference_stage().window());
}
void prepare(Analysis& a,const Case& c,Output& out) {
    auto config=source_fourier_profile::btrack_config({c.hop,c.frame});
    require(c.window<5,"BTrack window choice");
    require(a.beat.prepare(desktop_resampling::sinc_backend_factory(),config,source_fourier_profile::btrack_factory()),"BTrack beat prepare");
    config.window=r::PredictiveWindowShape(c.window);
    for(std::size_t i=0;i<a.onset.size();++i){
        config.method=r::PredictiveOnsetMethod(i);
        require(a.onset[i].prepare(config,source_fourier_profile::btrack_factory()),"BTrack ODF prepare");
    }
    reference_workspace(a,config,out);
    if(source_fourier_profile::btrack_reference()){
        const auto window=a.onset[5].reference_stage().window();const std::vector<float> diagnostic(window.begin(),window.end());
        out.row<float>("window.f32",diagnostic);
    }else out.row<float>("window.f32",a.onset[5].spectrum_stage().window_coefficients());
}
void reference_tempo_frame(const Analysis& a,Output& output) {
    const auto& tempo=a.beat.tracker_stage().tempo_stage().reference_stage();
    if(tempo.ready()){
        output.row<double>("reference_thresholded.f64",tempo.thresholded_onsets());
        output.row<double>("reference_acf.f64",tempo.autocorrelation());
        output.row<double>("reference_comb.f64",tempo.response());
        output.row<double>("reference_delta.f64",tempo.probabilities());
    }
}
void reference_frame(const Analysis& a,Output& output) {
    if(!source_fourier_profile::btrack_reference())return;
    const auto& frontend=a.onset[6];const auto& stage=frontend.reference_stage();
    output.values<double>("reference_odf.f64",{stage.value()});
    std::array<double,10> values{};
    for(std::size_t i=0;i<values.size();++i)values[i]=a.onset[i].reference_stage().value();
    output.row<double>("reference_odfs.f64",values);
    output.row<float>("reference_fft_input.f32",stage.fft_input());
    output.row<float>("reference_complex.f32",frontend.complex_values());
    output.row<double>("reference_norm.f64",stage.magnitude());output.row<double>("reference_phase.f64",stage.phase());
    output.row<float>("reference_onset_window.f32",a.beat.onset_window());reference_tempo_frame(a,output);
}
void onset_frame(Analysis& a,std::span<const float> input,Output& out) {
    for(auto& onset:a.onset){
        float value=0;require(onset.process_hop(input,value),"BTrack ODF process");
        out.values<float>("odf.f32",{value});
    }
    out.row<float>("norm.f32",a.onset[5].magnitude());out.row<float>("phase.f32",a.onset[5].phase());
}
void beat_frame(Analysis& a,std::span<const float> input,Output& out) {
    r::PcmPredictiveBeatObservation observation;
    require(a.beat.process_hop(input,observation),"BTrack beat process");
    const auto& tracker=a.beat.tracker_stage();const auto& tempo=tracker.tempo_stage();
    out.values<std::uint64_t>("beat.u64",{std::uint64_t(observation.prediction.beat_due)});
    out.values<float>("state.f32",{observation.bpm,tracker.latest_cumulative_score(),observation.prediction.period_frames});
    out.row<float>("resampled.f32",tracker.resampled_onsets());out.row<float>("comb.f32",tempo.response());
    out.row<float>("delta.f32",tempo.state_probabilities());
}
bool tracker_only() {
    const char* value=std::getenv("BARELIEF_BTRACK_TRACKER_ONLY");
    if(!value || !*value)return false;
    require(std::string_view(value)=="1" && source_fourier_profile::btrack_reference(),"explicit source tracker-only fixture");return true;
}
void prepare_tracker(Analysis& a,const Case& c,Output& output) {
    const auto config=source_fourier_profile::btrack_config({c.hop,c.frame});const auto* fft=source_fourier_profile::btrack_factory();
    const auto& resampler=desktop_resampling::sinc_backend_factory();std::size_t frontend=0,whole=0;
    require(a.beat.prepare(resampler,config,fft) && r::PreparedPcmPredictiveOnset::storage_bytes(config,frontend,fft) &&
        r::PreparedPcmPredictiveBeatTracker::storage_bytes(resampler,config,whole,fft),"source tracker-only prepare/query");
    output.values<std::uint64_t>("reference_workspace.u64",{frontend,a.beat.frontend().workspace_bytes(),whole,a.beat.workspace_bytes()});
}
void tracker_case(const Case& c) {
    Output output(c.prefix);Analysis analysis;prepare_tracker(analysis,c,output);
    for(std::size_t position=0;position+c.hop<=c.pcm.size();position+=c.hop){
        beat_frame(analysis,std::span(c.pcm).subspan(position,c.hop),output);reference_tempo_frame(analysis,output);
    }
}
}
void btrack(const Case& c) {
    require(c.rate==44100,"BTrack source sample rate is 44100");
    if(tracker_only()){tracker_case(c);return;}
    Output output(c.prefix);Analysis analysis;prepare(analysis,c,output);
    for(std::size_t position=0;position+c.hop<=c.pcm.size();position+=c.hop){
        const auto input=std::span(c.pcm).subspan(position,c.hop);
        onset_frame(analysis,input,output);beat_frame(analysis,input,output);
        reference_frame(analysis,output);
    }
}
}
