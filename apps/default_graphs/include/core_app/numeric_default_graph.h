#pragma once
#include <domain/processing_contracts.h>
domain::NumericProcessingGraph numeric_default_graph(std::uint32_t sample_rate=48000);
// Silent single-source changing-tone graph for test fixtures only.
domain::NumericProcessingGraph numeric_test_graph();
// Temporal Fourier analysis of each Mel band, driven by silent changing tones.
domain::NumericProcessingGraph numeric_temporal_fft_graph(std::uint32_t sample_rate=48000);
// Product generator, bound to an explicit local SampleClock.
domain::NumericProcessingGraph numeric_generator_graph(std::uint32_t sample_rate=48000);
// Generator drives analysis; physical source awaits native discovery and explicit wiring.
domain::NumericProcessingGraph numeric_startup_graph();
// PC research pipeline, separate from startup and user projects.
domain::NumericProcessingGraph numeric_music_graph();
domain::NumericProcessingGraph numeric_rhythm_graph();

struct NumericExampleSection {
    domain::NodeId view;
    const char* label;
    std::size_t begin,count;
};
struct NumericExampleStreamNames {
    domain::NodeId selector;
    std::vector<const char*> labels;
};
struct NumericExampleScalarNames {
    domain::NodeId view;
    std::vector<const char*> labels;
};
struct NumericExampleGraph {
    const char* id;
    const char* title_key;
    domain::NumericProcessingGraph graph;
    std::vector<domain::NodeId> audio_matrix_views;
    std::vector<NumericExampleSection> sections;
    std::vector<NumericExampleStreamNames> streams;
    std::vector<NumericExampleScalarNames> scalar_names;
};
std::vector<NumericExampleGraph> numeric_example_graphs();
NumericExampleGraph numeric_startup_example();
