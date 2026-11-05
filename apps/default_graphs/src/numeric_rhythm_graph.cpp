#include <core_app/numeric_default_graph.h>
#include "example_builder.h"
#include <algorithm>
#include <array>
#include <stdexcept>
#include <utility>

namespace {
using namespace domain;
using signal_contracts::NumericLiteral;
void add_replay_pcm_fifo(examples::Builder& b) {
    using namespace rhythm_replay_graph;
    MixerProcessingParameters mix;mix.weights={{0,1.0}};
    mix.mode=MixerMode::WeightedSum;mix.consumption=MixerConsumption::ClockedFifo;mix.fifo_milliseconds=100;
    b.add(pcm_clock,"SampleClock",SampleClockProcessingParameters{pcm_rate,"local"});
    b.add(pcm_mixer,"Mixer",mix);
    auto& node=b.node(pcm_mixer);
    node.inputs={{0,PortDirection::Input,b.output(pcm_source)}};
    node.outputs={{0,PortDirection::Output,b.output(pcm_source)}};
    node.clock_node=pcm_clock;b.connect(pcm_source,pcm_mixer);b.configuration(pcm_clock,pcm_mixer);b.derive(pcm_mixer);
}
void append_replay_cadence(examples::Builder& b) {
    add_replay_pcm_fifo(b);
    const auto route=std::find_if(b.graph.edges.begin(),b.graph.edges.end(),[](const auto& edge){
        return edge.from==rhythm_replay_graph::pcm_source && edge.to==2 && !edge.output_port && !edge.input_port;
    });
    if(route==b.graph.edges.end())throw std::runtime_error("Replay calibrated PCM route missing");
    b.graph.edges.erase(route);b.connect(rhythm_replay_graph::pcm_mixer,2);b.derive(2);
}
void append_periodic_spectrum(examples::Builder& b,bool complete_frames=false) {
    b.chain(1313,"PcmResampler",200,examples::transform({{"mode",std::string{"speex"}},
        {"output_rate",NumericLiteral{std::uint64_t{44100}}}}));
    b.connect(200,1313,1,input_sample_rate_input);
    if(complete_frames)
        b.chain(1314,"FrameBuffer",1313,examples::transform({{"size",NumericLiteral{std::uint64_t{2048}}},
            {"hop_samples",NumericLiteral{std::uint64_t{512}}}}));
    else b.chain(1314,"Window",1313,examples::transform({{"size",NumericLiteral{std::uint64_t{2048}}}}));
    b.chain(1315,"ArrayLength",1314);
    b.chain(1310,"WindowFunction",1315,examples::transform({{"size",NumericLiteral{std::uint64_t{2048}}},
        {"function",std::string{"hann"}},{"periodic",true}}));
    b.add(1311,"ArrayMultiply");b.connect(1314,1311);b.connect(1310,1311,0,1);b.derive(1311);
    b.chain(1312,"RFFT",1311,examples::transform({{"fft_size",NumericLiteral{std::uint64_t{2048}}},
        {"sample_rate",NumericLiteral{std::uint64_t{44100}}}}));
    b.connect(1313,1312,sample_rate_output,target_sample_rate_input);
}
void append_analyzer(examples::Builder& builder) {
    builder.chain(1102,"RhythmAnalyzer",7,examples::transform({
        {"feature_bins",NumericLiteral{std::uint64_t{96}}},
        {"feature_rate_fps",NumericLiteral{std::uint64_t{60}}},
        {"sample_rate",NumericLiteral{std::uint64_t{32768}}}}));
    builder.connect(332,1102,sample_rate_output,target_sample_rate_input);
}
void append_rhythm_views(examples::Builder& builder) {
    builder.chain(1103,"ScalarMetricsView",1102);
    for(PortId port=1;port<rhythm_output_count;++port)
        builder.connect(1102,1103,port,port);
    // Candidate BPM is available while the accepted tempo is still undecided.
    builder.chain(1105,"NumberAccumulator",1102,HistoryProcessingParameters{512});
    builder.chain(1106,"VectorLineView",1105);
}
void append_correction_view(examples::Builder& builder) {
    builder.chain(1110,"AudioLineView",1005,examples::transform({
        {"first_bin",NumericLiteral{std::uint64_t{1}}}}));
    builder.connect(1003,1110,0,frequency_step_input);
}
void append_activity_history(examples::Builder& builder,NodeId first,std::uint64_t size) {
    builder.chain(first,"Spectrogram",1102,HistoryProcessingParameters{std::size_t(size)},7);
    builder.chain(first+1,"MatrixTranspose",first);
    builder.chain(first+2,"MatrixCenter",first+1,examples::transform({{"method",NumericLiteral{std::uint64_t{0}}}}));
}
void append_fourier_periodicity(examples::Builder& builder,NodeId first,std::uint64_t size,bool view) {
    builder.chain(first+3,"MatrixWindowFunction",first+2,examples::transform({{"function",std::string{"hann"}}}));
    builder.node(first+3).enabled=true;
    builder.chain(first+4,"MatrixFFT",first+3,examples::transform({{"fft_size",NumericLiteral{size}},
        {"sample_rate",NumericLiteral{std::uint64_t{60}}}}));
    builder.add(first+50,"ComplexPolar",examples::transform({{"fft_size",NumericLiteral{size}},
        {"first_bin",NumericLiteral{std::uint64_t{0}}}}));
    builder.connect(first+4,first+50,0,0);builder.connect(first+4,first+50,1,1);builder.derive(first+50);
    builder.chain(first+5,"MatrixRow",first+50,examples::transform({{"row",NumericLiteral{std::uint64_t{0}}}}));
    if(view)builder.chain(first+6,"VectorLineView",first+5);
}
void append_autocorrelation_periodicity(examples::Builder& builder,NodeId first,bool view) {
    builder.chain(first+7,"MatrixAutocorrelation",first+2,examples::transform({
        {"max_lag",NumericLiteral{std::uint64_t{120}}},{"frame_rate_fps",NumericLiteral{std::uint64_t{60}}}}));
    builder.chain(first+8,"MatrixRow",first+7,examples::transform({{"row",NumericLiteral{std::uint64_t{0}}}}));
    if(view)builder.chain(first+9,"VectorLineView",first+8);
}
void append_log_mel(examples::Builder& b) {
    b.chain(1300,"SpectrumProjection",1312,examples::transform({{"representation",std::string{"power"}},
        {"normalize",false},{"fft_size",NumericLiteral{std::uint64_t{2048}}},
        {"sample_rate",NumericLiteral{std::uint64_t{44100}}}}));
    b.chain(1301,"Mel",1300,examples::transform({{"bands",NumericLiteral{std::uint64_t{128}}},
        {"sample_rate",NumericLiteral{std::uint64_t{44100}}},{"bin_hz",NumericLiteral{44100./2048}},
        {"first_bin",NumericLiteral{std::uint64_t{0}}},{"scale",std::string{"slaney"}},{"norm",std::string{"slaney"}}}));
    b.chain(1302,"Spectrogram",1301,HistoryProcessingParameters{512,true});
    b.chain(1303,"PowerToDb",1302);
}
void append_onset_strength(examples::Builder& b,bool maximum_filter) {
    auto parameters=examples::transform({{"center",false},
        {"n_fft",NumericLiteral{std::uint64_t{2048}}},{"hop_length",NumericLiteral{std::uint64_t{512}}}});
    if(maximum_filter){
        parameters.values.push_back({"lag",NumericLiteral{std::uint64_t{2}}});
        parameters.values.push_back({"max_size",NumericLiteral{std::uint64_t{3}}});
    }
    const NodeId first=maximum_filter?1360:1304;
    b.chain(first,"OnsetStrength",1303,std::move(parameters));
    b.chain(first+1,"VectorLineView",first);
}
void append_spectral_descriptor(examples::Builder& b,std::string_view kind,NodeId first) {
    b.chain(first,kind,1312);
    b.chain(first+1,"NumberAccumulator",first,HistoryProcessingParameters{512,true});
    b.chain(first+2,"VectorLineView",first+1);
}
void append_spectral_descriptors(examples::Builder& b) {
    constexpr std::array kinds{"SpectralFlux","ComplexSpectralDifference","PhaseDeviation","HighFrequencyContent"};
    for(std::size_t i=0;i<kinds.size();++i)append_spectral_descriptor(b,kinds[i],1320+NodeId(10*i));
}
void append_spectral_onsets(examples::Builder& b) {
    // Complete existing reference fixture, used only by offline trace replay.
    append_periodic_spectrum(b,true);append_log_mel(b);append_onset_strength(b,false);
    b.chain(1306,"AudioSurfaceView",1302);
    append_onset_strength(b,true);append_spectral_descriptors(b);
}
void append_tempogram(examples::Builder& builder,NodeId first,std::uint64_t size) {
    append_activity_history(builder,first,size);
    append_fourier_periodicity(builder,first,size,true);
    append_autocorrelation_periodicity(builder,first,true);
}
void append_tempo_evidence(examples::Builder& builder) {
    builder.add(1220,"TempoEvidence",examples::transform({{"frame_rate_fps",NumericLiteral{std::uint64_t{60}}}}));
    builder.connect(1205,1220,0,0);builder.connect(1215,1220,0,1);
    builder.connect(1208,1220,0,2);builder.connect(1218,1220,0,3);
    builder.connect(1200,1220,1,4);builder.connect(1210,1220,1,5);
    builder.derive(1220);
}
examples::Builder source_builder() {
    examples::Builder builder;examples::add_startup_input(builder);
    builder.graph.execution_interval={1,60};return builder;
}
examples::Builder calibrated_builder() {
    auto builder=source_builder();examples::add_calibrated_features(builder);return builder;
}
NumericProcessingGraph periodicity_graph(NodeId first,std::uint64_t size,bool fourier) {
    auto builder=calibrated_builder();append_analyzer(builder);
    append_activity_history(builder,first,size);
    if(fourier)append_fourier_periodicity(builder,first,size,true);
    else append_autocorrelation_periodicity(builder,first,true);
    return builder.finish();
}
void append_evidence_inputs(examples::Builder& builder,NodeId first,std::uint64_t size) {
    append_activity_history(builder,first,size);
    append_fourier_periodicity(builder,first,size,false);
    append_autocorrelation_periodicity(builder,first,false);
}
void append_evidence_views(examples::Builder& builder) {
    builder.chain(1221,"MatrixImageView",1220);
    builder.chain(1222,"ScalarMetricsView",1220,{},1);
    builder.connect(1220,1222,2,1);builder.connect(1220,1222,3,2);
}
NumericProcessingGraph evidence_graph() {
    auto builder=calibrated_builder();append_analyzer(builder);
    append_evidence_inputs(builder,1200,512);append_evidence_inputs(builder,1210,4096);
    append_tempo_evidence(builder);append_evidence_views(builder);return builder.finish();
}
NumericProcessingGraph onset_graph(bool maximum_filter) {
    auto builder=source_builder();append_periodic_spectrum(builder);append_log_mel(builder);
    append_onset_strength(builder,maximum_filter);builder.chain(1306,"AudioSurfaceView",1302);
    return builder.finish();
}
NumericProcessingGraph descriptor_graph(std::string_view kind,NodeId first) {
    auto builder=source_builder();append_periodic_spectrum(builder);
    append_spectral_descriptor(builder,kind,first);return builder.finish();
}
bool node_present(const NumericProcessingGraph& graph,NodeId id) {
    return std::any_of(graph.nodes.begin(),graph.nodes.end(),[id](const auto& node){return node.id==id;});
}
void append_node_names(NumericExampleGraph& result) {
    static constexpr NumericExampleNodeName names[]{{2,"Audio history · 4096 samples"},{4,"Real FFT · symmetric Hann"},
        {1313,"Reference PCM · 44100 Hz"},{1314,"Reference frames · 2048 / hop 512"},
        {1310,"Hann · periodic"},{1311,"Apply periodic window"},{1312,"Real FFT · periodic Hann"},
        {7,"Linear feature history · 512 frames"},{1102,"Experimental rhythm tracker"},
        {1200,"Short activity history · 512 frames"},{1210,"Long activity history · 4096 frames"},
        {1204,"Short Fourier periodicity"},{1214,"Long Fourier periodicity"},
        {1207,"Short autocorrelation"},{1217,"Long autocorrelation"},{1220,"Tempo candidates · experimental"},
        {1300,"Linear spectral power"},{1301,"Mel filter bank · 128 bands"},
        {1302,"Linear Mel history · 512 frames"},{1303,"Log power · onset branch"},
        {1304,"Spectral onset envelope"},{1305,"Onset strength over time"},
        {1360,"Max-filter onset · lag 2 / width 3"},{1361,"Max-filter onset over time"},
        {1320,"Positive spectral flux"},{1321,"Spectral flux history"},{1322,"Spectral flux over time"},
        {1330,"Complex spectral difference"},{1331,"Complex difference history"},{1332,"Complex difference over time"},
        {1340,"Phase deviation"},{1341,"Phase deviation history"},{1342,"Phase deviation over time"},
        {1350,"High frequency content"},{1351,"HFC history"},{1352,"HFC over time"},
        {1221,"Tempo evidence candidates"},{1222,"Tempo evidence metrics"}};
    for(const auto& name:names)if(node_present(result.graph,name.node))result.node_names.push_back(name);
}
void append_scalar_names(NumericExampleGraph& result) {
    if(node_present(result.graph,1103))result.scalar_names.push_back({1103,{"Candidate tempo, BPM","Accepted tempo, BPM",
        "Decision score (not probability)","Tempo status: 0 unknown, 1 ambiguous, 2 determined",
        "Beat phase, 0...1","Next beat, source seconds","Tempo change, BPM/s","Spectral activity",
        "Periodicity support","ACF cost units (not time)","Accent strength",
        "Grid status: 0 unknown, 1 ambiguous, 2 determined"}});
    if(node_present(result.graph,1222))result.scalar_names.push_back({1222,{"Candidate count","Evidence maturity","Harmonic ambiguity"}});
}
NumericExampleGraph rhythm_example(const char* id,const char* title,NumericProcessingGraph graph,
                                  std::initializer_list<NodeId> matrix_views={}) {
    NumericExampleGraph result{id,title,std::move(graph),std::vector<NodeId>(matrix_views),{},{{200,{"Generator","Loopback"}}}};
    examples::describe_rhythm_example(result);return result;
}
void append_periodicity_examples(std::vector<NumericExampleGraph>& result) {
    struct Recipe {const char* id;const char* title;NodeId first;std::uint64_t size;bool fourier;};
    static constexpr Recipe recipes[]{
        {"rhythm-fourier-short","Rhythm: Fourier periodicity, 512 frames",1200,512,true},
        {"rhythm-fourier-long","Rhythm: Fourier periodicity, 4096 frames",1210,4096,true},
        {"rhythm-acf-short","Rhythm: autocorrelation, 512 frames",1200,512,false},
        {"rhythm-acf-long","Rhythm: autocorrelation, 4096 frames",1210,4096,false}};
    for(const auto& recipe:recipes)result.push_back(rhythm_example(recipe.id,recipe.title,
        periodicity_graph(recipe.first,recipe.size,recipe.fourier)));
    result.push_back(rhythm_example("rhythm-tempo-evidence","Rhythm: multiscale tempo evidence",evidence_graph()));
}
void append_onset_examples(std::vector<NumericExampleGraph>& result) {
    result.push_back(rhythm_example("rhythm-onset-strength","Onsets: log-Mel strength",onset_graph(false),{1306}));
    result.push_back(rhythm_example("rhythm-onset-max-filter","Onsets: max-filter log-Mel strength",onset_graph(true),{1306}));
}
void append_descriptor_examples(std::vector<NumericExampleGraph>& result) {
    struct Recipe {const char* id;const char* title;const char* kind;NodeId first;};
    static constexpr Recipe recipes[]{
        {"rhythm-spectral-flux","Onsets: spectral flux","SpectralFlux",1320},
        {"rhythm-complex-difference","Onsets: complex spectral difference","ComplexSpectralDifference",1330},
        {"rhythm-phase-deviation","Onsets: phase deviation","PhaseDeviation",1340},
        {"rhythm-high-frequency-content","Onsets: high frequency content","HighFrequencyContent",1350}};
    for(const auto& recipe:recipes)result.push_back(rhythm_example(recipe.id,recipe.title,descriptor_graph(recipe.kind,recipe.first)));
}
}
// Share the unquantized Float32 history with the existing baseline observer.
void append_startup_rhythm(examples::Builder& builder) {
    append_analyzer(builder);append_rhythm_views(builder);append_correction_view(builder);
}
domain::NumericProcessingGraph numeric_rhythm_graph() {
    auto builder=calibrated_builder();builder.chain(8,"AudioSurfaceView",7);
    append_analyzer(builder);append_rhythm_views(builder);return builder.finish();
}
domain::NumericProcessingGraph numeric_rhythm_replay_graph() {
    examples::Builder builder;builder.graph=numeric_startup_graph();
    append_replay_cadence(builder);
    append_tempogram(builder,1200,512);append_tempogram(builder,1210,4096);
    append_tempo_evidence(builder);append_spectral_onsets(builder);return builder.finish();
}
namespace examples {
void describe_rhythm_example(NumericExampleGraph& result) {
    append_node_names(result);append_scalar_names(result);
}
std::vector<NumericExampleGraph> rhythm_examples() {
    std::vector<NumericExampleGraph> result;
    result.push_back(rhythm_example("rhythm-research","Spectrogram and rhythm: shared 96-band history",numeric_rhythm_graph(),{8}));
    append_periodicity_examples(result);append_onset_examples(result);append_descriptor_examples(result);return result;
}
}
