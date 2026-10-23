#include <dsp_kernels/rhythm.h>
#include <iomanip>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <vector>

namespace {
namespace r=dsp_kernels::rhythm;
void number(std::optional<double> value) {std::cout<<' '<<value.value_or(std::numeric_limits<double>::quiet_NaN());}
void write_output(const r::Output& out) {
    std::cout<<unsigned(out.tempo.status);number(out.bpm);number(out.tempo.score);number(out.raw_support);
    std::cout<<' '<<out.candidate_count;number(out.candidate_count?std::optional{out.candidates[0].bpm}:std::nullopt);
    number(out.period_s);std::cout<<' '<<unsigned(out.rhythm_grid.status);number(out.grid_phase_s);number(out.next_beat_s);
    std::cout<<' '<<unsigned(out.tempo_dynamics.status);number(out.bpm_per_s);
}
void write_diagnostics(const r::Output& out) {
    std::cout<<' '<<out.heavy_executed<<' '<<out.cost_units<<' '<<out.possible_cost_units<<' '<<out.history_frames<<' '<<out.positive_changes<<' '<<out.event_count;
    number(out.event_count?std::optional{double(out.events[out.event_count-1].sequence)}:std::nullopt);number(out.periodicity);
    number(out.tempo.t_obs);number(out.tempo.t_ready);number(out.rhythm_grid.t_obs);
    number(out.change.present?std::optional{out.change.from_bpm}:std::nullopt);number(out.change.present?std::optional{out.change.to_bpm}:std::nullopt);
    number(out.change.present?std::optional{out.change.confirm_s}:std::nullopt);
    std::cout<<' '<<out.evidence_group_overflow;number(out.pending_primary_since);number(out.pending_subdivision_since);
    std::cout<<' '<<out.change.acoustic_only;
}
void write_candidate(const r::Output& out) {
    const auto c=out.candidate_count?std::optional{out.candidates[0]}:std::nullopt;
    number(c?std::optional{c->phase_support}:std::nullopt);number(c?std::optional{c->ioi}:std::nullopt);number(c?std::optional{c->acf}:std::nullopt);
    std::cout<<'\n';
}
void read_accents(std::vector<r::Accent>& accents,int count) {
    accents.clear();
    for(int i=0;i<count;++i){r::Accent a;unsigned kind=0;std::cin>>a.t_obs>>a.strength>>kind>>a.provenance>>a.group;a.kind=r::Kind(kind);accents.push_back(a);}
}
bool read_frame(r::Frame& frame,std::vector<std::uint8_t>& features,std::vector<r::Accent>& accents) {
    int count=0;
    if(!(std::cin>>frame.epoch>>frame.sequence>>frame.sample_index>>frame.sample_rate_hz>>frame.t_obs>>frame.t_ready>>frame.t_pub>>frame.profile>>frame.saturated))return false;
    for(auto& value:features){unsigned input=0;std::cin>>input;value=std::uint8_t(input);}
    std::cin>>count;read_accents(accents,count);
    frame.features=features;frame.accents=count<0?std::nullopt:std::optional{std::span<const r::Accent>{accents}};
    return bool(std::cin);
}
void process_stream(r::Analyzer& analyzer,unsigned width) {
    std::vector<std::uint8_t> features(width);std::vector<r::Accent> accents;r::Frame frame;
    std::cout<<std::setprecision(17);
    while(read_frame(frame,features,accents)){
        if(!analyzer.process(frame))std::exit(3);
        const auto& out=analyzer.output();write_output(out);write_diagnostics(out);write_candidate(out);
    }
}
int run() {
    unsigned mode=0,fps=0,width=0;std::cin>>mode>>fps>>width;
    r::Analyzer analyzer;r::Config config;config.feature_rate_fps=fps;
    if(!analyzer.prepare(config,r::Mode(mode)))return 2;
    process_stream(analyzer,width);
    return 0;
}
}
int main(){return run();}
