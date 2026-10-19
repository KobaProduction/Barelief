#include "synthetic_capture.h"
#include <core_app/core_application.h>
#include <core_app/main_window.h>
#include <core_app/numeric_default_graph.h>
#include <core_app/numeric_view_adapter.h>
#include <core_app/scheme_editor/editor_widget.h>
#include <core_app/scheme_editor/numeric_editor_model.h>
#include <core_app/scheme_editor/audio_node_model.h>
#include <core_app/scheme_editor/node_parameter_model.h>
#include <desktop/numeric_project_document.h>
#include <offline/profile_json.h>
#include <barelief/numeric_image_view.h>
#include <barelief/ui_text.h>
#include <QtNodes/internal/NodeGraphicsObject.hpp>
#include <QAction>
#include <QDialog>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QMenu>
#include <QLabel>
#include <QListWidget>
#include <QPointer>
#include <QPushButton>
#include <QTableWidget>
#include <QTemporaryDir>
#include <QThread>
#include <QStatusBar>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <functional>
#include <map>
#include <mutex>

using namespace signal_contracts;
#define CHECK(x) do { if(!(x)){std::fprintf(stderr,"FAIL %d %s\n",__LINE__,#x);return 1;} } while(false)
static bool wait_until(const std::function<bool()>& predicate,int timeout=7000) {
    QElapsedTimer timer;timer.start();
    while(timer.elapsed()<timeout){QCoreApplication::processEvents();if(predicate())return true;QThread::msleep(5);}return predicate();
}
static QByteArray read(const QString& path) {QFile f(path);return f.open(QIODevice::ReadOnly)?f.readAll():QByteArray{};}
static bool write_tones(const QString& path) {
    constexpr unsigned rate=48000,samples=48000;
    QByteArray bytes;auto word=[&](unsigned value,unsigned width){for(unsigned b=0;b<width;++b)bytes.append(char(value>>(8*b)));};
    bytes.append("RIFF",4);word(36+samples*2,4);bytes.append("WAVEfmt ",8);word(16,4);word(1,2);word(1,2);word(rate,4);word(rate*2,4);word(2,2);word(16,2);bytes.append("data",4);word(samples*2,4);
    for(unsigned i=0;i<samples;++i) {
        double value=0;for(unsigned tone=0;tone<8;++tone)value+=.04*std::sin(6.283185307179586*(130.+tone*tone*173.)*i/rate+.3*std::sin(double(i)/rate+tone));
        word(unsigned(std::int16_t(value*32767)),2);
    }
    QFile file(path);return file.open(QIODevice::WriteOnly)&&file.write(bytes)==bytes.size();
}
static bool separated_layout(MainWindow* window,NumericEditorGraphModel* model) {
    auto* editor=window->findChild<SchemeEditorWidget*>();if(!editor)return false;
    auto* scene=editor->findChild<QtNodes::DataFlowGraphicsScene*>();if(!scene)return false;
    std::vector<QRectF> bounds;
    for(auto id:model->allNodeIds()) {
        auto* item=scene->nodeGraphicsObject(id);if(!item)return false;
        const auto rectangle=item->sceneBoundingRect();if(rectangle.isEmpty())return false;
        for(const auto& previous:bounds)if(rectangle.intersects(previous))return false;
        bounds.push_back(rectangle);
    }
    return !bounds.empty();
}
static bool finite_values(const pipeline::DeliveredNumericFrame& frame) {
    if(!frame.frame || !frame.frame.view().stamp.available)return false;
    const auto view=std::get<NumericView>(frame.frame.view().payload);
    const auto columns=view.contract->shape.rank==Rank::Matrix?view.actual[1]:1;
    for(size_t plane=0;plane<view.planes.size();++plane)
        for(size_t row=0;row<view.actual[0];++row)for(size_t column=0;column<columns;++column) {
            NumericLiteral value;if(pipeline::read_numeric_literal(view,{plane,row,column},value)!=Status::ok)return false;
            if(!std::visit([](auto x){return std::isfinite(double(x));},value))return false;
        }
    return true;
}
// Test-only capture adapter keeps all eight moving tones below every selected
// native Nyquist; no branch opens a host audio device or playback stream.
class MusicCapture final : public pipeline::IAudioSource {
    std::unique_ptr<pipeline::IAudioSource> worker_;
    static QJsonObject bounded(QJsonObject parameters) {parameters["maximum_hz"]=10000.;parameters["tones"]=8;return parameters;}
public:
    explicit MusicCapture(pipeline::AudioSourceKind kind):worker_(std::make_unique<test_fixture::SyntheticCapture>(kind,true,kind==pipeline::AudioSourceKind::SystemOutput)){}
    void start()override{worker_->start();}void stop()override{worker_->stop();}
    void configure(const QJsonObject& p)override{worker_->configure(bounded(p));}
    bool reconfigure_live(const QJsonObject& p)override{return worker_->reconfigure_live(bounded(p));}
    bool is_running()const override{return worker_->is_running();}
    pipeline::AudioSourceKind source()const override{return worker_->source();}
    pipeline::AudioSourceDiagnostics diagnostics()const override{return worker_->diagnostics();}
    void set_audio_handler(AudioHandler h)override{worker_->set_audio_handler(std::move(h));}
    void set_status_handler(StatusHandler h)override{worker_->set_status_handler(std::move(h));}
    void set_lifecycle_handler(LifecycleHandler h)override{worker_->set_lifecycle_handler(std::move(h));}
};
int main(int argc,char** argv) {
    CHECK(qEnvironmentVariable("QT_QPA_PLATFORM")=="offscreen");
    QCoreApplication::setAttribute(Qt::AA_UseSoftwareOpenGL);
    QString export_path,image_dir;unsigned native_rate=48000;
    for(int i=1;i<argc;++i) {
        if(QByteArray(argv[i])=="--native32000")native_rate=32000;
        else if(QByteArray(argv[i])=="--export" && i+1<argc)export_path=QString::fromLocal8Bit(argv[++i]);
        else if(QByteArray(argv[i])=="--image-dir" && i+1<argc)image_dir=QString::fromLocal8Bit(argv[++i]);
        else {std::fprintf(stderr,"Unknown test option %s\n",argv[i]);return 1;}
    }
    QTemporaryDir directory;CHECK(directory.isValid());
    // Preserve existing evidence exports; each repeated test still exercises Save As.
    if(!export_path.isEmpty() && QFile::exists(export_path))export_path=directory.filePath("fresh-export.json");
    const auto input_path=directory.filePath("original.json"),saved_path=directory.filePath("saved.json");
    desktop::NumericDraftPresentation display;
    display.application_settings={{"ui",QJsonObject{{"autosave_enabled",false},{"show_data_flow",true}}}};
    auto initial_graph=numeric_generator_graph();initial_graph.graph_id="music-test-custom-stable";
    desktop::NumericProjectDocument project;CHECK(desktop::create_numeric_project(initial_graph,display,project));
    CHECK(desktop::save_numeric_project(project,input_path));const auto original=read(input_path);
    auto encoded=input_path.toLocal8Bit();char executable[]="music_gui_tests",option[]="--config";
    char* args[]{executable,option,encoded.data(),nullptr};int count=3;
    CoreApplication app(count,args,[](pipeline::AudioSourceKind kind)->std::unique_ptr<pipeline::IAudioSource>{return std::make_unique<MusicCapture>(kind);});
    for(const auto* kind:{"RFFT","SpectrumProjection","FrequencyNormalizer","BroadbandFeatures","NoteSalience","SpectralQuantize"}) {
        const auto node=numeric_editor::default_node(kind);domain::ResolvedProcessingNode resolved;
        CHECK(domain::resolve_processing_node(node,numeric_editor::limits(),resolved));
    }
    app._ctx.file_configs.ui.autosave_enabled=false;
    auto* model=dynamic_cast<NumericEditorGraphModel*>(app._ctx.data_flow_graph.get());CHECK(model);
    QString edit_rejection;QObject::connect(model,&NumericEditorGraphModel::edit_rejected,&app,[&](const QString& error){edit_rejection=error;});
    for(const auto* kind:{"SpectrumProjection","RFFT","SpectralQuantize"}) {
        NumericEditorGraphModel fresh(model->dataModelRegistry());
        domain::NumericProcessingGraph draft;draft.graph_id="fresh-catalog-node";
        auto node=numeric_editor::default_node(kind);node.id=900;draft.nodes={node};CHECK(fresh.set_numeric_graph(draft));
        auto* delegate=fresh.delegateModel<AudioNodeModel>(900);CHECK(delegate);
        CHECK(delegate->parameter_model()->edit(kind==std::string_view("SpectrumProjection")?"representation":kind==std::string_view("RFFT")?"fft_size":"bits",
            kind==std::string_view("SpectrumProjection")?2:kind==std::string_view("RFFT")?0:1));
    }
    model->set_capture_device_provider([native_rate](int,QString*) {
        return QJsonArray{QJsonObject{{"id","synthetic-capture"},{"name","Synthetic capture"},{"is_default",true},
            {"native_sample_rate",int(native_rate)},{"native_channels",1}}};
    });
    MainWindow* window=nullptr;
    for(auto* candidate:QApplication::topLevelWidgets())if(auto* found=qobject_cast<MainWindow*>(candidate))window=found;
    CHECK(window);window->show();
    CHECK(wait_until([&]{return app.presenter().client().snapshot().graph_revision>0&&!app.presenter().graph_request_pending();}));
    auto* menu=window->findChild<QMenu*>("pipelinePresetsMenu");CHECK(menu);
    auto* preset=menu->findChild<QAction*>("example-music-research");CHECK(preset&&preset->isVisible());
    const auto old_revision=app.presenter().client().snapshot().graph_revision;preset->trigger();
    CHECK(wait_until([&]{return app.presenter().client().snapshot().graph_revision>old_revision&&!app.presenter().graph_request_pending()&&separated_layout(window,model);}));
    CHECK(model->numeric_graph().execution_interval==(Ratio{1,60}));
    CHECK(model->numeric_graph().graph_id==initial_graph.graph_id);
    const auto research_graph=model->numeric_graph();
    const auto coefficient_generator=std::find_if(research_graph.nodes.begin(),research_graph.nodes.end(),[](const auto& n){return n.id==3;});
    CHECK(coefficient_generator!=research_graph.nodes.end() && coefficient_generator->kind=="WindowFunction");
    const auto n_input=std::get<NumericContract>(coefficient_generator->inputs[0].contract.view());
    CHECK(n_input.dtype==DType::UInt32 && n_input.shape.rank==Rank::Scalar);
    CHECK(std::any_of(research_graph.edges.begin(),research_graph.edges.end(),[](const auto& e){return e.from==1000 && e.to==3 && e.input_port==0;}));
    CHECK(std::any_of(research_graph.edges.begin(),research_graph.edges.end(),[](const auto& e){return e.from==2 && e.to==1001 && e.input_port==0;}));
    CHECK(std::any_of(research_graph.edges.begin(),research_graph.edges.end(),[](const auto& e){return e.from==3 && e.to==1001 && e.input_port==1;}));
    CHECK(std::any_of(research_graph.edges.begin(),research_graph.edges.end(),[](const auto& e){return e.from==1001 && e.to==4 && e.input_port==0;}));

    for(int id:{8,18,9,10,11,12,19,20}){
        CHECK(!domain::has_configuration_binding(model->numeric_graph(),id,domain::ConfigurationRole::SampleClock));
        CHECK(!domain::has_configuration_binding(model->numeric_graph(),id,domain::ConfigurationRole::FrequencyGrid));
        const auto* view=model->delegateModel<AudioNodeModel>(id);CHECK(view);
        const auto* configured=view->numeric_node();CHECK(configured);
        CHECK(std::any_of(configured->inputs.begin(),configured->inputs.end(),[](const auto& p){return p.id==0;}));
        for(const auto& port:configured->inputs)if(port.id!=0){CHECK(port.id==domain::frequency_step_input);const auto value=std::get<NumericContract>(port.contract.view());CHECK(value.dtype==DType::Float64 && value.shape.rank==Rank::Scalar);}
    }
    const auto graph_id=model->numeric_graph().graph_id;
    auto endpoint=[&](int id,domain::PortId port=0){return pipeline::EndpointIdentity{graph_id,id,domain::PortDirection::Output,port};};
    std::map<int,pipeline::DeliveredNumericFrame> latest;std::mutex latest_mutex;
    auto query=[&](int id,pipeline::DeliveredNumericFrame& frame){
        if(id==7||id==17)return app.pipeline().query_numeric(endpoint(id),frame)==Status::ok&&finite_values(frame);
        {std::lock_guard lock(latest_mutex);const auto found=latest.find(id);if(found==latest.end())return false;frame=found->second;}
        return frame.status==Status::ok&&frame.delivery.graph_revision==app.presenter().client().snapshot().graph_revision&&finite_values(frame);
    };
    std::vector<pipeline::EndpointIdentity> endpoints;
    for(int id:{200,201,2,3,4,5,6,30,31,32,33,34,7,17})endpoints.push_back(endpoint(id));
    const auto tap=app.pipeline().subscribe_numeric_tap(endpoints,[&](pipeline::DeliveredNumericFrame frame){
        if(!frame.context)return;std::lock_guard lock(latest_mutex);latest[frame.context->endpoint.node]=std::move(frame);
    },{});CHECK(tap);
    struct Guard{pipeline::NumericEngine& engine;std::uint64_t tap;~Guard(){engine.unsubscribe_numeric_tap(tap);}} guard{app.pipeline(),tap};
    pipeline::DeliveredNumericFrame broad,note,pcm,fft,phase,power;
    const bool initial_ready=wait_until([&]{return query(7,broad)&&query(17,note)&&query(201,pcm)&&query(4,fft)&&query(5,power)&&query(30,phase);});
    if(!initial_ready) {
        std::fprintf(stderr,"Initial music query failure graph=%s revision=%llu diagnostics=%s\n",graph_id.c_str(),
            static_cast<unsigned long long>(app.presenter().client().snapshot().graph_revision),
            QJsonDocument(app.pipeline().snapshot().diagnostics).toJson(QJsonDocument::Compact).constData());
        for(int id:{200,201,2,3,4,5,6,30,31,32,33,34,7,17}) {
            pipeline::DeliveredNumericFrame frame;const auto status=app.pipeline().query_numeric(endpoint(id),frame);
            const bool present=bool(frame.frame),available=present&&frame.frame.view().stamp.available;
            std::fprintf(stderr,"Node %d query=%u owner=%d available=%d finite=%d\n",id,unsigned(status),present,available,
                status==Status::ok&&present&&available?finite_values(frame):false);
            pipeline::DeliveredNumericFrame tapped;std::fprintf(stderr,"Node %d current owning tap valid=%d\n",id,query(id,tapped));
        }
    }
    CHECK(initial_ready);
    CHECK(std::get<NumericView>(pcm.frame.view().payload).contract->clock.sample_rate==32768);
    CHECK(std::get<NumericView>(fft.frame.view().payload).actual[0]==2049);
    CHECK(std::get<NumericView>(phase.frame.view().payload).contract->unit==NumericUnit::Radians);
    CHECK(std::get<NumericView>(power.frame.view().payload).contract->unit==NumericUnit::SquaredAmplitude);
    std::uint64_t ordinal=0,filled=0;
    CHECK(wait_until([&]{return app.pipeline().query_numeric(endpoint(7),broad,&ordinal,&filled)==Status::ok&&filled==400;},11000));
    CHECK(wait_until([&]{return app.pipeline().query_numeric(endpoint(17),note,&ordinal,&filled)==Status::ok&&filled==400;}));
    const auto broad_view=std::get<NumericView>(broad.frame.view().payload),note_view=std::get<NumericView>(note.frame.view().payload);
    CHECK(broad_view.actual[0]==400&&broad_view.actual[1]==16&&broad_view.contract->dtype==DType::UInt8);
    CHECK(note_view.actual[0]==400&&note_view.actual[1]==48&&note_view.contract->dtype==DType::UInt8);
    CHECK(400*(broad_view.actual[1]+note_view.actual[1])==25600);
    CHECK(broad_view.contract->coordinates[1].unit==NumericUnit::Hertz&&note_view.contract->coordinates[1].unit==NumericUnit::Values);
    // Render only owned test data. numeric_raster is the renderer's own image;
    // no desktop capture or native graphics claim is involved.
    for(int id:{7,17}) {
        pipeline::DeliveredNumericFrame history;CHECK(app.pipeline().query_numeric(endpoint(id),history,&ordinal,&filled)==Status::ok);
        presentation::NumericViewAdapter adapter({1,{400,128,1024*1024,{false,400*128}},{false,400*128}});
        CHECK(adapter.bind(endpoint(id))==Status::ok);CHECK(adapter.accept_history_snapshot(history,ordinal,filled)==Status::ok);
        NumericImageView image;image.resize(640,360);
        presentation::NumericImagePort port;port.set_matrix=[&](const auto& rows){image.set_matrix(rows);};
        port.set_axis_metadata=[&](const auto& axes){return image.set_axis_metadata(axes);};
        port.set_snapshot=[&](auto snapshot,bool age){image.set_snapshot(std::move(snapshot),age);};
        presentation::NumericRenderTarget target;target.image=&port;target.visible=true;
        target.axes={true,true};
        CHECK(adapter.render(endpoint(id),0,target)==Status::ok);
        CHECK(image.value_count()==400*size_t(id==7?16:48)&&image.peak_value()>0);
        CHECK(image.source_axis_metadata().value_title_key=="Quantized code (Y)");
        CHECK(image.source_axis_metadata().column_title_key==(id==7?"Frequency, Hz (Z)":"Note salience, MIDI (Z)"));
        CHECK(image.source_axis_metadata().columns.front()==(id==7?30.:48.));
        CHECK(image.source_axis_metadata().columns.back()==(id==7?14250.:95.));
        CHECK(image.source_axis_metadata().ages.front()==0);
        const auto raster=image.numeric_raster();CHECK(!raster.isNull());
        if(!image_dir.isEmpty()){CHECK(QDir().mkpath(image_dir));CHECK(raster.save(QDir(image_dir).filePath(id==7?"broadband.png":"note-salience.png")));}
    }
    if(!export_path.isEmpty()) {
        CHECK(!QFile::exists(export_path));app.save_configuration_as(export_path);
        if(!QFile::exists(export_path))std::fprintf(stderr,"Save As failed: %s\n",window->statusBar()->currentMessage().toUtf8().constData());
        CHECK(QFile::exists(export_path));
    }
    // Each control commits through the public editor model and waits for Apply.
    auto edit=[&](int id,const char* field,const NodeParameterModel::Value& value) {
        const auto revision=app.presenter().client().snapshot().graph_revision;
        edit_rejection.clear();auto* node=model->delegateModel<AudioNodeModel>(id);
        const bool committed=node&&node->parameter_model()->edit(field,value);
        const bool ready=committed&&wait_until([&]{pipeline::DeliveredNumericFrame result,changed;return !app.presenter().graph_request_pending()&&app.presenter().client().snapshot().graph_revision>revision&&query(17,result)&&query(id,changed);});
        if(!ready) {
            pipeline::DeliveredNumericFrame changed;const bool delivered=query(id,changed);
            std::fprintf(stderr,"Edit failure node=%d field=%s committed=%d before_rev=%llu after_rev=%llu pending=%d current_tap=%d rejection=%s statusbar=%s\n",id,field,committed,
                static_cast<unsigned long long>(revision),static_cast<unsigned long long>(app.presenter().client().snapshot().graph_revision),
                app.presenter().graph_request_pending(),delivered,edit_rejection.toUtf8().constData(),window->statusBar()->currentMessage().toUtf8().constData());
            std::fprintf(stderr,"Edit diagnostics=%s\n",QJsonDocument(app.pipeline().snapshot().diagnostics).toJson(QJsonDocument::Compact).constData());
        }
        return ready;
    };
    for(int mode:{0,1,2,3,4,1}) {
        CHECK(edit(201,"mode",mode));CHECK(query(201,pcm));
        CHECK(std::get<NumericView>(pcm.frame.view().payload).contract->clock.sample_rate==32768);
    }
    CHECK(edit(201,"output_rate",NumericLiteral{std::uint64_t{32000}}));CHECK(query(201,pcm));
    CHECK(std::get<NumericView>(pcm.frame.view().payload).contract->clock.sample_rate==32000);
    CHECK(edit(201,"output_rate",NumericLiteral{std::uint64_t{32768}}));
    for(int function:{1,2,3,4,5,6,0})CHECK(edit(3,"function",function));
    CHECK(edit(4,"fft_size",0));CHECK(query(4,fft));CHECK(std::get<NumericView>(fft.frame.view().payload).actual[0]==257);
    CHECK(edit(4,"fft_size",3));CHECK(edit(4,"radix",1));CHECK(edit(4,"radix",0));
    for(int representation:{0,1,3,4,5,6,7,8,9,2})CHECK(edit(30,"representation",representation));
    CHECK(edit(33,"bits",1));CHECK(query(7,broad));CHECK(std::get<NumericView>(broad.frame.view().payload).contract->dtype==DType::UInt16);
    CHECK(edit(33,"bits",0));
    CHECK(edit(31,"boundaries_hz",QString{"0,60,120,300,650,1250,3500,7000,16000"}));
    CHECK(query(7,broad));CHECK(std::get<NumericView>(broad.frame.view().payload).actual[1]==8);
    CHECK(edit(32,"notes",NumericLiteral{std::int64_t{24}}));CHECK(query(17,note));CHECK(std::get<NumericView>(note.frame.view().payload).actual[1]==24);
    auto* notes=model->delegateModel<AudioNodeModel>(32);CHECK(notes&&notes->parameter_model()->field("noteGridReadout"));
    CHECK(std::get<QString>(notes->parameter_model()->field("noteGridReadout")->read()).contains("C3"));
    CHECK(edit(6,"manual_gain",NumericLiteral{2.}));CHECK(edit(6,"mode",1));
    // A real canonical profile document from the offline export adapter.
    desktop_analysis::offline::ProfileDocument profile;
    profile.profile.compatibility.resampler_mode=dsp_kernels::ResamplerMode::CompactFir;
    profile.profile.gains.assign(2049,1.f);profile.profile.mean_spectrum.assign(2049,.1);
    profile.preserved["future_padding"]=QString(96*1024,QLatin1Char('p'));
    const auto profile_json=desktop_analysis::offline::encode_profile(profile);CHECK(profile_json.size()>64*1024&&profile_json.size()<=1024*1024);
    std::printf("Canonical test profile JSON bytes=%lld\n",static_cast<long long>(profile_json.size()));std::fflush(stdout);
    const auto profile_path=directory.filePath("frequency-profile.json");
    {QFile file(profile_path);CHECK(file.open(QIODevice::WriteOnly)&&file.write(profile_json)==profile_json.size());}
    CHECK(edit(6,"loadFrequencyProfile",profile_path));
    CHECK(std::get<int>(model->delegateModel<AudioNodeModel>(6)->parameter_model()->field("mode")->read())==2);
    for(int stream:{1,0}) {
        auto* selector=model->delegateModel<AudioNodeModel>(200);CHECK(selector);
        const auto revision=app.presenter().client().snapshot().graph_revision;CHECK(selector->select_input(domain::PortId(stream)));
        CHECK(wait_until([&]{return !app.presenter().graph_request_pending()&&app.presenter().client().snapshot().graph_revision>revision&&query(17,note);}));
    }
    CHECK(model->edit_execution_interval({1,64}));
    CHECK(wait_until([&]{return !app.presenter().graph_request_pending()&&query(17,note);}));
    CHECK(read(input_path)==original);app.save_configuration_as(saved_path);CHECK(QFile::exists(saved_path));
    desktop::NumericProjectDocument saved;domain::NumericProcessingGraph reloaded;CHECK(desktop::load_numeric_project(saved_path,saved,reloaded));
    CHECK(reloaded.execution_interval==(Ratio{1,64}));CHECK(reloaded.nodes.size()==model->numeric_graph().nodes.size());
    bool profile_roundtrip=false;
    for(const auto& node:reloaded.nodes)if(node.id==6)
        for(const auto& p:std::get<domain::TransformProcessingParameters>(node.parameters).values)
            if(p.key=="profile_json")profile_roundtrip=std::get<std::string>(p.value)==profile_json.toStdString();
    CHECK(profile_roundtrip);
    desktop::NumericDraftPresentation reloaded_display;CHECK(desktop::read_numeric_presentation(saved,reloaded_display));
    auto* editor=window->findChild<SchemeEditorWidget*>();CHECK(editor);QString reload_error;
    CHECK(editor->load_numeric_draft(reloaded,&reload_error,reloaded_display.node_adapters));
    const auto reload_revision=app.presenter().client().snapshot().graph_revision;CHECK(app.presenter().apply_pipeline_graph({}));
    CHECK(wait_until([&]{return !app.presenter().graph_request_pending()&&app.presenter().client().snapshot().graph_revision>reload_revision&&query(17,note);}));
    CHECK(read(input_path)==original);
    // Benchmark normal completion and destruction while its worker is running.
    auto* tools=window->findChild<QMenu*>("analysisToolsMenu");CHECK(tools);
    CHECK(!menu->findChild<QAction*>("resamplerComparison") && !menu->findChild<QAction*>("musicFileAnalysis"));
    auto* comparison=tools->findChild<QAction*>("resamplerComparison");CHECK(comparison);
    comparison->trigger();auto* dialog=window->findChild<QDialog*>("resamplerComparisonDialog");CHECK(dialog);
    auto* run=dialog->findChild<QPushButton*>("runResamplerComparison");CHECK(run);run->click();
    auto* table=dialog->findChild<QTableWidget*>("resamplerComparisonResults");CHECK(table);
    CHECK(wait_until([&]{return run->isEnabled()&&table->item(5,1)!=nullptr;}));
    for(int row=0;row<6;++row) {
        CHECK(table->item(row,1)&&table->item(row,1)->text().toUInt()>0);
        std::printf("Benchmark mode %d Fs=%s prepare_us=%s p50_us=%s p95_us=%s owned_bytes=%s delay_input_samples=%s\n",row,
            table->item(row,1)->text().toUtf8().constData(),table->item(row,2)->text().toUtf8().constData(),
            table->item(row,3)->text().toUtf8().constData(),table->item(row,4)->text().toUtf8().constData(),
            table->item(row,5)->text().toUtf8().constData(),table->item(row,6)->text().toUtf8().constData());
    }
    delete dialog;
    comparison->trigger();dialog=window->findChild<QDialog*>("resamplerComparisonDialog");CHECK(dialog);
    run=dialog->findChild<QPushButton*>("runResamplerComparison");CHECK(run);run->click();delete dialog;QCoreApplication::processEvents();
    auto* analyze=tools->findChild<QAction*>("musicFileAnalysis");CHECK(analyze);analyze->trigger();
    auto* analysis=window->findChild<QDialog*>("musicAnalysisDialog");CHECK(analysis);
    auto* files=analysis->findChild<QListWidget*>("musicFiles");auto* analyze_run=analysis->findChild<QPushButton*>("musicRun");
    auto* analysis_status=analysis->findChild<QLabel*>("musicStatus");CHECK(files&&analyze_run&&analysis_status);
    const auto wav=directory.filePath("eight-changing-tones.wav");CHECK(write_tones(wav));files->addItem(wav);analyze_run->click();
    CHECK(wait_until([&]{return analyze_run->isEnabled()&&analysis_status->text().startsWith(ui::text("Analysis complete: %1 s audio / %2 s processing (%3×)").section("%1",0,0));}));
    bool statistics=false;for(auto* result:analysis->findChildren<QTableWidget*>())statistics|=result->rowCount()==1;CHECK(statistics);
    delete analysis;
    std::printf("Music Qt preset: native %u; menu/layout, full 400x(16+48) UInt8 History 25600-byte payload, renderer rasters, six resamplers, 32000/32768 Fs, four windows, 512/4096 RFFT, radix2/4, UInt16, 8 broad/24 note cells, manual/canonical profile, synthetic source switching, 60/64 Hz, save/reload, benchmark lifecycle PASS (offscreen only)\n",native_rate);
    return 0;
}
