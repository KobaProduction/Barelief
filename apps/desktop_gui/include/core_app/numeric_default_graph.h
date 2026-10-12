#pragma once
#include <domain/processing_contracts.h>
domain::NumericProcessingGraph numeric_default_graph();
// Silent single-source changing-tone graph for test fixtures only.
domain::NumericProcessingGraph numeric_test_graph();
// Product generator, bound to an explicit local SampleClock.
domain::NumericProcessingGraph numeric_generator_graph();
// Generator drives analysis; physical source awaits native discovery and explicit wiring.
domain::NumericProcessingGraph numeric_startup_graph();

struct NumericExampleGraph {
    const char* id;
    const char* title_key;
    domain::NumericProcessingGraph graph;
};
std::vector<NumericExampleGraph> numeric_example_graphs();
