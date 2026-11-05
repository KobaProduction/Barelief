#include "native_lifecycle_io.h"
#include "btrack_frontend_profile.h"
#include <rhythm_reference_ports/pcm_predictive_beat_tracker.h>
#include <sinc_backend.h>
#include <algorithm>

namespace native_lifecycle {
namespace {
namespace r=rhythm_reference_ports;
r::PcmPredictiveOnsetConfig configuration(const Plan& plan) {
    return source_fourier_profile::btrack_config({std::uint32_t(plan.hop),std::uint32_t(plan.frame),
        r::PredictiveOnsetMethod(plan.method),r::PredictiveWindowShape(plan.window)});
}
class Odf final:public Operation {
public:
    explicit Odf(Plan plan):plan_(std::move(plan)) {prepare();}
    std::size_t hop() const noexcept override {return plan_.hop;}
    Memory memory() const noexcept override {return {bound_,kernel_.workspace_bytes(),true};}
    std::unique_ptr<Operation> fresh() const override {return std::make_unique<Odf>(plan_);}
    bool apply(const Action& action) override {
        if(action.command==Command::Restart){kernel_.restart();return true;}
        if(action.command==Command::Method){plan_.method=unsigned_argument(action.values[0]);return kernel_.set_method(r::PredictiveOnsetMethod(plan_.method));}
        if(action.command!=Command::Geometry)return false;
        plan_.hop=unsigned_argument(action.values[0]);plan_.frame=unsigned_argument(action.values[1]);prepare();return true;
    }
    bool process(std::span<const float> input,Row& output) override {
        float value=0;if(!kernel_.process_hop(input,value))return false;
        output.value_count=1;output.integer_count=4;output.values[0]=value;
        output.integers={plan_.method,plan_.window,plan_.frame,plan_.hop,0,0,0,0};return true;
    }
private:
    void prepare() {
        const auto config=configuration(plan_);
        const auto* factory=source_fourier_profile::btrack_factory();
        require(decltype(kernel_)::storage_bytes(config,bound_,factory) && kernel_.prepare(config,factory) && kernel_.workspace_bytes()<=bound_,"prepare/query lifecycle ODF");
    }
    Plan plan_;r::PreparedPcmPredictiveOnset kernel_;std::size_t bound_=0;
};
class Tracker final:public Operation {
public:
    explicit Tracker(Plan plan):plan_(std::move(plan)) {
        require(plan_.rate==44100 && plan_.method==6 && plan_.window==1,"source BTrack whole recipe controls");
        require(kernel_.prepare(desktop_resampling::sinc_backend_factory(),configuration(plan_),source_fourier_profile::btrack_factory()),"prepare lifecycle BTrack");
        query();
    }
    std::size_t hop() const noexcept override {return plan_.hop;}
    Memory memory() const noexcept override {return {bound_,kernel_.workspace_bytes(),true};}
    std::unique_ptr<Operation> fresh() const override {return std::make_unique<Tracker>(plan_);}
    bool apply(const Action& action) override {
        if(action.command==Command::Restart)return kernel_.restart();
        if(action.command==Command::SetTempo)return kernel_.set_tempo(float(action.values[0]));
        if(action.command==Command::FixTempo)return kernel_.fix_tempo(float(action.values[0]));
        if(action.command==Command::ReleaseTempo){kernel_.release_tempo();return true;}
        return action.command==Command::Geometry && geometry(action);
    }
    bool process(std::span<const float> input,Row& output) override;
private:
    bool geometry(const Action& action) {
        const auto hop=unsigned_argument(action.values[0]),frame=unsigned_argument(action.values[1]);
        if(!kernel_.update_hop_and_frame(hop,frame,source_fourier_profile::btrack_factory()))return false;
        plan_.hop=hop;plan_.frame=frame;query();return true;
    }
    void query() {
        require(decltype(kernel_)::storage_bytes(desktop_resampling::sinc_backend_factory(),configuration(plan_),bound_,
            source_fourier_profile::btrack_factory()) && kernel_.workspace_bytes()<=bound_,"query lifecycle BTrack workspace");
    }
    Plan plan_;r::PreparedPcmPredictiveBeatTracker kernel_;std::size_t bound_=0;
};
bool Tracker::process(std::span<const float> input,Row& output) {
    r::PcmPredictiveBeatObservation value;if(!kernel_.process_hop(input,value))return false;
    const auto& tracker=kernel_.tracker_stage();const auto& tempo=tracker.tempo_stage();
    output.value_count=684;output.integer_count=4;
    output.values[0]=value.bpm;output.values[1]=tracker.latest_cumulative_score();output.values[2]=value.prediction.period_frames;
    std::copy(tracker.resampled_onsets().begin(),tracker.resampled_onsets().end(),output.values.begin()+3);
    std::copy(tempo.response().begin(),tempo.response().end(),output.values.begin()+515);
    std::copy(tempo.state_probabilities().begin(),tempo.state_probabilities().end(),output.values.begin()+643);
    output.integers={std::uint64_t(value.prediction.beat_due),plan_.hop,std::uint64_t(value.prediction.next_beat),std::uint64_t(value.prediction.next_prediction),0,0,0,0};return true;
}
}
std::unique_ptr<Operation> btrack(const Plan& plan) {
    if(plan.component==Component::Odf)return std::make_unique<Odf>(plan);
    if(plan.component==Component::Tracker)return std::make_unique<Tracker>(plan);
    return {};
}
}
