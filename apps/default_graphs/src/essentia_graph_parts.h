#pragma once
#include "example_builder.h"
namespace essentia_graph {
domain::NodeId mel_input(examples::Builder&,domain::NodeId spectrum);
void band_emphasis(examples::Builder&,domain::NodeId bands,std::size_t frames);
void candidate(examples::Builder&,domain::NodeId source,domain::PortId curve,domain::NodeId output,
    float frame_rate,std::size_t upsampling);
}
