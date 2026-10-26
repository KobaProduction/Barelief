#include <core_app/scheme_editor/audio_node_model.h>
#include <core_app/scheme_editor/numeric_editor_model.h>
#include <core_app/scheme_editor/node_parameter_model.h>
#include <core_app/scheme_editor/node_parameter_form.h>
#include <barelief/ui_text.h>
#include <core_app/numeric_default_graph.h>
#include <core_app/example_presentation.h>
#include <core_app/plot_composition.h>
#include <dataflow_runtime/runtime.h>
#include <QApplication>
#include <QUndoStack>
#include <QCheckBox>
#include <QComboBox>
#include <QDir>
#include <QFontDatabase>
#include <QLabel>
#include <QSet>
#include <QVBoxLayout>
#include <cstdio>
#include <cstdlib>
#include <algorithm>
#include <string_view>

namespace {
void broadband_estimator_labels(){
    using enum signal_contracts::Quantity;
    const signal_contracts::Quantity quantities[]{BroadbandEnergy,BroadbandPower,BroadbandRms,BroadbandMaximum,SpectralFlux,
        BroadbandBinPowerSum,BroadbandBinPowerMean,BroadbandBinPowerRms,BroadbandBinPowerMaximum,BroadbandBinPowerFlux};
    QSet<QString> labels;
    for(const auto quantity:quantities){
        signal_contracts::NumericContract value;value.quantity=quantity;
        labels.insert(presentation::audio_plot_presentation(value).value_title);
    }
    if(labels.size()!=10 || std::any_of(labels.begin(),labels.end(),[](const auto& label){return label.contains("Energy");})){
        std::fputs("FAIL Fourier visibility: broadband estimators need distinct, non-energy axis titles\n",stderr);std::exit(1);
    }
}
void check(bool ok,const char* message){if(!ok){std::fprintf(stderr,"FAIL Fourier visibility: %s\n",message);std::exit(1);}}
std::shared_ptr<QtNodes::NodeDelegateModelRegistry> registry(){
    auto result=std::make_shared<QtNodes::NodeDelegateModelRegistry>();
    const auto kinds=numeric_editor::catalog();
    for(const auto& kind:kinds)result->registerModel([kind]{return std::make_unique<AudioNodeModel>(kind);},numeric_editor::category(kind));
    return result;
}
void ports(const std::shared_ptr<QtNodes::NodeDelegateModelRegistry>& registry,const QString& kind){
    NumericEditorGraphModel graph(registry);QUndoStack undo;graph.set_undo_stack(&undo);
    const auto id=graph.addNode(kind);auto* fft=graph.delegateModel<AudioNodeModel>(id);
    check(fft && fft->numeric_node()->outputs.size()==4,"DSP contract retains all four stable outputs");
    check(fft->nPorts(QtNodes::PortType::Out)==2,"unused DC ports hidden by default");
    check(fft->port_id(QtNodes::PortType::Out,0)==0 && fft->port_id(QtNodes::PortType::Out,1)==1,"Re/Im IDs unchanged");
    check(fft->port_index(QtNodes::PortType::Out,2)==QtNodes::InvalidPortIndex,"hidden DC cannot be selected as visible port");
    check(fft->edit_dc_outputs_shown(true) && fft->nPorts(QtNodes::PortType::Out)==4,"explicit show exposes actual ports");
    undo.undo();check(fft->nPorts(QtNodes::PortType::Out)==2,"undo restores hidden ports");
    undo.redo();check(fft->nPorts(QtNodes::PortType::Out)==4,"redo restores visible ports");
    const auto sink=graph.addNode("ScalarView");auto scalar=*graph.delegateModel<AudioNodeModel>(sink)->numeric_node();
    scalar.inputs[0].contract=fft->numeric_node()->outputs[3].contract;
    graph.delegateModel<AudioNodeModel>(sink)->set_numeric_node(scalar);
    const QtNodes::ConnectionId edge{id,3,sink,0};check(graph.connectionPossible(edge),"DC Im is connectable scalar");graph.addConnection(edge);
    check(graph.connectionExists(edge),"DC Im connection created");
    check(fft->edit_dc_outputs_shown(false) && fft->nPorts(QtNodes::PortType::Out)==4,"connected DC remains visible when explicit show disabled");
    check(graph.numeric_graph().edges.front().output_port==3,"stable DC Im ID preserved");
    const auto saved=graph.save();NumericEditorGraphModel restored(registry);restored.load(saved);
    auto* reloaded=restored.delegateModel<AudioNodeModel>(id);
    check(reloaded && !reloaded->dc_outputs_shown() && reloaded->nPorts(QtNodes::PortType::Out)==4,"connected hidden preference restores safely");
    check(restored.connectionExists(edge) && restored.numeric_graph().edges.front().output_port==3,"reload retains DC connection");
    NumericEditorGraphModel imported(registry);check(imported.set_numeric_graph(graph.numeric_graph()),"physical graph import reveals connected DC before validation");
    check(imported.connectionExists(edge),"physical import retains DC Im endpoint");
    check(restored.deleteConnection(edge) && reloaded->nPorts(QtNodes::PortType::Out)==2,"last DC disconnect hides unused outputs");
    check(reloaded->numeric_node()->outputs.size()==4,"hiding never removes DSP outputs");
    restored.addConnection({id,3,QtNodes::NodeId{999999},0});
    check(reloaded->nPorts(QtNodes::PortType::Out)==2,"invalid DC reconnect leaves unrelated ports hidden");
    restored.addConnection(edge);
    check(restored.connectionExists(edge) && reloaded->nPorts(QtNodes::PortType::Out)==4 && !reloaded->dc_outputs_shown(),"reconnect restores hidden-preference DC endpoint for Undo");
    check(restored.deleteConnection(edge) && reloaded->nPorts(QtNodes::PortType::Out)==2,"reconnected DC hides after final disconnect");
    check(reloaded->edit_dc_outputs_shown(true),"manual optional ports reenabled");
    const auto savedShown=restored.save();NumericEditorGraphModel shown(registry);shown.load(savedShown);
    check(shown.delegateModel<AudioNodeModel>(id)->dc_outputs_shown() && shown.delegateModel<AudioNodeModel>(id)->nPorts(QtNodes::PortType::Out)==4,"explicit show persists without connection");
    check(!AudioNodeModel::valid_presentation({{"show_dc_outputs",QStringLiteral("true")}}),"malformed visibility flag rejected");
}
void musical_layout_caption(){
    auto node=numeric_editor::default_node("MusicalSpectrogram");NodeParameterModel parameters;
    for(const auto step:{signal_contracts::Ratio{48000,4096},signal_contracts::Ratio{32000,2048}}){
        auto& fields=std::get<domain::TransformProcessingParameters>(node.parameters).values;
        auto bin=std::find_if(fields.begin(),fields.end(),[](const auto& field){return field.key=="bin_hz";});
        check(bin!=fields.end(),"musical fixture has explicit frequency-step setting");
        bin->value=signal_contracts::NumericLiteral{double(step.numerator)/step.denominator};
        if(std::none_of(fields.begin(),fields.end(),[](const auto& field){return field.key=="first_bin";}))fields.push_back({"first_bin",signal_contracts::NumericLiteral{std::uint64_t{1}}});
        auto input=std::get<signal_contracts::NumericContract>(node.inputs[0].contract.view());
        input.coordinates[0].origin=step;input.coordinates[0].step=step;
        domain::OwnedProcessingContract owned;check(domain::own_processing_contract(input,numeric_editor::limits().frame,65536,owned)==signal_contracts::Status::ok,"own musical input");node.inputs[0].contract=owned;
        std::array<domain::OwnedProcessingContract,4> outputs;std::array<std::size_t,4> counts{};
        check(domain::musical_spectrogram_output_contracts(node,input,outputs,&counts)==signal_contracts::Status::ok,"canonical musical layout counts");
        parameters.setNode(node,true);const auto* field=parameters.field("rowLayoutReadout");
        check(field && field->primary && field->editor==NodeParameterModel::Editor::Caption,"row layout primary caption exists");
        const auto expected=ui::text("[Low %1] [Mid %2] [High %3] [Notes %4]").arg(qulonglong(counts[0])).arg(qulonglong(counts[1])).arg(qulonglong(counts[2])).arg(qulonglong(counts[3]));
        check(std::get<QString>(field->read())==expected,"caption follows canonical section counts after frequency-axis change");
    }
}
void spectrum_normalization_controls(){
    using namespace domain;using namespace signal_contracts;
    auto node=numeric_editor::default_node("AmplitudePhase");
    check(std::none_of(node.inputs.begin(),node.inputs.end(),[](const auto& p){return p.id==window_size_input;}),"amplitude/phase has no independent N input");
    check(numeric_parameter_key("AmplitudePhase",window_size_input).empty(),"N is not a bindable amplitude/phase parameter");
    NodeParameterModel parameters;parameters.setNode(node,true);
    check(!parameters.field("fft_size"),"no editable or hidden N control");
    const auto* normalize=parameters.field("normalize");
    check(normalize && normalize->primary && std::get<bool>(normalize->read()),"normalization checkbox is visible and defaults on");
    const auto* half=parameters.field("positive_half");check(half && half->primary,"input spectrum layout is explicit");
    auto array=std::get<NumericContract>(node.inputs[0].contract.view());array.shape.extents[0]={128,128};
    std::array<OwnedProcessingContract,2> outputs;
    // A stale serialized size is not an input and cannot override array geometry.
    node.parameters=TransformProcessingParameters{{{"fft_size",NumericLiteral{std::uint64_t{4096}}},{"representation",std::string{"psd"}},{"normalize",false}}};
    check(amplitude_phase_output_contracts(node,array,outputs)==Status::ok,"derive projection from Re/Im geometry");
    auto value=std::get<NumericContract>(outputs[0].view());
    check(value.quantity==Quantity::SpectrumPower && value.unit==NumericUnit::SquaredAmplitude,"unnormalized PSD is labelled raw squared magnitude");
    check(value.coordinates[0].step==Ratio{48000,256},"frequency axis derives FFT length from half-spectrum array");
    std::get<TransformProcessingParameters>(node.parameters).values.push_back({"positive_half",false});
    check(amplitude_phase_output_contracts(node,array,outputs)==Status::ok,"full-spectrum layout derives size without producer metadata");
    value=std::get<NumericContract>(outputs[0].view());check(value.coordinates[0].step==Ratio{48000,129},"full spectrum excludes separate DC");
}
void normalization_widget(){
    for(const auto* language:{"ru","en"})for(const auto* theme:{"light","dark"}){
        ui::Language::instance().set(language);ui::Theme::instance().set(theme);
        NumericEditorGraphModel graph(registry());const auto id=graph.addNode("AmplitudePhase");
        auto* model=graph.delegateModel<AudioNodeModel>(id);
        QWidget panel;QVBoxLayout layout(&panel);
        layout.addWidget(new QLabel(model->caption(),&panel));
        auto* form=new NodeParameterForm(model->parameter_model(),true,&panel);layout.addWidget(form);
        auto* output=new QLabel(&panel);layout.addWidget(output);
        auto* toggle=form->findChild<QCheckBox*>("normalize");
        auto* representation=form->findChild<QComboBox*>("representation");
        check(toggle && toggle->isChecked() && representation,"actual inline normalization and representation controls exist");
        representation->setCurrentIndex(0);QCoreApplication::processEvents();
        check(model->portCaption(QtNodes::PortType::Out,0).contains(ui::text("Compensated FFT magnitude")),"calibrated magnitude must not be captioned peak amplitude");
        representation=form->findChild<QComboBox*>("representation");
        representation->setCurrentIndex(3);QCoreApplication::processEvents();
        check(model->portCaption(QtNodes::PortType::Out,0).contains(ui::text("Power spectral density (PSD)")),"normalized PSD output caption");
        // The real checkbox must update the graph and the resulting unit/caption.
        toggle=form->findChild<QCheckBox*>("normalize");toggle->setChecked(false);QCoreApplication::processEvents();
        output->setText(model->portCaption(QtNodes::PortType::Out,0));
        check(output->text().contains(ui::text("Raw magnitude squared")),"switching off PSD removes density label");
        const auto saved=graph.save();NumericEditorGraphModel restored(registry());restored.load(saved);
        check(!std::get<bool>(restored.delegateModel<AudioNodeModel>(id)->parameter_model()->field("normalize")->read()),"normalization state survives graph save/load");
        panel.adjustSize();panel.show();QCoreApplication::processEvents();
        check(form->findChild<QCheckBox*>("normalize")->isVisible(),"normalization is visible on block");
        const auto evidence=qEnvironmentVariable("FFT_NORMALIZATION_EVIDENCE");
        if(!evidence.isEmpty()){
            check(QDir().mkpath(evidence),"create own widget evidence directory");
            check(panel.grab().save(QDir(evidence).filePath(QString("controls-%1-%2.png").arg(language,theme))),"save own offscreen widget render");
        }
    }
}
void shipped_window_dependency_branches(){
    using namespace domain;using namespace signal_contracts;using namespace dataflow_runtime;
    for(const auto& example:numeric_example_graphs()){
        for(const auto& node:example.graph.nodes)if(node.kind=="WindowFunction"){
            check(!node.inputs.empty(),"window generator has numeric size input");
            const auto input=std::get<NumericContract>(node.inputs[0].contract.view());
            check(input.dtype==DType::UInt32 && input.shape.rank==Rank::Scalar,"all shipped window generators consume N scalar");
            const auto edge=std::find_if(example.graph.edges.begin(),example.graph.edges.end(),[&](const auto& e){return e.to==node.id && e.input_port==0;});
            check(edge!=example.graph.edges.end(),"all shipped window generators retain size dependency");
            const auto parent=std::find_if(example.graph.nodes.begin(),example.graph.nodes.end(),[&](const auto& n){return n.id==edge->from;});
            check(parent!=example.graph.nodes.end() && parent->kind=="ArrayLength","all shipped window sizes measured from corresponding buffer");
        }
        if(std::string_view(example.id)!="spectral-mix")continue;
        unsigned products=0;
        for(const auto& node:example.graph.nodes)if(node.kind=="ArrayMultiply"){
            ++products;
            const auto coefficients=std::find_if(example.graph.edges.begin(),example.graph.edges.end(),[&](const auto& e){return e.to==node.id && e.input_port==1;});
            check(coefficients!=example.graph.edges.end(),"cloned window product retains coefficient input");
            const auto generator=std::find_if(example.graph.nodes.begin(),example.graph.nodes.end(),[&](const auto& n){return n.id==coefficients->from;});
            check(generator!=example.graph.nodes.end() && generator->kind=="WindowFunction","cloned product consumes coefficient generator");
        }
        check(products==2,"spectrum mixer retains two independent window multiplications");
        NumericRuntimeProfile profile{{128,256,16,32,1048576,16*1048576,{8,131072,524288,16384,256,8192}},{{16*1048576,32*1048576,16*1048576,1048576},0,192*1048576},{32768,8,262144,262144,16*1048576},{true,true,true}};profile.music_enabled=true;
        std::shared_ptr<const PreparedGraph> plan;std::unique_ptr<RuntimeContext> state;
        check(PreparedGraph::prepare_numeric(example.graph,profile,plan,state)==Status::ok,"spectral-mix complete cloned branch passes runtime admission");
        NumericEditorGraphModel editor(registry());check(editor.set_numeric_graph(example.graph),"spectral-mix complete branch imports in actual GUI model");
    }
}
void rhythm_node_ports(){
    const auto models=registry();
    for(const auto* language:{"ru","en"}){
        ui::Language::instance().set(language);
        NumericEditorGraphModel editor(models);
        const auto id=editor.addNode("RhythmAnalyzer");
        auto* model=editor.delegateModel<AudioNodeModel>(id);
        check(model && model->nPorts(QtNodes::PortType::Out)==domain::rhythm_output_count,"all rhythm outputs visible");
        check(model->portCaption(QtNodes::PortType::Out,0).contains(ui::text("Candidate tempo, BPM")),"localized candidate caption");
        check(model->portCaption(QtNodes::PortType::Out,1).contains(ui::text("Accepted tempo, BPM")),"accepted tempo separate from candidate");
        check(model->parameter_model()->field("feature_rate_fps") && model->parameter_model()->field("mode"),"rhythm settings registered");
        NumericEditorGraphModel example(models);
        check(example.set_numeric_graph(numeric_rhythm_graph()),"rhythm example imports in actual GUI model");
        const auto saved=example.save();NumericEditorGraphModel restored(models);restored.load(saved);
        check(restored.numeric_graph().nodes.size()==example.numeric_graph().nodes.size(),"rhythm example survives save/load");
    }
}
void rhythm_example_labels(){
    const auto examples=numeric_example_graphs();
    const auto found=std::find_if(examples.begin(),examples.end(),[](const auto& value){return std::string_view(value.id)=="rhythm-research";});
    check(found!=examples.end(),"rhythm example registered");
    for(const auto* language:{"ru","en"}){
        ui::Language::instance().set(language);
        NumericEditorGraphModel editor(registry());check(editor.set_numeric_graph(found->graph),"loopback rhythm example imports");
        apply_example_labels(&editor,*found);
        const auto* metrics=editor.delegateModel<AudioNodeModel>(1103);
        check(metrics && metrics->numeric_node()->inputs.size()==domain::rhythm_output_count,"one metric view consumes all rhythm outputs");
        const auto labels=metrics->presentation().value("scalar_inputs").toObject();
        for(unsigned port=0;port<domain::rhythm_output_count;++port)
            check(labels.value(QString::number(port)).toObject().value("display_name").toString()==ui::text(found->scalar_names[0].labels[port]),"localized metric names assigned");
        const auto* selector=editor.delegateModel<AudioNodeModel>(200);
        check(selector && selector->selected_input()==1 && selector->numeric_node()->inputs.size()==4,"Loopback selected by default and microphone absent");
        const auto saved=editor.save();NumericEditorGraphModel restored(registry());restored.load(saved);
        check(restored.delegateModel<AudioNodeModel>(1103)->presentation().value("scalar_inputs").toObject()==labels,"metric labels survive save/load");
    }
}
}
int main(int argc,char** argv){
    std::fputs("Fourier visibility: QApplication\n",stderr);QApplication app(argc,argv);
#ifdef _WIN32
    QFontDatabase::addApplicationFont(QDir(qEnvironmentVariable("WINDIR")).filePath("Fonts/segoeui.ttf"));app.setFont(QFont("Segoe UI",9));
#endif
    auto r=registry();std::fputs("Fourier visibility: complex ports\n",stderr);ports(r,"Fourier");
    std::fputs("Fourier visibility: real ports\n",stderr);ports(r,"RealFourier");
    musical_layout_caption();broadband_estimator_labels();std::fputs("Fourier visibility: normalization controls\n",stderr);spectrum_normalization_controls();normalization_widget();
    std::fputs("Fourier visibility: shipped graphs\n",stderr);shipped_window_dependency_branches();rhythm_node_ports();rhythm_example_labels();
    std::puts("PASS Fourier DC visibility, normalization controls, musical layout and shipped window dependencies");
}
