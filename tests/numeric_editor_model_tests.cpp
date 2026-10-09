#include "core_app/scheme_editor/numeric_editor_model.h"

#include "core_app/scheme_editor/audio_node_model.h"
#include "core_app/scheme_editor/node_parameter_model.h"

#include "visualization/ui_text.h"

#include <desktop/numeric_project_document.h>

#include <QApplication>
#include <QQuickWidget>
#include <QQuickItem>
#include <QElapsedTimer>
#include <QThread>
#include <visualization/quick3d/surface_quick_item.h>
#include <QSurfaceFormat>
#include <QTextStream>
#include <visualization/spectrograms/spectrogram_graph.h>
#include <visualization/axis_control_widget.h>
#include <QDoubleSpinBox>

#include <QLineEdit>

#include <QSpinBox>

#include <QPushButton>

#include <QComboBox>

#include <QCheckBox>

#include <QSpinBox>

#include <QJsonArray>
#include <QJsonDocument>

#include <QTemporaryDir>

#include <QFile>
#include <QFileInfo>

#include <QtNodes/DataFlowGraphicsScene>

#include <core_app/scheme_editor/editor_geometry.h>
#include <core_app/scheme_editor/node_preview_card.h>

#include <QtNodes/internal/UndoCommands.hpp>

#include <QtNodes/internal/NodeGraphicsObject.hpp>

#include <cstdio>

#include <limits>

namespace {

int failures=0;

void check(bool ok,const char* text){if(!ok){std::fprintf(stderr,"FAIL %s\n",text);++failures;}}

}

int main(int argc,char** argv){

    QApplication app(argc,argv);
    if(argc==3 && QString::fromLocal8Bit(argv[1])=="--native-path") {
        QFile file(QString::fromLocal8Bit(argv[2]));check(file.open(QIODevice::ReadOnly),"native path data file");
        std::vector<std::vector<float>> history;QTextStream input(&file);
        while(!input.atEnd()){const auto fields=input.readLine().split(',');if(fields.size()<52 || fields[0]!="7")continue;
            history.clear();for(int i=2;i+49<fields.size();i+=50){std::vector<float> row;for(int b=0;b<50;++b)row.push_back(fields[i+b].toFloat());history.push_back(std::move(row));}}
        check(!history.empty(),"native runtime History matrix");float peak=0;size_t peakAge=0,peakBand=0;
        for(size_t r=0;r<history.size();++r)for(size_t b=0;b<50;++b)if(history[r][b]>peak){peak=history[r][b];peakAge=history.size()-1-r;peakBand=b;}
        SpectrogramGraph view;view.resize(900,600);check(view.initialize({240,180},{1200,900}),"native renderer initialization");
        view.set_history_dimensions(400,50);view.update_frame_history(history);view.set_y_range(0,peak);
        view.setAttribute(Qt::WA_ShowWithoutActivating);view.setWindowFlag(Qt::WindowDoesNotAcceptFocus);
        auto* host=view.findChild<QQuickWidget*>("spectrogramQuickView");
        auto* gl=host && host->rootObject()?host->rootObject()->findChild<SurfaceQuickItem*>():nullptr;check(gl,"native Quick renderer container");
        gl->backend()->setProperty("verifyGeometryUpload",true);view.show();gl->update();
        QElapsedTimer timer;timer.start();while(!view.axis_layout_telemetry().value("geometryUploadVerified").toBool() && timer.elapsed()<5000){QCoreApplication::processEvents();QThread::msleep(2);}
        const auto telemetry=view.axis_layout_telemetry();const auto image=host->grabFramebuffer();
        if(!image.isNull())image.save(QFileInfo(file).absolutePath()+"/native-history-frame.png");
        check(!image.isNull() && host->status()==QQuickWidget::Ready && telemetry.value("rendererReady").toBool(),"native Quick FBO rendering");
        check(telemetry.value("geometryUploadVerified").toBool() && telemetry.value("vertexCount").toInt()==49*399*6,"actual GPU VBO exact readback/mesh topology");
        check(telemetry.value("surfacePeakAge").toULongLong()==peakAge && telemetry.value("surfacePeakBand").toULongLong()==peakBand,"GPU mesh retains History band/age peak");
        std::printf("native Quick runtime History rows=%zu bands=50 peak=%.9g age=%zu band=%zu GPU vertices=%d exact VBO PASS\n",history.size(),peak,peakAge,peakBand,telemetry.value("vertexCount").toInt());return 0;
    }


    {
        SpectrogramGraph view;view.update_frame_history({{-.5f,.25f},{0,.75f}});
        check(view.y_range().first<=-.5f && view.y_range().second>=.75f && view.y_range().second<2,"automatic signed actual history extent");
        view.set_y_range(-2,3);
        check(view.y_range()==QPair<float,float>{-2,3} && view.surface_settings()["y_range_mode"]=="manual","manual API changes display range");
        view.update_frame_history({{.001f,.002f}});check(view.y_range()==QPair<float,float>{-2,3},"new frames preserve manual range");
        const auto explicit_settings=view.surface_settings();SpectrogramGraph restored;restored.set_surface_settings(explicit_settings);
        check(restored.y_range()==QPair<float,float>{-2,3},"explicit manual settings restore");
        auto settings=view.surface_settings();settings["y_range_mode"]="auto";view.set_surface_settings(settings);
        check(view.y_range().first==0 && view.y_range().second>=.002f && view.y_range().second<.01f,"Auto uses actual current values with bounded display headroom");
        view.set_y_range(0,2e-8f);
        check(view.y_range()==QPair<float,float>{0,2e-8f},"small manual range stays applied instead of resetting to 0..1");
        AxisControlWidget control("Amplitude (Y)");control.set_state(0,2e-8,true,false);
        double appliedLow=-1,appliedHigh=-1;bool automatic=true;
        QObject::connect(&control,&AxisControlWidget::range_changed,&control,[&](double low,double high,bool mode){appliedLow=low;appliedHigh=high;automatic=mode;});
        control.auto_box()->setChecked(false);control.findChild<QDoubleSpinBox*>("axisMaximum")->setValue(3e-8);
        check(!automatic && appliedLow==0 && std::abs(appliedHigh-3e-8)<1e-15,"real range control emits small manual interval");
        settings["y_range_mode"]="auto";view.set_surface_settings(settings);const auto heldRange=view.y_range();view.update_frame_history({});
        check(view.projected_history_frames()==0 && view.y_range()==heldRange,"empty projection clears geometry and preserves interval amplitude bounds");
    }
    auto registry=std::make_shared<QtNodes::NodeDelegateModelRegistry>();

    for(const auto& kind:numeric_editor::catalog())registry->registerModel([kind]{return std::make_unique<AudioNodeModel>(kind);},numeric_editor::category(kind));

    {
        for(const auto* kind:{"AudioView","WindowView","WindowedView","FFTView","MelView","SpectrogramView","ScalarView"}) {
            NumericEditorGraphModel views(registry);QtNodes::DataFlowGraphicsScene scene(views);
            const QString sourceKind=QString(kind)=="ScalarView"?"ScalarReduce":QString(kind)=="SpectrogramView"?"Spectrogram":"Window";
            const auto source=views.addNode(sourceKind),target=views.addNode(kind);const QtNodes::ConnectionId edge{source,0,target,0};
            auto* node=views.delegateModel<AudioNodeModel>(target);
            check(!node->parameter_model()->field("input0Rank") && !node->parameter_model()->field("input0Extent0") && !node->parameter_model()->field("input0Dtype"),"View dimensions/types belong to upstream contract");
            const auto before=views.save();check(views.connectionPossible(edge),"View accepts its structural rank without semantic gate");scene.undoStack().push(new QtNodes::ConnectCommand(&scene,edge));
            check(signal_contracts::same_contract(views.delegateModel<AudioNodeModel>(source)->numeric_node()->outputs[0].contract.view(),node->numeric_node()->inputs[0].contract.view()),"Compatible view adopts full producer contract");
            check(!node->portCaption(QtNodes::PortType::In,0).contains("4096") && !node->portCaption(QtNodes::PortType::In,0).contains("400"),"View labels expose dtype and rank without dimensions");
            scene.undoStack().undo();check(views.save()==before,"Rank-compatible connect Undo restores entire document");scene.undoStack().redo();
            const auto other=views.addNode(sourceKind=="Window"?"Spectrogram":"Window"),emptyTarget=views.addNode(kind);const auto unchanged=views.save();
            check(!views.connectionPossible({other,0,emptyTarget,0}) && views.save()==unchanged,"Rank mismatch refuses an unoccupied input without adopting contract");
        }
        NumericEditorGraphModel metrics(registry);QUndoStack undo;metrics.set_undo_stack(&undo);
        const auto id=metrics.addNode("ScalarView");auto* metric=metrics.delegateModel<AudioNodeModel>(id);
        check(metric->add_scalar_input() && metric->numeric_node()->inputs.size()==2,"Metrics support independent scalar inputs");
        check(metric->edit_scalar_input_name(1,"Exact count") && metric->set_scalar_input_enabled(1,false),"Metric names and visibility are editable presentation");
        auto numeric=metrics.numeric_graph();auto contract=std::get<signal_contracts::NumericContract>(numeric.nodes[0].inputs[1].contract.view());contract.dtype=signal_contracts::DType::UInt64;
        check(numeric_editor::set_contract(numeric.nodes[0],domain::PortDirection::Input,1,contract) && metrics.edit_node(id,numeric.nodes[0]),"Metric inputs have independent primitive dtype contracts");
        const auto beforeRemove=metrics.save();const auto undoBefore=undo.index();check(metric->remove_scalar_input(1) && metric->numeric_node()->inputs.size()==1 && undo.index()==undoBefore+1,"Metric removal is one topology plus presentation Undo");
        undo.undo();check(metrics.save()==beforeRemove,"Metric removal Undo restores names visibility and contract");
        auto copied=metrics.saveNode(id);copied["id"]=70;metrics.loadNode(copied);check(metrics.delegateModel<AudioNodeModel>(70)->presentation()==metric->presentation(),"Metric clipboard retains input names and visibility");
    }

    {
        NumericEditorGraphModel extraction(registry);QtNodes::DataFlowGraphicsScene scene(extraction);
        const auto source=extraction.addNode("NetworkInput"),extractor=extraction.addNode("IndexExtract"),metric=extraction.addNode("ScalarView");
        auto producer=*extraction.delegateModel<AudioNodeModel>(source)->numeric_node();
        const auto matrixTemplate=numeric_editor::default_node("Spectrogram");
        auto matrix=std::get<signal_contracts::NumericContract>(matrixTemplate.outputs[0].contract.view());
        matrix.dtype=signal_contracts::DType::UInt64;matrix.semantic=signal_contracts::Semantic::Numeric;matrix.representation.normalization=signal_contracts::Normalization::None;
        check(numeric_editor::set_contract(producer,domain::PortDirection::Output,0,matrix) && extraction.edit_node(source,producer),"Extractor matrix UInt64 producer fixture");
        const auto before=extraction.save();const QtNodes::ConnectionId edge{source,0,extractor,0};
        check(extraction.connectionPossible(edge),"Extractor adopts matrix rank and exact primitive source metadata");scene.undoStack().push(new QtNodes::ConnectCommand(&scene,edge));
        const auto* adopted=extraction.delegateModel<AudioNodeModel>(extractor)->numeric_node();
        const auto scalar=std::get<signal_contracts::NumericContract>(adopted->outputs[0].contract.view());
        check(signal_contracts::same_contract(adopted->inputs[0].contract.view(),producer.outputs[0].contract.view()) && scalar.shape.rank==signal_contracts::Rank::Scalar && scalar.dtype==signal_contracts::DType::UInt64 && scalar.channel_order.size()==1,"Extractor derives an exact independent scalar output");
        check(extraction.connectionPossible({extractor,0,metric,0}),"Extracted scalar feeds metrics");
        const auto accepted=extraction.save();auto malformed=*adopted;
        std::get<domain::TransformProcessingParameters>(malformed.parameters).values[0].value=signal_contracts::NumericLiteral{std::numeric_limits<double>::infinity()};
        check(!extraction.edit_node(extractor,malformed) && extraction.save()==accepted,"Malformed extractor channel refuses atomically before numeric conversion");
        scene.undoStack().undo();check(extraction.save()==before,"Extractor adoption Undo restores all endpoint contracts");
    }

    {
        NumericEditorGraphModel rates(registry);QUndoStack history;rates.set_undo_stack(&history);
        const auto genericSource=rates.addNode("NetworkInput"),accumulator=rates.addNode("Window");
        auto generic=*rates.delegateModel<AudioNodeModel>(genericSource)->numeric_node();
        auto samples=std::get<signal_contracts::NumericContract>(generic.outputs[0].contract.view());
        samples.semantic=signal_contracts::Semantic::Numeric;samples.shape.extents[0]={768,768};samples.clock.tick_seconds={1,44100};samples.clock.sample_rate=44100;
        check(numeric_editor::set_contract(generic,domain::PortDirection::Output,0,samples) && rates.edit_node(genericSource,generic),"Retagged sample-vector source fixture");
        rates.addConnection({genericSource,0,accumulator,0});
        const auto* accumulated=rates.delegateModel<AudioNodeModel>(accumulator)->numeric_node();
        check(rates.connectionExists({genericSource,0,accumulator,0}) && std::get<signal_contracts::NumericContract>(accumulated->inputs[0].contract.view()).shape.extents[0].maximum==768 &&
              std::get<signal_contracts::NumericContract>(accumulated->outputs[0].contract.view()).shape.extents[0].maximum==4096,"Passive source tag must not block chunk adaptation or overwrite Window-owned size");
    }
    {
        NumericEditorGraphModel rates(registry);QUndoStack history;rates.set_undo_stack(&history);
        // Connected clipboard round-trip exercises adapter-owned policy, placement and names.
        const auto a=rates.addNode("Converter"),b=rates.addNode("Converter");
        rates.setNodeData(a,QtNodes::NodeRole::Position,QPointF{20,40});rates.setNodeData(b,QtNodes::NodeRole::Position,QPointF{240,160});
        rates.delegateModel<AudioNodeModel>(a)->edit_display_name("Exact source");
        rates.delegateModel<AudioNodeModel>(b)->edit_display_name("Exact target");
        const QtNodes::ConnectionId original{a,0,b,0};rates.addConnection(original);rates.edit_connection_policy(original,3);
        QtNodes::DataFlowGraphicsScene scene(rates);
        scene.nodeGraphicsObject(a)->setSelected(true);scene.nodeGraphicsObject(b)->setSelected(true);
        QtNodes::CopyCommand copy(&scene);copy.redo();const auto before=rates.save();
        scene.undoStack().push(new QtNodes::PasteCommand(&scene,QPointF{900,700}));
        QtNodes::NodeId ca=QtNodes::InvalidNodeId,cb=QtNodes::InvalidNodeId;
        for(auto id:rates.allNodeIds())if(id!=a && id!=b) {
            const auto title=rates.delegateModel<AudioNodeModel>(id)->caption();if(title=="Exact source")ca=id;if(title=="Exact target")cb=id;
        }
        const QtNodes::ConnectionId pasted{ca,0,cb,0};
        check(ca!=QtNodes::InvalidNodeId && cb!=QtNodes::InvalidNodeId && rates.connectionExists(pasted) && rates.connection_policy(pasted)==3,"Connected paste preserves policy and user names on new node IDs");
        check(rates.nodeData(cb,QtNodes::NodeRole::Position).toPointF()-rates.nodeData(ca,QtNodes::NodeRole::Position).toPointF()==QPointF{220,120},"Paste preserves relative node positions");
        const auto after=rates.save();scene.undoStack().undo();check(rates.save()==before,"Connected paste undo restores the entire original document");
        scene.undoStack().redo();check(rates.save()==after,"Connected paste redo restores metadata, connections and placement");
    }
    {
        NumericEditorGraphModel rates(registry);QUndoStack history;rates.set_undo_stack(&history);
        const auto source=rates.addNode("TestSignal"),window=rates.addNode("Window"),windowed=rates.addNode("WindowFunction"),fft=rates.addNode("StereoFFT"),merge=rates.addNode("SpectrumMerge"),mel=rates.addNode("Mel"),acc=rates.addNode("Spectrogram"),view=rates.addNode("SpectrogramView");
        for(auto edge:std::array{QtNodes::ConnectionId{source,0,window,0},QtNodes::ConnectionId{window,0,windowed,0},QtNodes::ConnectionId{windowed,0,fft,0},QtNodes::ConnectionId{fft,0,merge,0},QtNodes::ConnectionId{merge,0,mel,0},QtNodes::ConnectionId{mel,0,acc,0},QtNodes::ConnectionId{acc,0,view,0}})rates.addConnection(edge);
        const auto independentSource=rates.addNode("TestSignal"),independentView=rates.addNode("AudioView");const QtNodes::ConnectionId untouched{independentSource,0,independentView,0};rates.addConnection(untouched);
        int unrelatedDisconnects=0;QObject::connect(&rates,&QtNodes::DataFlowGraphModel::connectionDeleted,&rates,[&](auto edge){if(edge==untouched)++unrelatedDisconnects;});
        int finished=0;QObject::connect(&rates,&NumericEditorGraphModel::atomic_edit_finished,&rates,[&]{++finished;});
        const auto baseline=rates.save();
        const QtNodes::ConnectionId reordered{source,0,window,0};rates.deleteConnection(reordered);rates.addConnection(reordered);
        check(rates.save()==baseline,"Equivalent connectivity must save identically after edge reinsertion");
        auto* sourceModel=rates.delegateModel<AudioNodeModel>(source);
        check(sourceModel->parameter_model()->field("sample_rate") && sourceModel->parameter_model()->editLiteral("sample_rate","44100",1),"Source sample-rate UI edits a connected graph atomically");
        check(finished==1 && unrelatedDisconnects==0 && rates.connectionExists(untouched),"Rate edit is published once and never reconnects unrelated branches");
        for(const auto& node:rates.numeric_graph().nodes)if(node.id!=domain::NodeId(independentSource) && node.id!=domain::NodeId(independentView))for(const auto* ports:{&node.inputs,&node.outputs})for(const auto& port:*ports){
            const auto data=port.contract.view();if(const auto* c=std::get_if<signal_contracts::NumericContract>(&data))check(c->clock.sample_rate==44100 && c->clock.tick_seconds==signal_contracts::Ratio{1,44100},"Clock propagates through FFT/Mel/History/count/view ports");
        }
        const auto spectrum=std::get<signal_contracts::NumericContract>(rates.delegateModel<AudioNodeModel>(fft)->numeric_node()->outputs[0].contract.view());
        check(std::abs(double(spectrum.coordinates[0].step.numerator)/spectrum.coordinates[0].step.denominator-44100./4096)<1e-12,"FFT Hertz grid follows the source sample rate");
        const auto melContract=std::get<signal_contracts::NumericContract>(rates.delegateModel<AudioNodeModel>(mel)->numeric_node()->outputs[0].contract.view());
        const auto historyContract=std::get<signal_contracts::NumericContract>(rates.delegateModel<AudioNodeModel>(acc)->numeric_node()->outputs[0].contract.view());
        check(melContract.coordinates[0].mode==signal_contracts::CoordinateMode::ExplicitFloat64LE && melContract.coordinates[0].unit==signal_contracts::NumericUnit::Hertz,"Mel producer declares exact runtime Hertz centres after sample-rate editing");
        check(historyContract.coordinates[1]==melContract.coordinates[0] && historyContract.coordinates[0].mode==signal_contracts::CoordinateMode::ExplicitFloat64LE && historyContract.coordinates[0].unit==signal_contracts::NumericUnit::Seconds,"History retains source Hertz and individual source-clock seconds after connected edits");
        check(history.count()==1,"Connected rate edit must be one undo command");history.undo();check(rates.save()==baseline,"Rate undo must restore all contracts and edges");history.redo();
        const auto changed=rates.save();check(!sourceModel->parameter_model()->editLiteral("sample_rate","7999",1) && rates.save()==changed,"Unsupported rate must leave the entire graph unchanged");
        const auto audio=rates.addNode("AudioSource");auto* audioParameters=rates.delegateModel<AudioNodeModel>(audio)->parameter_model();
        check(audioParameters->field("sample_rate"),"AudioSource exposes source rate beside its normal processing settings");
        check(audioParameters->editLiteral("sample_rate","1000",1) && rates.delegateModel<AudioNodeModel>(audio)->numeric_node()->outputs[0].contract.view().index()==0 &&
              std::get<signal_contracts::NumericContract>(rates.delegateModel<AudioNodeModel>(audio)->numeric_node()->outputs[0].contract.view()).clock.sample_rate==1000,"AudioSource retains SDK-supported isolated 1000 Hz configuration");
    }
    {
        NumericEditorGraphModel rates(registry);QUndoStack history;rates.set_undo_stack(&history);
        auto malformed=numeric_editor::default_node("PcmResampler");malformed.inputs.clear();
        check(!numeric_editor::set_sample_rate(malformed,44100),"Malformed resampler rate edit must fail without indexing a missing input");
        auto belowResampler=numeric_editor::default_node("PcmResampler");
        const auto initialResampler=belowResampler.outputs[0].contract;
        check(!numeric_editor::set_sample_rate(belowResampler,1000) && signal_contracts::same_contract(initialResampler.view(),belowResampler.outputs[0].contract.view()),"Resampler rejects 1000 Hz without changing its contract");
        auto unsupported=numeric_editor::default_node("PcmResampler");auto lowClock=std::get<signal_contracts::NumericContract>(unsupported.inputs[0].contract.view());lowClock.clock.tick_seconds={1,8000};lowClock.clock.sample_rate=8000;
        check(numeric_editor::set_contract(unsupported,domain::PortDirection::Input,0,lowClock) && !numeric_editor::set_sample_rate(unsupported,192000),"Editor refuses unsupported resampling ratios before device-specific validation");
        const auto source=rates.addNode("TestSignal"),resampler=rates.addNode("PcmResampler"),window=rates.addNode("Window"),sink=rates.addNode("WindowView");
        rates.addConnection({source,0,resampler,0});rates.addConnection({resampler,0,window,0});rates.addConnection({window,0,sink,0});
        auto* sourceModel=rates.delegateModel<AudioNodeModel>(source);auto* resamplerModel=rates.delegateModel<AudioNodeModel>(resampler);
        check(sourceModel->parameter_model()->editLiteral("sample_rate","44100",1),"Source rate propagates up to explicit resampler boundary");
        const auto resampleInput=std::get<signal_contracts::NumericContract>(resamplerModel->numeric_node()->inputs[0].contract.view());
        const auto resampleOutput=std::get<signal_contracts::NumericContract>(resamplerModel->numeric_node()->outputs[0].contract.view());
        check(resampleInput.clock.sample_rate==44100 && resampleOutput.clock.sample_rate==48000 && resampleOutput.shape.extents[0].maximum==(256*48000+44100-1)/44100+1,"Resampler retains its own output rate and admitted variable block geometry");
        check(resamplerModel->parameter_model()->field("output_rate") && resamplerModel->parameter_model()->editLiteral("output_rate","32000",1),"Resampler exposes editable output sample rate");
        const auto downstream=std::get<signal_contracts::NumericContract>(rates.delegateModel<AudioNodeModel>(window)->numeric_node()->outputs[0].contract.view());
        check(downstream.clock.sample_rate==32000,"Resampler output-rate change re-clocks its downstream chain");
        const auto fresh=rates.addNode("AudioView");const auto beforeConnection=rates.save();
        QtNodes::DataFlowGraphicsScene scene(rates);const QtNodes::ConnectionId edge{resampler,0,fresh,0};
        scene.undoStack().push(new QtNodes::ConnectCommand(&scene,edge));
        check(rates.connectionExists(edge),"Connection adapts default downstream clock and PCM block bounds");
        scene.undoStack().push(new QtNodes::DisconnectCommand(&scene,edge));
        const auto disconnected=std::get<signal_contracts::NumericContract>(rates.delegateModel<AudioNodeModel>(fresh)->numeric_node()->inputs[0].contract.view());
        check(!rates.connectionExists(edge) && disconnected.clock.sample_rate==32000,"Ordinary disconnect retains known clock for typed-zero continuity");
        scene.undoStack().undo();check(rates.connectionExists(edge),"Undo disconnect must restore the connection with its current contract");
        scene.undoStack().undo();check(rates.save()==beforeConnection,"Undo connection creation restores adapted downstream contracts atomically");
        scene.undoStack().redo();check(rates.connectionExists(edge),"Redo connection creation adapts downstream clock again");
    }

    NumericEditorGraphModel graph(registry);QUndoStack undo;graph.set_undo_stack(&undo);

    for(const auto& kind:numeric_editor::catalog()){

        auto node=numeric_editor::default_node(kind);domain::ResolvedProcessingNode resolved;

        check(bool(domain::resolve_processing_node(node,numeric_editor::limits(),resolved)),qPrintable(kind));

    }

    {

        const auto source=graph.addNode("TestSignal"),view=graph.addNode("Converter");auto* target=graph.delegateModel<AudioNodeModel>(view);

        auto changed=*target->numeric_node();numeric_editor::set_dtype(changed,domain::PortDirection::Input,0,signal_contracts::DType::UInt16);graph.edit_node(view,changed);

        const QtNodes::ConnectionId edge{source,0,view,0};graph.addConnection(edge);

        check(graph.connectionExists(edge) && graph.connection_visual_state(edge)==2 && graph.connection_effects(edge).contains("65535"),"float to uint16 edge exposes potential range and precision loss");

        graph.edit_connection_policy(edge,3);check(graph.connection_policy(edge)==3 && graph.numeric_graph().edges.back().conversion==3,"edge policy is actual processing metadata");

        undo.undo();check(graph.connection_policy(edge)==0,"connection policy undo restores exact rejection");

        auto context=std::make_shared<pipeline::NumericDeliveryContext>();context->endpoint={"main",domain::NodeId(source),domain::PortDirection::Output,0};context->contract=graph.delegateModel<AudioNodeModel>(source)->numeric_node()->outputs[0].contract;

        check(graph.delegateModel<AudioNodeModel>(source)->preview(pipeline::DeliveredNumericFrame{context,{1,1},{},signal_contracts::Status::overflow}) && graph.connection_visual_state(edge)==3 && !graph.connection_failure(edge).isEmpty(),"reported kernel overflow changes connection to observed failure");

        graph.delegateModel<AudioNodeModel>(source)->clear_preview();check(graph.connection_visual_state(edge)==2,"clearing failed delivery restores potential risk state");

        graph.deleteNode(view);graph.deleteNode(source);

    }

    {

        std::fprintf(stderr,"editor fixture phase selector\n");
        const auto generator=graph.addNode("TestSignal"),audio=graph.addNode("AudioSource"),selector=graph.addNode("InputSelector");

        graph.addConnection({generator,0,selector,0});graph.addConnection({audio,0,selector,1});

        auto* model=graph.delegateModel<AudioNodeModel>(selector);auto* body=model->embeddedWidget();

        check(!body->findChild<QComboBox*>("numericSelected") && !body->findChild<QPushButton*>("addSelectorInput"),"selector has no central strip");
        check(!body->findChild<QWidget*>("pipelineDataStatus"),"no default diagnostic card in node body");
        model->select_input(1);undo.undo();check(model->selected_input()==0,"selector undo selection");undo.redo();
        auto changed=*model->numeric_node();check(numeric_editor::set_input_count(changed,3) && graph.edit_node(selector,changed),"add selector input");
        QApplication::processEvents();
        check(model->numeric_node()->inputs.size()==3 && graph.connectionExists({generator,0,selector,0}) && graph.connectionExists({audio,0,selector,1}),"retained stable edges after growth");
        graph.addConnection({generator,0,selector,2});model->select_input(2);
        changed=*model->numeric_node();numeric_editor::set_input_count(changed,2);check(graph.edit_node(selector,changed),"remove connected tail input");
        check(!graph.connectionExists({generator,0,selector,2}) && graph.connectionExists({audio,0,selector,1}),"only removed tail edge disappears");
        undo.undo();check(model->numeric_node()->inputs.size()==3 && graph.connectionExists({generator,0,selector,2}),"undo count restores removed edge");
        changed=*model->numeric_node();numeric_editor::set_input_count(changed,1);check(graph.edit_node(selector,changed) && graph.connectionExists({generator,0,selector,0}) && !graph.connectionExists({audio,0,selector,1}),"multi-tail shrink retains stable input zero");
        undo.undo();check(graph.connectionExists({audio,0,selector,1}) && graph.connectionExists({generator,0,selector,2}),"multi-tail undo restores exact IDs");
        QByteArray bytes;domain::NumericProcessingGraph restored;
        check(desktop::encode_numeric_graph(graph.numeric_graph(),bytes) && desktop::decode_numeric_graph(bytes,restored),"N selector typed save/load");
        NumericEditorGraphModel loaded(registry);check(loaded.set_numeric_graph(restored) && loaded.delegateModel<AudioNodeModel>(selector)->numeric_node()->inputs.size()==3,"N selector model load");
        QtNodes::DataFlowGraphicsScene scene(graph);scene.setNodeGeometry(std::make_unique<EditorNodeGeometry>(graph,[]{return QJsonObject{};}));
        for(const auto& language:{QString("ru"),QString("en")}) {
            ui::Language::instance().set(language);QApplication::processEvents();
            check(model->portCaption(QtNodes::PortType::In,2)==ui::text("Input %1").arg(3),"physical input captions match compact selector numbering");
            scene.nodeGeometry().recomputeSize(selector);
            check(scene.nodeGeometry().boundingRect(selector).contains(QRectF(scene.nodeGeometry().widgetPosition(selector),body->size())),"node bounds contain compact controls");
        }
        for(unsigned count:{1u,2u,3u,8u}) {
            auto n=*model->numeric_node();numeric_editor::set_input_count(n,count);check(graph.edit_node(selector,n) || model->nPorts(QtNodes::PortType::In)==count,"geometry count edit");
            auto& geometry=static_cast<EditorNodeGeometry&>(scene.nodeGeometry());geometry.recomputeSize(selector);
            const auto center=geometry.size(selector).height()/2.0;
            for(unsigned i=0;i<count;++i){const auto p=geometry.portPosition(selector,QtNodes::PortType::In,i);
                check(std::abs(p.y()+geometry.portPosition(selector,QtNodes::PortType::In,count-1-i).y()-2*center)<1e-6,"ports symmetric about whole node");
                check(geometry.checkPortHit(selector,QtNodes::PortType::In,p)==i && geometry.selectorButtonHit(selector,geometry.selectorButtonRect(selector,i).center())==i,"shared pin/button hit geometry");}
            check(geometry.portPosition(selector,QtNodes::PortType::Out,0).y()==center,"singleton side centered independently");
        }
        auto n=*model->numeric_node();numeric_editor::set_input_count(n,3);graph.edit_node(selector,n);
        graph.addConnection({audio,0,selector,1});graph.addConnection({generator,0,selector,2});model->select_input(2);
        n=*model->numeric_node();check(numeric_editor::remove_input(n,1) && graph.edit_node(selector,n),"remove arbitrary connected input");
        check(model->port_id(QtNodes::PortType::In,1)==2 && graph.connectionExists({generator,0,selector,1}) && model->selected_input()==2,"stable remaining input/connection/selection");
        const auto canonical=graph.numeric_graph();check(desktop::encode_numeric_graph(canonical,bytes) && desktop::decode_numeric_graph(bytes,restored) && loaded.set_numeric_graph(restored),"sparse IDs canonical round trip");
        check(loaded.connectionExists({generator,0,selector,1}) && loaded.delegateModel<AudioNodeModel>(selector)->port_id(QtNodes::PortType::In,1)==2,"sparse connection mapped on load");
        undo.undo();check(model->port_id(QtNodes::PortType::In,1)==1 && graph.connectionExists({audio,0,selector,1}) && graph.connectionExists({generator,0,selector,2}),"arbitrary delete undo exact endpoints");
        undo.redo();n=*model->numeric_node();check(numeric_editor::remove_input(n,0) && graph.edit_node(selector,n) && model->port_id(QtNodes::PortType::In,0)==2,"selector permits removal of original anchor zero");
        const auto sparseVisual=graph.save();NumericEditorGraphModel visualLoaded(registry);visualLoaded.load(sparseVisual);
        check(visualLoaded.delegateModel<AudioNodeModel>(selector)->port_id(QtNodes::PortType::In,0)==2 && visualLoaded.connectionExists({generator,0,selector,0}),"sparse visual document round trip");
        domain::NumericProcessingGraph decodedVisual;const auto visualBytes=QJsonDocument(sparseVisual).toJson(QJsonDocument::Compact);
        check(desktop::decode_numeric_editor_graph(visualBytes,decodedVisual) && std::any_of(decodedVisual.edges.begin(),decodedVisual.edges.end(),[&](const auto& e){return e.to==domain::NodeId(selector) && e.input_port==2;}),"visual indices decode to stable domain IDs");
        std::unique_ptr<QWidget> selectorForm(model->parameter_widget());auto* reset=selectorForm->findChild<QPushButton*>("numericResetSettings");check(reset,"selector reset inspector");reset->click();
        check(model->numeric_node()->inputs.size()==1 && model->port_id(QtNodes::PortType::In,0)==2 && graph.connectionExists({generator,0,selector,0}),"settings reset preserves selector topology and stable IDs");
        scene.clearScene();



    }

    {

        std::fprintf(stderr,"editor fixture phase window-function\n");
        const auto id=graph.addNode("WindowFunction");auto* window=graph.delegateModel<AudioNodeModel>(id);std::unique_ptr<QWidget> controls(window->parameter_widget());auto* function=controls->findChild<QLineEdit*>("function");check(function,"registered window-function editor exists");

        if(function){function->setText("hamming");function->setModified(true);QMetaObject::invokeMethod(function,"editingFinished",Qt::DirectConnection);}

        domain::ResolvedProcessingNode resolved;domain::ProcessingParameterView value;

        check(bool(domain::resolve_processing_node(*window->numeric_node(),numeric_editor::limits(),resolved)) && domain::resolve_processing_parameter(resolved,"function",value)==signal_contracts::Status::ok && std::get<std::string_view>(value)=="hamming","window function actual edit resolves supported algorithm");

        undo.undo();check(function && function->text()!="hamming","window-function undo refreshes registered editor");undo.redo();check(function && function->text()=="hamming","window-function redo preserves selected algorithm");graph.deleteNode(id);

    }

    std::fprintf(stderr,"editor fixture phase routing\n");
    const auto source=graph.addNode("NetworkInput"),route=graph.addNode("Splitter"),sink=graph.addNode("NetworkOutput"),other=graph.addNode("NetworkOutput");

    check(graph.connectionPossible({source,0,route,0}),"compatible typed source route");graph.addConnection({source,0,route,0});

    graph.addConnection({route,0,sink,0});graph.addConnection({route,1,other,0});

    {

        auto* sourceModel=graph.delegateModel<AudioNodeModel>(source);const QJsonObject adapter{{"provider_uuid","00000000-0000-4000-8000-000000000001"},{"stream_id","spectrum-main"},{"host","127.0.0.1"},{"port",47655},{"remote_endpoint",QJsonObject{{"graph_id","main"},{"node_id",4},{"direction","output"},{"port_index",0}}}};

        QByteArray beforeAdapter,afterAdapter;desktop::encode_numeric_graph(graph.numeric_graph(),beforeAdapter);

        check(graph.edit_adapter(source,adapter) && sourceModel->adapter()==adapter,"owned registered adapter edit");desktop::encode_numeric_graph(graph.numeric_graph(),afterAdapter);check(beforeAdapter==afterAdapter,"adapter remains outside DSP parameters");

        undo.undo();check(sourceModel->adapter().isEmpty(),"adapter undo clears route metadata");undo.redo();check(sourceModel->adapter()==adapter,"adapter redo restores route metadata");

        auto invalid=adapter;invalid["port"]=70000;check(!graph.edit_adapter(source,invalid) && sourceModel->adapter()==adapter,"invalid adapter edit is atomic");

        auto copy=graph.saveNode(source);copy["id"]=50;graph.loadNode(copy);check(graph.delegateModel<AudioNodeModel>(50)->adapter()==adapter,"clipboard remapped node preserves adapter");graph.deleteNode(50);

    }

    auto n=*graph.delegateModel<AudioNodeModel>(route)->numeric_node();numeric_editor::set_output_count(n,4);graph.edit_node(route,n);

    check(graph.connectionExists({route,0,sink,0}) && graph.connectionExists({route,1,other,0}),"grow preserves existing edges");

    const auto tail=graph.addNode("NetworkOutput");graph.addConnection({route,3,tail,0});

    n=*graph.delegateModel<AudioNodeModel>(route)->numeric_node();numeric_editor::set_output_count(n,2);

    const auto beforeShrink=graph.saveNode(route);const int undoBeforeShrink=undo.index();

    check(!graph.edit_node(route,n) && graph.saveNode(route)==beforeShrink && undo.index()==undoBeforeShrink && graph.connectionExists({route,3,tail,0}),"connected endpoint shrink refuses atomically");

    graph.deleteConnection({route,3,tail,0});check(graph.edit_node(route,n),"explicitly disconnected endpoint can shrink");

    check(graph.connectionExists({route,0,sink,0}),"explicit shrink preserves unaffected connection");

    undo.undo();check(graph.delegateModel<AudioNodeModel>(route)->nPorts(QtNodes::PortType::Out)==4 && !graph.connectionExists({route,3,tail,0}),"undo preserves explicit disconnection");undo.redo();

    {

        auto* routing=graph.delegateModel<AudioNodeModel>(route);std::unique_ptr<QWidget> routeForm(routing->parameter_widget());auto* routeBody=routing->embeddedWidget();

        check(routing->parameter_on_block("numericOutputs"),"splitter output count is primary without opt-in");

        auto* inlineCount=routeBody->findChild<QSpinBox*>("numericOutputs");check(inlineCount && inlineCount->isVisibleTo(routeBody),"routing selected control visible inline");

        std::fprintf(stderr,"editor fixture phase inline-routing\n");
        if(inlineCount)inlineCount->setValue(4);QApplication::processEvents();

        check(routing->nPorts(QtNodes::PortType::Out)==4 && routeForm->findChild<QAction*>("showOnBlock_output3Dtype"),"inline routing grow updates ports and full inspector fields");

        undo.undo();QApplication::processEvents();

        check(routing->nPorts(QtNodes::PortType::Out)==2 && !routeForm->findChild<QAction*>("showOnBlock_output3Dtype") && routing->parameter_on_block("numericOutputs"),"routing undo restores form shape and keeps visibility flag");

    }

    auto mismatch=*graph.delegateModel<AudioNodeModel>(tail)->numeric_node();numeric_editor::set_dtype(mismatch,domain::PortDirection::Input,0,signal_contracts::DType::UInt64);graph.edit_node(tail,mismatch);

    check(graph.connectionPossible({source,0,tail,0}),"dtype-only link supports prepared conversion");graph.addConnection({source,0,tail,0});check(graph.connectionExists({source,0,tail,0}),"raw add admits dtype-only conversion");graph.deleteConnection({source,0,tail,0});

    std::fprintf(stderr,"editor fixture phase typed-converter\n");
    auto converter=graph.addNode("Converter");auto* model=graph.delegateModel<AudioNodeModel>(converter);

    std::fprintf(stderr,"editor converter before form\n");
    std::unique_ptr<QWidget> form(model->parameter_widget());
    std::fprintf(stderr,"editor converter after form\n");

    auto* type=form->findChild<QComboBox*>("numericNan_type");auto* text=form->findChild<QLineEdit*>("numericNan");

    check(type && text,"exact typed field exists");

    std::fprintf(stderr,"editor converter before type\n");
    if(type && text){type->setCurrentIndex(1);std::fprintf(stderr,"editor converter after type\n");text=form->findChild<QLineEdit*>("numericNan");text->setText("18446744073709551615");text->setModified(true);QMetaObject::invokeMethod(text,"editingFinished",Qt::DirectConnection);}

    std::fprintf(stderr,"editor converter after edit\n");
    auto value=[&]{return std::get<domain::ConverterProcessingParameters>(model->numeric_node()->parameters).policy.nan_replacement;};

    check(std::holds_alternative<std::uint64_t>(value()) && std::get<std::uint64_t>(value())==std::numeric_limits<std::uint64_t>::max(),"UI UInt64 exact");

    std::fprintf(stderr,"editor converter before undo\n");
    undo.undo();check(std::get<std::uint64_t>(value())==0,"undo exact field");undo.redo();

    auto* signedText=form->findChild<QLineEdit*>("numericNegativeInf");

    if(signedText){signedText->setText("-9223372036854775808");signedText->setModified(true);QMetaObject::invokeMethod(signedText,"editingFinished",Qt::DirectConnection);}

    check(std::get<std::int64_t>(std::get<domain::ConverterProcessingParameters>(model->numeric_node()->parameters).policy.negative_inf_replacement)==std::numeric_limits<std::int64_t>::min(),"UI Int64 minimum exact");

    auto* exponent=form->findChild<QSpinBox*>("numericGainExponent");check(exponent,"dyadic gain exponent is editable");

    if(exponent)exponent->setValue(-17);

    check(std::get<domain::ConverterProcessingParameters>(model->numeric_node()->parameters).policy.scaling.gain.exponent2==-17,"dyadic exponent edit preserves signed value");

    std::fprintf(stderr,"editor converter before inline\n");
    auto* body=model->embeddedWidget();auto* inlineText=body->findChild<QLineEdit*>("numericNan");

    auto* show=form->findChild<QAction*>("showOnBlock_numericNan");

    check(show && inlineText && !show->isChecked() && !inlineText->isVisibleTo(body),"compact default hides editable parameters");

    QByteArray processingBefore;desktop::encode_numeric_graph(graph.numeric_graph(),processingBefore);

    if(show)show->setChecked(true);

    check(inlineText && inlineText->isVisibleTo(body) && inlineText->text()=="18446744073709551615","selected inline parameter displays exact inspector value");

    QByteArray processingAfter;desktop::encode_numeric_graph(graph.numeric_graph(),processingAfter);

    check(processingBefore==processingAfter,"presentation flags leak into processing serialization");

    undo.undo();check(show && !show->isChecked() && !inlineText->isVisibleTo(body),"visibility undo synchronizes inspector and block");

    undo.redo();check(show && show->isChecked() && inlineText->isVisibleTo(body),"visibility redo restores block field");

    if(inlineText){inlineText->setText("9007199254740993");inlineText->setModified(true);QMetaObject::invokeMethod(inlineText,"editingFinished",Qt::DirectConnection);}

    check(text && text->text()=="9007199254740993" && std::get<std::uint64_t>(value())==9007199254740993ULL,"inline exact edit synchronizes inspector");

    undo.undo();check(inlineText->text()=="18446744073709551615" && text->text()==inlineText->text(),"parameter undo synchronizes both forms");

    if(show)show->setChecked(false);

    check(!inlineText->isVisibleTo(body) && std::get<std::uint64_t>(value())==std::numeric_limits<std::uint64_t>::max(),"hiding inline preserves parameter value");

    if(show)show->setChecked(true);

    const auto beforeNameUndo=undo.count();
    std::fprintf(stderr,"editor converter before custom presentation node=%u exists=%d\n",unsigned(converter),int(graph.nodeExists(converter)));
    auto display=model->presentation();display["future_display_field"]="kept";model->set_presentation(display);
    check(model->edit_display_name(QString::fromUtf8("Мой преобразователь")) && model->caption()==QString::fromUtf8("Мой преобразователь") && model->name()=="Converter","Custom caption preserves stable algorithm kind");
    QByteArray namedProcessing;desktop::encode_numeric_graph(graph.numeric_graph(),namedProcessing);
    check(namedProcessing==processingBefore && undo.count()==beforeNameUndo+1,"Node rename is one presentation-only undo command");
    const auto namedPresentation=model->presentation();
    check(!model->edit_display_name(QString(257,'x')) && !model->edit_display_name(QString("bad")+QChar(1)) && model->presentation()==namedPresentation,"Invalid display names refuse without mutation");
    auto malformedName=model->save();auto invalidDisplay=namedPresentation;invalidDisplay["display_name"]=17;malformedName["presentation"]=invalidDisplay;
    AudioNodeModel malformedCaption("Converter");malformedCaption.load(malformedName);
    check(!malformedCaption.numeric_node(),"Malformed persisted display name must reject the node rather than silently discard metadata");
    undo.undo();check(model->caption()!=QString::fromUtf8("Мой преобразователь") && model->presentation()["future_display_field"]=="kept","Rename undo preserves unknown presentation fields");
    undo.redo();check(model->caption()==QString::fromUtf8("Мой преобразователь"),"Rename redo restores user caption");

    const auto saved=graph.saveNode(converter);auto copied=saved;copied["id"]=100;graph.loadNode(copied);

    auto* copy=graph.delegateModel<AudioNodeModel>(100);

    check(copy && copy->numeric_node() && copy->numeric_node()->id==100 && domain::same_processing_parameters(model->numeric_node()->parameters,copy->numeric_node()->parameters),"clipboard remaps id preserves exact literals");

    check(copy && copy->presentation()==model->presentation() && copy->parameter_on_block("numericNan"),"node snapshot preserves presentation flags");

    {

        QtNodes::DataFlowGraphicsScene scene(graph);scene.nodeGraphicsObject(converter)->setSelected(true);

        QtNodes::CopyCommand copyCommand(&scene);copyCommand.redo();const auto before=graph.allNodeIds();

        scene.undoStack().push(new QtNodes::PasteCommand(&scene,QPointF{1000,1000}));

        QtNodes::NodeId pasted=QtNodes::InvalidNodeId;for(auto id:graph.allNodeIds())if(!before.contains(id))pasted=id;

        auto* pastedNode=graph.delegateModel<AudioNodeModel>(pasted);

        check(pastedNode && pastedNode->numeric_node()->id==domain::NodeId(pasted) && domain::same_processing_parameters(model->numeric_node()->parameters,pastedNode->numeric_node()->parameters),"actual QtNodes clipboard exact typed snapshot");

        check(pastedNode && pastedNode->presentation()==model->presentation() && pastedNode->caption()==model->caption() && pasted!=converter,"actual clipboard retains user caption and visibility with a distinct stable ID");

        scene.undoStack().undo();check(!graph.nodeExists(pasted),"paste undo removes copy");scene.undoStack().redo();check(graph.nodeExists(pasted),"paste redo restores copy");

    }

    QByteArray bytes;check(bool(desktop::encode_numeric_graph(graph.numeric_graph(),bytes)),"known snapshot encode");domain::NumericProcessingGraph restored;check(bool(desktop::decode_numeric_graph(bytes,restored)),"known snapshot decode");

    if(restored.nodes.empty()){check(false,"Known snapshot unexpectedly contains no restored nodes");return 1;}

    NumericEditorGraphModel restoredGraph(registry);restoredGraph.set_numeric_graph(restored);check(restoredGraph.connectionExists({route,0,sink,0}),"typed graph reload keeps ports");

    {

        auto timed=restored;timed.execution_interval={1,40};
        check(restoredGraph.set_numeric_graph(timed),"custom analysis interval imports");
        NumericEditorGraphModel timedReload(registry);timedReload.load(restoredGraph.save());
        check(timedReload.numeric_graph().execution_interval==timed.execution_interval,"editor save/load preserves physical interval");
        auto invalidInterval=timed;invalidInterval.execution_interval={2,80};const auto intervalBefore=restoredGraph.save();
        check(!restoredGraph.set_numeric_graph(invalidInterval) && restoredGraph.save()==intervalBefore,"nonreduced interval refuses atomically");
        check(restoredGraph.set_numeric_graph(restored),"restore ordinary fixture");
        const auto before=restoredGraph.save();auto invalid=restored;

        invalid.nodes[0].kind="UnknownNumericNode";QString error;

        check(!restoredGraph.set_numeric_graph(invalid,&error) && !error.isEmpty() && restoredGraph.save()==before,"unknown node import refuses atomically");

        invalid=restored;invalid.edges.push_back({-1,domain::NodeId(sink),0,0});

        check(!restoredGraph.set_numeric_graph(invalid,&error) && restoredGraph.save()==before,"invalid edge import refuses without losing draft");

        invalid=restored;invalid.versions.processing=77;

        check(!restoredGraph.set_numeric_graph(invalid,&error) && restoredGraph.save()==before,"future processing edition refuses atomically");

        auto external=restored;external.versions.control=77;external.versions.data=78;

        check(restoredGraph.set_numeric_graph(external) && restoredGraph.numeric_graph().versions.control==77 && restoredGraph.numeric_graph().versions.data==78,"external protocol editions survive local draft editing");

        NumericEditorGraphModel empty(registry);check(empty.set_numeric_graph(domain::NumericProcessingGraph{}),"empty typed draft loads");

        check(empty.has_numeric_nodes() && empty.save().value("version").toInt()==3,"empty typed draft retains source-backed save mode");

        const auto only=empty.addNode("Converter");empty.deleteNode(only);

        check(empty.has_numeric_nodes(),"deleting final typed node never falls back to legacy writer");

    }

    {

        QByteArray sourceBytes="{\"schema_version\":2,\"scene_graph\":"+bytes+",\"ui\":{\"unknown\":9007199254740993},\"app\":-0}";

        desktop::NumericProjectDocument sourceDocument,staged,reloaded;domain::NumericProcessingGraph snapshot,readback;

        check(bool(desktop::read_numeric_project(sourceBytes,sourceDocument)),"lossless project baseline");

        check(restoredGraph.export_numeric_graph(snapshot),"owned draft export");

        for(auto& node:snapshot.nodes)if(node.id==domain::NodeId(converter))std::get<domain::ConverterProcessingParameters>(node.parameters).policy.positive_inf_replacement=std::uint64_t{9007199254740993ULL};

        check(bool(desktop::stage_numeric_draft(sourceDocument,snapshot,staged)),"typed GUI snapshot stages source-backed draft");

        desktop::NumericDraftPresentation visibility;visibility.node_presentations[QString::number(converter)]=QJsonObject{
            {"inline_parameters",model->presentation()["inline_parameters"]},{"display_name",model->caption()}};

        desktop::NumericProjectDocument presented;check(bool(desktop::stage_numeric_presentation(staged,visibility,presented)),"presentation flags stage separately from processing DTO");staged=std::move(presented);

        QTemporaryDir directory;check(directory.isValid(),"temporary project directory");const auto path=directory.filePath("config.json");

        check(bool(desktop::save_numeric_project(staged,path)),"one-file draft save");

        check(bool(desktop::load_numeric_project(path,reloaded,readback)),"one-file draft reload");
        QString readbackError;
        if(!restoredGraph.set_numeric_graph(readback,&readbackError)){std::fprintf(stderr,"one-file draft import: %s\n",qPrintable(readbackError));check(false,"one-file draft imports owning graph");return 1;}

        desktop::NumericDraftPresentation loadedPresentation;check(bool(desktop::read_numeric_presentation(reloaded,loadedPresentation)),"read owning presentation after reload");

        std::fprintf(stderr,"editor converter before reloaded presentation node=%u exists=%d\n",unsigned(converter),int(restoredGraph.nodeExists(converter)));
        if(!restoredGraph.delegateModel<AudioNodeModel>(converter)){check(false,"reloaded draft retains original converter ID");return 1;}
        restoredGraph.delegateModel<AudioNodeModel>(converter)->set_presentation(loadedPresentation.node_presentations.value(QString::number(converter)).toObject());

        check(restoredGraph.delegateModel<AudioNodeModel>(converter)->parameter_on_block("numericNan"),"visibility survives one-file project reload");

        const auto& p=std::get<domain::ConverterProcessingParameters>(restoredGraph.delegateModel<AudioNodeModel>(converter)->numeric_node()->parameters).policy;

        check(std::get<std::uint64_t>(p.nan_replacement)==std::numeric_limits<std::uint64_t>::max() && std::get<std::int64_t>(p.negative_inf_replacement)==std::numeric_limits<std::int64_t>::min() && std::get<std::uint64_t>(p.positive_inf_replacement)==9007199254740993ULL,"exact UI literals survive source-backed save/reload");

        QFile file(path);check(file.open(QIODevice::ReadOnly) && file.readAll().contains("\"unknown\":9007199254740993"),"unknown raw64 preserved beside GUI edits");

    }

    auto history=graph.addNode("Spectrogram");auto* accumulator=graph.delegateModel<AudioNodeModel>(history);std::unique_ptr<QWidget> historyForm(accumulator->parameter_widget());auto* capacity=historyForm->findChild<QLineEdit*>("numericCapacity");
    const auto historyContract=std::get<signal_contracts::NumericContract>(accumulator->numeric_node()->outputs[0].contract.view());
    check(historyContract.coordinates[0].mode==signal_contracts::CoordinateMode::ExplicitFloat64LE && historyContract.coordinates[0].unit==signal_contracts::NumericUnit::Seconds,"new History preserves individual source-clock seconds");

    if(capacity){capacity->setText("17");capacity->setModified(true);QMetaObject::invokeMethod(capacity,"editingFinished",Qt::DirectConnection);}

    auto h=*accumulator->numeric_node();domain::ResolvedProcessingNode hr;

    check(bool(domain::resolve_processing_node(h,numeric_editor::limits(),hr)) && std::get<signal_contracts::NumericContract>(h.outputs[0].contract.view()).shape.extents[0].maximum==17,"history capacity edits both configured parameter and contract");

    auto bad=graph.saveNode(source);bad["id"]=101;auto data=bad.value("internal-data").toObject();data["numeric_processing"]="{invalid";bad["internal-data"]=data;graph.loadNode(bad);

    check(!graph.connectionPossible({101,0,sink,0}),"malformed typed snapshot never defaults to connectable node");

    graph.loadNode({{"id",102},{"internal-data",QJsonObject{{"model-name","NetworkOutput"}}}});

    check(!graph.connectionPossible({101,0,102,0}),"malformed typed snapshot never falls back to legacy ports");

    auto changed=*graph.delegateModel<AudioNodeModel>(tail)->numeric_node();

    auto contract=changed.inputs[0].contract.view();std::get<signal_contracts::NumericContract>(contract).unit=signal_contracts::NumericUnit::Values;

    numeric_editor::set_contract(changed,domain::PortDirection::Input,0,contract);numeric_editor::set_dtype(changed,domain::PortDirection::Input,0,signal_contracts::DType::Float32);graph.edit_node(tail,changed);

    check(!graph.connectionPossible({source,0,tail,0}),"units mismatch denied for same dtype shape");

    {

        QJsonArray choices{QJsonObject{{"id","render-A"},{"name","Endpoint A"},{"is_default",true}},QJsonObject{{"id","render-B"},{"name","Endpoint B"}}};

        int enumerations=0;graph.set_capture_device_provider([&](int kind,QString*){++enumerations;return kind==0?choices:QJsonArray{};});

        const auto physical=graph.addNode("AudioSource"),view=graph.addNode("AudioView");graph.addConnection({physical,0,view,0});

        check(graph.connectionExists({physical,0,view,0}),"Graph data delivery remains safe after clipboard scene destruction");

        auto* node=graph.delegateModel<AudioNodeModel>(physical);

        if(!node || !node->numeric_node()){check(false,"Fresh physical node missing numeric DTO");return 1;}

        std::unique_ptr<QWidget> controls(node->parameter_widget());

        auto* devices=controls->findChild<QComboBox*>("captureDevice");auto* refresh=controls->findChild<QPushButton*>("refreshCaptureDevices");

        if(!devices || !refresh){check(false,"Physical endpoint chooser controls missing");return 1;}

        check(devices->count()==3,"Physical endpoint chooser exposes default and discovered devices");

        const auto configured=*node->numeric_node();const auto edges=graph.allConnectionIds(physical);const int beforeDevice=undo.index();

        if(devices)devices->setCurrentIndex(devices->findData("render-B"));

        check(node->adapter().value("device_identity")=="render-B" && undo.index()==beforeDevice+1 && graph.allConnectionIds(physical)==edges && signal_contracts::same_contract(configured.outputs[0].contract.view(),node->numeric_node()->outputs[0].contract.view()),"Physical endpoint choice atomically preserves declared PCM and links");

        auto* sourceKind=controls->findChild<QComboBox*>("source");const auto beforeKind=node->save();const int beforeKindUndo=undo.index();if(sourceKind)sourceKind->setCurrentIndex(1);

        check(sourceKind && sourceKind->currentIndex()==0 && node->save()==beforeKind && undo.index()==beforeKindUndo && graph.allConnectionIds(physical)==edges,"Capture kind change silently reuses incompatible pinned endpoint");

        undo.undo();check(node->adapter().isEmpty() && devices->currentData().toString().isEmpty(),"Endpoint undo restores OS default and registered chooser");undo.redo();

        choices={QJsonObject{{"id","render-A"},{"name","Endpoint A"},{"is_default",true}}};const int beforeRefresh=undo.index();if(refresh)refresh->click();

        check(node->adapter().value("device_identity")=="render-B" && devices->currentData()=="render-B" && devices->currentText().contains(ui::text("Unavailable device: %1").arg("render-B")) && undo.index()==beforeRefresh,"Refresh silently loses unavailable pinned endpoint");

        auto unavailable=node->adapter();unavailable["device_identity"]="missing-C";check(!graph.edit_adapter(physical,unavailable) && undo.index()==beforeRefresh && graph.allConnectionIds(physical)==edges,"Unknown endpoint bypasses atomic model admission");

        if(devices)devices->setCurrentIndex(0);check(node->adapter().isEmpty() && graph.allConnectionIds(physical)==edges && enumerations>=3,"OS default endpoint repair loses links or does not discover");

        graph.deleteNode(physical);const int afterDeletion=enumerations;

        refresh->click();const auto language=ui::Language::instance().code();

        ui::Language::instance().set(language=="ru"?"en":"ru");ui::Language::instance().set(language);

        check(enumerations==afterDeletion,"External capture form accesses discovery after its model is deleted");

    }

    graph.set_capture_device_provider({});

    std::printf("numeric editor failures=%d\n",failures);return failures?1:0;

}

