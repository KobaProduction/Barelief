#include <core_app/scheme_editor/audio_node_model.h>
#include <core_app/scheme_editor/node_parameter_model.h>
#include <core_app/scheme_editor/numeric_editor_model.h>
#include <core_app/scheme_editor/numeric_node_operations.h>
#include <QApplication>
#include <QUndoStack>
#include <array>
#include <cstdio>
#include <cstdlib>

namespace {
using namespace signal_contracts;
void check(bool value,const char* why) {
    if(!value){std::fprintf(stderr,"FAIL matrix phase editor: %s\n",why);std::exit(1);}
}
NumericContract output(NumericEditorGraphModel& graph,QtNodes::NodeId id,unsigned port=0) {
    return std::get<NumericContract>(graph.delegateModel<AudioNodeModel>(id)->numeric_node()->outputs[port].contract.view());
}
void phase_connections(const std::shared_ptr<QtNodes::NodeDelegateModelRegistry>& registry) {
    NumericEditorGraphModel graph(registry);QUndoStack undo;graph.set_undo_stack(&undo);
    const auto fft=graph.addNode("MatrixFFT"),polar=graph.addNode("ComplexPolar"),row=graph.addNode("MatrixRow"),view=graph.addNode("VectorLineView"),size=graph.addNode("UInt32Constant");
    auto* fft_model=graph.delegateModel<AudioNodeModel>(fft);
    auto* polar_model=graph.delegateModel<AudioNodeModel>(polar);
    auto* size_parameters=graph.delegateModel<AudioNodeModel>(size)->parameter_model();
    check(size_parameters->editLiteral("value","512",1),"explicit N source starts at 512");
    check(polar_model->parameter_model()->editLiteral("first_bin","0",0),"polar projection includes MatrixFFT DC");
    for(const auto edge:std::array{QtNodes::ConnectionId{size,0,fft,fft_model->port_index(QtNodes::PortType::In,domain::window_size_input)},
        QtNodes::ConnectionId{size,0,polar,polar_model->port_index(QtNodes::PortType::In,domain::window_size_input)},
        QtNodes::ConnectionId{fft,0,polar,0},QtNodes::ConnectionId{fft,1,polar,1}}){
        check(graph.connectionPossible(edge),"explicit N and raw Re/Im connections are admissible");graph.addConnection(edge);
        check(graph.connectionExists(edge),"explicit N and raw Re/Im routes are retained");
    }
    const QtNodes::ConnectionId phase_edge{polar,1,row,0},view_edge{row,0,view,0};
    check(graph.connectionPossible(phase_edge),"explicit phase is connectable through the ordinary matrix-row operation");
    graph.addConnection(phase_edge);graph.addConnection(view_edge);
    check(graph.connectionExists(phase_edge) && graph.connectionExists(view_edge),"phase routes are retained");
    check(output(graph,fft).quantity==Quantity::SpectrumReal && output(graph,fft,1).quantity==Quantity::SpectrumImaginary,"MatrixFFT preserves raw real and imaginary outputs");
    const auto phase=output(graph,polar,1),magnitude=output(graph,polar);
    check(phase.quantity==Quantity::SpectrumPhase && phase.unit==NumericUnit::Radians,"explicit polar phase contract");
    check(phase.shape==magnitude.shape && phase.coordinates==magnitude.coordinates,"both polar ports share bins and coordinates");
    check(output(graph,row).unit==NumericUnit::Radians && output(graph,row).quantity==Quantity::SpectrumPhase,"generic row preserves phase units");
    check(size_parameters->editLiteral("value","4096",1),"explicit shared FFT and polar N edit accepted");
    check(output(graph,polar,1).shape.extents[1].maximum==2049 && output(graph,row).shape.extents[0].maximum==2049,"phase geometry includes DC and Nyquist after N changes");
    undo.undo();check(output(graph,row).shape.extents[0].maximum==257,"undo restores complete phase geometry");
    undo.redo();check(output(graph,row).shape.extents[0].maximum==2049,"redo restores complete phase geometry");
    NumericEditorGraphModel restored(registry);
    check(restored.set_numeric_graph(graph.numeric_graph()),"phase graph round-trip");
    check(restored.connectionExists(phase_edge) && output(restored,row).unit==NumericUnit::Radians,"phase port identity and units survive restoration");
}
}
int main(int argc,char** argv) {
    QApplication app(argc,argv);
    auto registry=std::make_shared<QtNodes::NodeDelegateModelRegistry>();
    for(const auto kinds=numeric_editor::model_catalog();const auto& kind:kinds)
        registry->registerModel([kind]{return std::make_unique<AudioNodeModel>(kind);},numeric_editor::category(kind));
    phase_connections(registry);
    std::puts("matrix Fourier phase routing/geometry/undo/redo/round-trip PASS");
}
