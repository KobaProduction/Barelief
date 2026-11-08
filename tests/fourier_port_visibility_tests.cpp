#include <core_app/scheme_editor/audio_node_model.h>
#include <core_app/scheme_editor/numeric_editor_model.h>
#include <core_app/scheme_editor/node_parameter_model.h>
#include <core_app/scheme_editor/node_parameter_form.h>
#include <barelief/ui_text.h>
#include <core_app/numeric_default_graph.h>
#include <core_app/numeric_btrack_reference_graph.h>
#include <core_app/example_presentation.h>
#include <core_app/plot_composition.h>
#include <dataflow_runtime/runtime.h>
#include <dsp_kernels/dsp.h>
#include <QApplication>
#include <QUndoStack>
#include <QCheckBox>
#include <QComboBox>
#include <QDir>
#include <QFontDatabase>
#include <QLabel>
#include <QVBoxLayout>
#include <cstdio>
#include <cstdlib>
#include <algorithm>
#include <string_view>

namespace {
void check(bool ok,const char* message){if(!ok){std::fprintf(stderr,"FAIL Fourier visibility: %s\n",message);std::exit(1);}}
std::shared_ptr<QtNodes::NodeDelegateModelRegistry> registry(){
    auto result=std::make_shared<QtNodes::NodeDelegateModelRegistry>();
    const auto kinds=numeric_editor::model_catalog();
    for(const auto& kind:kinds)result->registerModel([kind]{return std::make_unique<AudioNodeModel>(kind);},numeric_editor::category(kind));
    return result;
}
void ports(const std::shared_ptr<QtNodes::NodeDelegateModelRegistry>& registry,const QString& kind){
    NumericEditorGraphModel graph(registry);QUndoStack undo;graph.set_undo_stack(&undo);
    const auto id=graph.addNode(kind);auto* fft=graph.delegateModel<AudioNodeModel>(id);
    const unsigned count=kind=="AmplitudePhase"?3:2,dc=count-1;
    check(fft && fft->numeric_node()->outputs.size()==count,"DSP contract retains real/imaginary and supported DC outputs");
    check(fft->nPorts(QtNodes::PortType::Out)==2,"unused DC ports hidden by default");
    check(fft->port_id(QtNodes::PortType::Out,0)==0 && fft->port_id(QtNodes::PortType::Out,1)==1,"Re/Im IDs unchanged");
    check(fft->port_index(QtNodes::PortType::Out,2)==QtNodes::InvalidPortIndex,"hidden DC cannot be selected as visible port");
    if(kind!="AmplitudePhase"){check(!fft->edit_dc_outputs_shown(true),"Fourier includes DC only in its planes");return;}
    check(fft->edit_dc_outputs_shown(true) && fft->nPorts(QtNodes::PortType::Out)==count,"explicit show exposes actual ports");
    undo.undo();check(fft->nPorts(QtNodes::PortType::Out)==2,"undo restores hidden ports");
    undo.redo();check(fft->nPorts(QtNodes::PortType::Out)==count,"redo restores visible ports");
    const auto sink=graph.addNode("ScalarView");auto scalar=*graph.delegateModel<AudioNodeModel>(sink)->numeric_node();
    scalar.inputs[0].contract=fft->numeric_node()->outputs[dc].contract;
    graph.delegateModel<AudioNodeModel>(sink)->set_numeric_node(scalar);
    const QtNodes::ConnectionId edge{id,dc,sink,0};check(graph.connectionPossible(edge),"DC offset is connectable scalar");graph.addConnection(edge);
    check(graph.connectionExists(edge),"DC offset connection created");
    check(fft->edit_dc_outputs_shown(false) && fft->nPorts(QtNodes::PortType::Out)==count,"connected DC remains visible when explicit show disabled");
    check(graph.numeric_graph().edges.front().output_port==dc,"stable DC offset ID preserved");
    const auto saved=graph.save();NumericEditorGraphModel restored(registry);restored.load(saved);
    auto* reloaded=restored.delegateModel<AudioNodeModel>(id);
    check(reloaded && !reloaded->dc_outputs_shown() && reloaded->nPorts(QtNodes::PortType::Out)==count,"connected hidden preference restores safely");
    check(restored.connectionExists(edge) && restored.numeric_graph().edges.front().output_port==dc,"reload retains DC connection");
    NumericEditorGraphModel imported(registry);check(imported.set_numeric_graph(graph.numeric_graph()),"physical graph import reveals connected DC before validation");
    check(imported.connectionExists(edge),"physical import retains DC offset endpoint");
    check(restored.deleteConnection(edge) && reloaded->nPorts(QtNodes::PortType::Out)==2,"last DC disconnect hides unused outputs");
    check(reloaded->numeric_node()->outputs.size()==count,"hiding never removes DSP outputs");
    restored.addConnection({id,dc,QtNodes::NodeId{999999},0});
    check(reloaded->nPorts(QtNodes::PortType::Out)==2,"invalid DC reconnect leaves unrelated ports hidden");
    restored.addConnection(edge);
    check(restored.connectionExists(edge) && reloaded->nPorts(QtNodes::PortType::Out)==count && !reloaded->dc_outputs_shown(),"reconnect restores hidden-preference DC endpoint for Undo");
    check(restored.deleteConnection(edge) && reloaded->nPorts(QtNodes::PortType::Out)==2,"reconnected DC hides after final disconnect");
    check(reloaded->edit_dc_outputs_shown(true),"manual optional ports reenabled");
    const auto savedShown=restored.save();NumericEditorGraphModel shown(registry);shown.load(savedShown);
    check(shown.delegateModel<AudioNodeModel>(id)->dc_outputs_shown() && shown.delegateModel<AudioNodeModel>(id)->nPorts(QtNodes::PortType::Out)==count,"explicit show persists without connection");
    check(!AudioNodeModel::valid_presentation({{"show_dc_outputs",QStringLiteral("true")}}),"malformed visibility flag rejected");
}
void projection_pair(domain::ConfiguredProcessingNode& node,const signal_contracts::NumericContract& real,std::size_t imaginary_count){
    auto imaginary=real;imaginary.quantity=signal_contracts::Quantity::SpectrumImaginary;imaginary.shape.extents[0]={imaginary_count,imaginary_count};
    check(domain::own_processing_contract(real,numeric_editor::limits().frame,65536,node.inputs[0].contract)==signal_contracts::Status::ok,"own explicit real spectrum");
    check(domain::own_processing_contract(imaginary,numeric_editor::limits().frame,65536,node.inputs[1].contract)==signal_contracts::Status::ok,"own explicit imaginary spectrum");
}
void spectrum_normalization_controls(){
    using namespace domain;using namespace signal_contracts;
    auto node=numeric_editor::default_node("AmplitudePhase");
    check(std::any_of(node.inputs.begin(),node.inputs.end(),[](const auto& p){return p.id==window_size_input;}),"amplitude/phase exposes explicit N input");
    check(numeric_parameter_key("AmplitudePhase",window_size_input)=="fft_size","N binds the registered amplitude/phase setting");
    NodeParameterModel parameters;parameters.setNode(node,true);
    check(parameters.field("fft_size"),"explicit N setting remains editable without a wire");
    const auto* normalize=parameters.field("normalize");
    check(normalize && normalize->primary && std::get<bool>(normalize->read()),"normalization checkbox is visible and defaults on");
    const auto* half=parameters.field("positive_half");check(half && half->primary,"input spectrum layout is explicit");
    auto array=std::get<NumericContract>(node.inputs[0].contract.view());array.shape.extents[0]={129,129};
    std::array<OwnedProcessingContract,3> outputs;
    node.parameters=TransformProcessingParameters{{{"fft_size",NumericLiteral{std::uint64_t{256}}},{"representation",std::string{"psd"}},{"normalize",false},{"sample_rate",NumericLiteral{std::uint64_t{48000}}}}};
    projection_pair(node,array,129);
    check(amplitude_phase_output_contracts(node,array,outputs)==Status::ok,"derive projection from explicit N and Re/Im geometry");
    auto value=std::get<NumericContract>(outputs[0].view());
    check(value.quantity==Quantity::SpectrumPower && value.unit==NumericUnit::SquaredAmplitude,"unnormalized PSD is labelled raw squared magnitude");
    check(value.coordinates[0].step==Ratio{48000,256},"frequency axis uses declared half-spectrum N");
    std::get<TransformProcessingParameters>(node.parameters).values[0].value=NumericLiteral{std::uint64_t{512}};
    check(amplitude_phase_output_contracts(node,array,outputs)==Status::incompatible_contract,"bounded source slices belong after the full spectral projection");
    std::get<TransformProcessingParameters>(node.parameters).values[0].value=NumericLiteral{std::uint64_t{129}};
    std::get<TransformProcessingParameters>(node.parameters).values.push_back({"positive_half",false});
    check(amplitude_phase_output_contracts(node,array,outputs)==Status::ok,"explicit full-spectrum N admits the declared non-DC bins");
    value=std::get<NumericContract>(outputs[0].view());check(value.coordinates[0].step==Ratio{48000,129},"full spectrum excludes separate DC according to explicit N");
    std::get<TransformProcessingParameters>(node.parameters).values[0].value=NumericLiteral{std::uint64_t{128}};
    check(amplitude_phase_output_contracts(node,array,outputs)==Status::incompatible_contract,"explicit N rejects a plane beyond its declared spectral bounds");
}
void select_projection(QComboBox& control,dsp_kernels::SpectrumProjection projection) {
    const auto variants=dsp_kernels::amplitude_spectrum_projections();
    const auto selected=std::find(variants.begin(),variants.end(),projection);
    check(selected!=variants.end(),"projection enum is registered");
    const auto index=control.findData(int(selected-variants.begin()));
    check(index>=0,"registered projection has a control value");
    control.setCurrentIndex(index);
}
void check_amplitude_output(const AudioNodeModel& node,signal_contracts::Quantity quantity,signal_contracts::NumericUnit unit) {
    using namespace signal_contracts;
    const auto& port=node.numeric_node()->outputs.front();
    const auto value=std::get<NumericContract>(port.contract.view());
    check(port.id==domain::processing_output && value.dtype==DType::Float32 && value.shape.rank==Rank::Vector &&
          value.quantity==quantity && value.unit==unit,"projection control updates the amplitude output contract");
}
void normalization_widget(){
    using namespace signal_contracts;
    using dsp_kernels::SpectrumProjection;
    for(const auto* language:{"ru","en"})for(const auto* theme:{"light","dark"}){
        ui::Language::instance().set(language);ui::Theme::instance().set(theme);
        NumericEditorGraphModel graph(registry());const auto id=graph.addNode("AmplitudePhase");
        auto* model=graph.delegateModel<AudioNodeModel>(id);
        check(model->parameter_model()->edit("sample_rate",signal_contracts::NumericLiteral{std::uint64_t{48000}}),"PSD uses an explicit sampling rate");
        QWidget panel;QVBoxLayout layout(&panel);
        layout.addWidget(new QLabel(model->caption(),&panel));
        auto* form=new NodeParameterForm(model->parameter_model(),true,&panel);layout.addWidget(form);
        auto* toggle=form->findChild<QCheckBox*>("normalize");
        auto* representation=form->findChild<QComboBox*>("representation");
        check(toggle && toggle->isChecked() && representation,"actual inline normalization and representation controls exist");
        select_projection(*representation,SpectrumProjection::Magnitude);QCoreApplication::processEvents();
        check_amplitude_output(*model,Quantity::SpectrumWindowScaledMagnitude,NumericUnit::RawAmplitude);
        representation=form->findChild<QComboBox*>("representation");
        select_projection(*representation,SpectrumProjection::Psd);QCoreApplication::processEvents();
        check_amplitude_output(*model,Quantity::SpectrumPsd,NumericUnit::SquaredAmplitudePerHertz);
        toggle=form->findChild<QCheckBox*>("normalize");toggle->setChecked(false);QCoreApplication::processEvents();
        check_amplitude_output(*model,Quantity::SpectrumPower,NumericUnit::SquaredAmplitude);
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
const NumericExampleGraph& rhythm_preset(const std::vector<NumericExampleGraph>& examples,std::string_view id) {
    const auto found=std::find_if(examples.begin(),examples.end(),[&](const auto& value){return std::string_view(value.id)==id;});
    check(found!=examples.end(),"explicit rhythm method preset registered");
    return *found;
}
void rhythm_preset_labels(const NumericExampleGraph& example) {
    NumericEditorGraphModel editor(registry());check(editor.set_numeric_graph(example.graph),"explicit rhythm method imports");
    apply_example_labels(&editor,example);
    const auto* selector=editor.delegateModel<AudioNodeModel>(200);
    check(selector&&selector->selected_input()==1&&selector->numeric_node()->inputs.size()==4,"Loopback selected by default and microphone absent");
    const auto saved=editor.save();NumericEditorGraphModel restored(registry());restored.load(saved);
    for(const auto& names:example.scalar_names){
        const auto* metrics=editor.delegateModel<AudioNodeModel>(names.view);
        check(metrics&&metrics->numeric_node()->inputs.size()==names.labels.size(),"one metric view consumes its explicit method outputs");
        const auto labels=metrics->presentation().value("scalar_inputs").toObject();
        const auto* recovered=restored.delegateModel<AudioNodeModel>(names.view);
        check(recovered&&recovered->presentation().value("scalar_inputs").toObject()==labels,"metric labels survive save/load");
    }
}

void generated_node_names(){
    NumericEditorGraphModel graph(registry());const auto id=graph.addNode("Converter");
    auto* model=graph.delegateModel<AudioNodeModel>(id);
    model->set_presentation(QJsonObject{{"display_name_key","Analysis step"}});
    ui::Language::instance().set("ru");
    check(model->caption()==ui::text("Analysis step"),"generated key follows the current language");
    check(model->edit_display_name("Analysis step") && model->caption()=="Analysis step","custom text matching a translation key stays literal");
    check(model->edit_display_name({}) && model->caption()==ui::text("Analysis step"),"clearing a custom name restores the generated translation");
    const auto saved=graph.save();NumericEditorGraphModel restored(registry());restored.load(saved);
    const auto* recovered=restored.delegateModel<AudioNodeModel>(id);
    check(recovered && recovered->caption()==ui::text("Analysis step"),"context key survives graph roundtrip");
    ui::Language::instance().set("en");
    check(recovered->caption()==ui::text("Analysis step") && recovered->name()=="Converter","language changes the caption and preserves stable kind");
}
void generated_metric_names(){
    NumericEditorGraphModel graph(registry());const auto id=graph.addNode("ScalarMetricsView");
    auto* model=graph.delegateModel<AudioNodeModel>(id);
    const QJsonObject metric{{"display_name_key","Analysis step"}};
    model->set_presentation(QJsonObject{{"scalar_inputs",QJsonObject{{"0",metric}}}});
    ui::Language::instance().set("ru");
    check(model->portCaption(QtNodes::PortType::In,0).startsWith(ui::text("Analysis step")),"generated scalar caption follows the current language");
    check(model->edit_scalar_input_name(0,"Analysis step"),"custom scalar name accepted");
    check(model->portCaption(QtNodes::PortType::In,0).startsWith("Analysis step"),"custom scalar caption remains literal");
    check(model->edit_scalar_input_name(0,{}),"custom scalar name reset");
    ui::Language::instance().set("en");
    check(model->portCaption(QtNodes::PortType::In,0).startsWith(ui::text("Analysis step")),"scalar generated key follows language changes");
}
void common_operation_controls(){
    auto extraction=numeric_editor::default_node("IndexExtract");NodeParameterModel fields;fields.setNode(extraction,true);
    check(fields.field("mode") && fields.field("index") && !fields.field("axis"),"scalar extraction exposes element settings");
    extraction.parameters=domain::TransformProcessingParameters{{{"mode",signal_contracts::NumericLiteral{std::uint64_t{1}}}}};
    fields.setNode(extraction,true);
    check(fields.field("axis") && fields.field("indices") && fields.field("first") && !fields.field("index"),"axis extraction exposes selection settings without scalar-only controls");
    check(!domain::node_kind("ArrayReindex") && !numeric_editor::catalog().contains("ArrayReindex"),"duplicate extractor is retired");
    fields.setNode(numeric_editor::default_node("AxisSum"),true);
    const auto* accumulation=fields.field("accumulation");
    check(accumulation && accumulation->primary && accumulation->options.size()==2,"summation order is an explicit translated setting");
}
void shipped_window_dependency_branches(){
    using namespace domain;using namespace signal_contracts;using namespace dataflow_runtime;
    const auto examples=numeric_fixture_examples();
    for(const auto* id:{"rhythm-onset-strength","rhythm-onset-max-filter","rhythm-spectral-flux",
                       "rhythm-complex-difference","rhythm-phase-deviation","rhythm-high-frequency-content"}){
        const auto& example=rhythm_preset(examples,id);
        const auto window=std::find_if(example.graph.nodes.begin(),example.graph.nodes.end(),[](const auto& node){return node.id==1310&&node.kind=="WindowFunction";});
        check(window!=example.graph.nodes.end(),"each independent spectral method retains its periodic-window branch");
    }
    for(const auto& example:examples){
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
        for(const domain::PortId port:{0,1}){
            using namespace signal_contracts;
            const auto& output=model->numeric_node()->outputs[port];
            const auto contract=std::get<NumericContract>(output.contract.view());
            check(output.id==port && contract.dtype==DType::Float32 && contract.shape.rank==Rank::Scalar,
                  "Candidate and accepted tempo use distinct stable scalar ports");
        }
        check(model->parameter_model()->field("feature_rate_fps") && model->parameter_model()->field("mode"),"rhythm settings registered");
        NumericEditorGraphModel example(models);
        check(example.set_numeric_graph(numeric_rhythm_graph()),"rhythm example imports in actual GUI model");
        const auto saved=example.save();NumericEditorGraphModel restored(models);restored.load(saved);
        check(restored.numeric_graph().nodes.size()==example.numeric_graph().nodes.size(),"rhythm example survives save/load");
    }
}
void rhythm_example_labels(){
    const auto examples=numeric_fixture_examples();
    const auto& baseline=rhythm_preset(examples,"rhythm-research");
    check(baseline.scalar_names.size()==1&&baseline.scalar_names[0].view==1103&&
        baseline.scalar_names[0].labels.size()==domain::rhythm_output_count,"baseline metric view retains all rhythm outputs");
    constexpr const char* ids[]{"rhythm-research","rhythm-fourier-short","rhythm-fourier-long","rhythm-acf-short","rhythm-acf-long",
        "rhythm-tempo-evidence","rhythm-onset-strength","rhythm-onset-max-filter","rhythm-spectral-flux",
        "rhythm-complex-difference","rhythm-phase-deviation","rhythm-high-frequency-content"};
    for(const auto* language:{"ru","en"}){
        ui::Language::instance().set(language);
        for(const auto* id:ids)rhythm_preset_labels(rhythm_preset(examples,id));
    }
}
}
int main(int argc,char** argv){
    std::fputs("Fourier visibility: QApplication\n",stderr);QApplication app(argc,argv);
#ifdef _WIN32
    QFontDatabase::addApplicationFont(QDir(qEnvironmentVariable("WINDIR")).filePath("Fonts/segoeui.ttf"));app.setFont(QFont("Segoe UI",9));
#endif
    auto r=registry();std::fputs("Fourier visibility: complex ports\n",stderr);ports(r,"Fourier");
    std::fputs("Fourier visibility: real ports\n",stderr);ports(r,"RealFourier");ports(r,"AmplitudePhase");
    generated_node_names();generated_metric_names();common_operation_controls();std::fputs("Fourier visibility: normalization controls\n",stderr);spectrum_normalization_controls();normalization_widget();
    std::fputs("Fourier visibility: shipped graphs\n",stderr);shipped_window_dependency_branches();rhythm_node_ports();rhythm_example_labels();
    std::puts("PASS Fourier visibility, normalization controls, presentation behavior and shipped window dependencies");
}
