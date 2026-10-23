#include <core_app/numeric_default_graph.h>
#include "example_builder.h"

namespace {
using namespace domain;
using signal_contracts::NumericLiteral;
}
// Exactly one analysis history: 48 broadband + 48 MIDI-note bands = 96.
// This node reads the existing Spectrogram #7 after the generic quantizer.
void append_startup_rhythm(examples::Builder& builder) {
    builder.chain(1102,"RhythmAnalyzer",7,examples::transform({
        {"feature_bins",NumericLiteral{std::uint64_t{96}}},
        {"feature_rate_fps",NumericLiteral{std::uint64_t{60}}},
        {"sample_rate",NumericLiteral{std::uint64_t{32768}}}}));
    builder.chain(1103,"ScalarMetricsView",1102);
    for(PortId port=1;port<rhythm_output_count;++port)
        builder.connect(1102,1103,port,port);
    // Candidate BPM is available while the accepted tempo is still undecided.
    builder.chain(1105,"NumberAccumulator",1102,HistoryProcessingParameters{512});
    builder.chain(1106,"VectorLineView",1105);
    // Linear gain-coefficient curve: reuse ordinary numeric line visualization.
    builder.chain(1110,"AudioLineView",1005,examples::transform({
        {"first_bin",NumericLiteral{std::uint64_t{1}}}}));
    builder.connect(1003,1110,0,frequency_step_input);
}
domain::NumericProcessingGraph numeric_rhythm_graph() {
    return numeric_startup_graph();
}
