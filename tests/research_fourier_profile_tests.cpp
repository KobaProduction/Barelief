// SPDX-License-Identifier: AGPL-3.0-or-later
#include "fourier_profile.h"
#include <kiss_backend.h>
#include <ooura_backend.h>
#include <cstdio>
#include <cstdlib>
#include <stdexcept>

namespace {
using namespace domain;
void check(bool value,const char* message) {
    if(!value){std::fprintf(stderr,"Research Fourier profile: %s\n",message);std::exit(1);}
}
ConfiguredProcessingNode node(std::string kind,std::string id) {
    ConfiguredProcessingNode value;value.kind=std::move(kind);
    value.parameters=TransformProcessingParameters{{{"backend_id",std::move(id)}}};return value;
}
void empty_and_named() {
    NumericProcessingGraph graph;
    check(!research_fourier_backend(graph),"empty graph has no provider");
    graph.nodes={node("Fourier",""),node("RealFourier","")};
    check(!research_fourier_backend(graph),"explicit empty IDs preserve builtin without provider");
    graph.nodes[0].parameters=TransformProcessingParameters{};
    graph.nodes[1].parameters=TransformProcessingParameters{};
    check(!research_fourier_backend(graph),"registered default IDs preserve builtin without provider");
    graph.nodes.push_back(node("RealFourier","aubio_ooura_fft8g_float32"));
    auto selected=research_fourier_backend(graph);
    check(selected.get()==&desktop_fourier::ooura_backend_factory(),"explicit Ooura selected");
    graph.nodes.push_back(node("RealFourier","aubio_ooura_fft8g_float32"));
    check(research_fourier_backend(graph)==selected,"repeated same ID admitted");
    graph.nodes={node("Fourier","kiss_fft130_float32")};
    check(research_fourier_backend(graph).get()==&desktop_fourier::kiss_backend_factory(),"explicit KISS selected");
}
void rejected(const NumericProcessingGraph& graph) {
    NumericProcessingGraph previous;previous.nodes={node("Fourier","kiss_fft130_float32")};
    auto retained=research_fourier_backend(previous);
    bool failed=false;
    try{retained=research_fourier_backend(graph);}catch(const std::invalid_argument&){failed=true;}
    check(failed,"invalid selection rejects before returning a profile");
    check(retained.get()==&desktop_fourier::kiss_backend_factory(),"rejection retains previous selection");
}
void invalid_requests() {
    NumericProcessingGraph graph;graph.nodes={node("RealFourier","missing-provider")};rejected(graph);
    graph.nodes={node("Fourier","kiss_fft130_float32"),node("RealFourier","aubio_ooura_fft8g_float32")};rejected(graph);
    graph.nodes={node("RealFourier","")};
    graph.nodes[0].parameters=TransformProcessingParameters{{{"backend_id",true}}};rejected(graph);
    graph.nodes={node("Number","missing-provider")};
    check(!research_fourier_backend(graph),"non-Fourier parameters do not select a provider");
}
void retained_factory() {
    NumericProcessingGraph graph;graph.nodes={node("RealFourier","aubio_ooura_fft8g_float32")};
    auto selected=research_fourier_backend(graph);graph={};
    dsp_kernels::FourierBackendResources resources;
    const dsp_kernels::FourierBackendRequest request{1024,dsp_kernels::FourierBackendTransform::RealForward};
    check(selected->query(request,resources) && resources.owned_bytes && resources.cold_peak_bytes>=resources.owned_bytes,
        "selected Ooura retains original query accounting after graph destruction");
    auto plan=selected->prepare(request);selected.reset();
    check(plan && plan->clone(),"prepared owner and clone outlive selection");
}
}
int main(){empty_and_named();invalid_requests();retained_factory();std::puts("research Fourier profile PASS");}
