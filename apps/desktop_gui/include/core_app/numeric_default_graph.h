#pragma once
#include <domain/processing_contracts.h>
domain::NumericProcessingGraph numeric_default_graph();
// Silent single-source graph for portable fixtures and Generator examples.
domain::NumericProcessingGraph numeric_test_graph();
// Both sources run; input 0 selects the silent generator on first startup.
domain::NumericProcessingGraph numeric_startup_graph();
