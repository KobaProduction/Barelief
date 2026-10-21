#include "core_app/scheme_editor/audio_node_model.h"
#include "core_app/scheme_editor/editor_geometry.h"
#include "core_app/scheme_editor/numeric_editor_model.h"
#include "core_app/scheme_editor/numeric_node_operations.h"
#include "barelief/ui_text.h"
#include <QtNodes/DataFlowGraphicsScene>
#include <QAbstractItemView>
#include <QApplication>
#include <QComboBox>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QGraphicsView>
#include <QHBoxLayout>
#include <QImageWriter>
#include <QJsonArray>
#include <QJsonDocument>
#include <QScrollArea>
#include <QSplitter>
#include <QTableWidget>
#include <QTimer>
#include <array>
#include <utility>
#include <stdexcept>

// Opt-in evidence collection. It renders only owned widget trees, never the
// desktop. Native platform painting is necessary but the images still require
// human review: geometry/model assertions are not visual acceptance.
static void record_widget(const QString& name,QWidget* widget,QRect region,const auto& bounded,const QDir& directory,QJsonArray& captures,const QString& platform) {
        bounded();widget->ensurePolished();if(widget->layout())widget->layout()->activate();
        if(region.isNull())region=widget->rect();
        QImage pixels(region.size()*widget->devicePixelRatioF(),QImage::Format_ARGB32_Premultiplied);pixels.setDevicePixelRatio(widget->devicePixelRatioF());pixels.fill(Qt::transparent);
        widget->render(&pixels,-region.topLeft(),QRegion(region));
        const auto filename=name+".png";QImageWriter writer(directory.filePath(filename),"PNG");writer.setCompression(1);
        if(!writer.write(pixels))throw std::runtime_error("PNG write failed");
        captures.append(QJsonObject{{"file",filename},{"widget",widget->metaObject()->className()},{"width",region.width()},{"height",region.height()},{"dpr",widget->devicePixelRatioF()},{"platform",platform}});
    }

static void collect_channel_variants(auto& kind,AudioNodeModel*& node,QtNodes::DataFlowGraphicsScene& scene,auto& id,QGraphicsView*& canvas,auto& settle,auto& record,auto& prefix) {

                    const auto original=*node->numeric_node();
                    for(unsigned channels:{1u,2u,4u,8u}){
                        auto variant=original;
                        if(QString(kind)=="AudioSource"){const auto mono=variant.outputs.front();variant.outputs.clear();for(unsigned lane=0;lane<channels;++lane){auto port=mono;port.id=lane;variant.outputs.push_back(port);}}
                        else if(QString(kind)=="InputSelector")numeric_editor::set_selector_channels(variant,channels);
                        else numeric_editor::set_route_channels(variant,channels);
                        node->set_numeric_node(variant);scene.nodeGeometry().recomputeSize(id);canvas->setSceneRect(scene.nodeGeometry().boundingRect(id));canvas->fitInView(canvas->sceneRect(),Qt::KeepAspectRatio);settle();record(prefix+QString("-channels-%1").arg(channels),canvas->viewport());
                    }
                    node->set_numeric_node(original);settle();
                }

static void collect_generator_headers(AudioNodeModel*& node,QWidget*& form,auto& language,auto& theme,auto& settle,auto& checks,auto& record,auto& prefix) {

                    auto* table=form->findChild<QTableWidget*>("oscillatorTable");if(!table)throw std::runtime_error("oscillator table missing");
                    const auto saved=node->save();const auto other=language=="ru"?QString("en"):QString("ru");ui::Language::instance().set(other);settle();
                    bool headers=table->columnCount()==8 && table->horizontalHeaderItem(0)->text()=="ID";const char* labels[]{"Enabled","Frequency, Hz","Amplitude","Phase, radians","Attack, seconds","Release, seconds","Remove oscillator"};for(int column=0;column<7;++column)headers&=table->horizontalHeaderItem(column+1)->text()==ui::text(labels[column]);
                    checks.append(QJsonObject{{"check","live oscillator headers"},{"language",other},{"theme",theme},{"passed",headers && node->save()==saved}});if(!headers || node->save()!=saved)throw std::runtime_error("live headers/DSP isolation");
                    record(prefix+"-live-headers",table);ui::Language::instance().set(language);settle();
                }

static void collect_form_states(auto& kind,auto& language,auto& theme,NumericEditorGraphModel& graph,AudioNodeModel*& node,QWidget*& form,auto& prefix,QScrollArea*& inspector,auto& settle,auto& record,auto& checks,QWidget& host,const auto& id) {
                for(int top=0,page=0;top<form->height();top+=1000,++page)record(prefix+QString("-form-%1").arg(page),form,QRect(0,top,form->width(),std::min(1000,form->height()-top)));
                form->setEnabled(false);settle();record(prefix+"-disabled",form,QRect(0,0,form->width(),std::min(1000,form->height())));form->setEnabled(true);
                // Show the real owned combo popup container as a nonactivating
                // Tool, not Qt::Popup: no implicit OS input/focus grab. This
                // captures its native delegate painting, not popup modality.
                if(auto* combo=form->findChild<QComboBox*>()){
                    auto* view=combo->view();auto* popup=view->window();
                    popup->setWindowFlags(Qt::Tool|Qt::FramelessWindowHint|Qt::WindowDoesNotAcceptFocus);popup->setAttribute(Qt::WA_ShowWithoutActivating);
                    view->setMinimumSize(460,std::min(400,std::max(90,combo->count()*30)));popup->adjustSize();popup->show();settle();record(prefix+"-choices",popup);popup->hide();
                }
                if(QString(kind)=="SignalGenerator"){ collect_generator_headers(node,form,language,theme,settle,checks,record,prefix); }
                checks.append(QJsonObject{{"kind",kind},{"language",language},{"theme",theme},{"summary",node->operation_summary()},{"inputs",int(node->numeric_node()->inputs.size())},{"outputs",int(node->numeric_node()->outputs.size())},{"window_accepts_focus",!host.windowFlags().testFlag(Qt::WindowDoesNotAcceptFocus)}});
                inspector->takeWidget();delete form;graph.deleteNode(id);settle();
            }

static void collect_kind(auto& kind,auto& language,auto& theme,NumericEditorGraphModel& graph,QtNodes::DataFlowGraphicsScene& scene,QGraphicsView*& canvas,QScrollArea*& inspector,auto& bounded,auto& settle,auto& record,auto& checks,QWidget& host) {

                bounded();const auto id=graph.addNode(kind);auto* node=graph.delegateModel<AudioNodeModel>(id);
                auto dto=*node->numeric_node();
                if(QString(kind)=="InputSelector"){numeric_editor::set_selector_channels(dto,2);numeric_editor::set_input_count(dto,3);node->set_numeric_node(dto);node->select_input(1);}
                if(QString(kind)=="Switch"){numeric_editor::set_route_channels(dto,2);numeric_editor::set_output_count(dto,3);std::get<domain::RouteProcessingParameters>(dto.parameters).selected=1;node->set_numeric_node(dto);}
                const auto longName=QString::fromUtf8("Длинное имя блока: независимые каналы и явный маршрут обработки");
                node->set_presentation(QJsonObject{{"display_name",longName},{"stream_names",QJsonArray{"Generator",QString::fromUtf8("Выбранный поток: очень длинное пользовательское название"),"Microphone"}}});
                node->set_capture_device_provider([](int,QString*){return QJsonArray{QJsonObject{{"id","fixture-device"},{"name","Owned fixture device (not hardware)"},{"native_sample_rate",48000},{"native_channels",2}}};});
                auto* form=node->parameter_widget();form->setFixedWidth(590);form->ensurePolished();if(form->layout())form->layout()->activate();form->resize(590,form->sizeHint().height());inspector->setWidget(form);
                scene.nodeGeometry().recomputeSize(id);canvas->setSceneRect(scene.nodeGeometry().boundingRect(id));canvas->fitInView(canvas->sceneRect(),Qt::KeepAspectRatio);settle();
                const auto prefix=language+"-"+theme+"-"+kind;record(prefix+"-card",canvas->viewport());
                if(QString(kind)=="AudioSource" || QString(kind)=="InputSelector" || QString(kind)=="Switch"){ collect_channel_variants(kind,node,scene,id,canvas,settle,record,prefix); }
collect_form_states(kind,language,theme,graph,node,form,prefix,inspector,settle,record,checks,host,id);
}

static int write_probe_report(auto& platform,auto& languages,auto& themes,auto& elapsed,auto& captures,auto& failure,auto& checks,auto& directory) {
    QJsonObject report{{"platform",platform},{"languages",QJsonArray::fromStringList(languages)},{"themes",QJsonArray::fromStringList(themes)},{"elapsed_ms",elapsed.elapsed()},{"capture_count",captures.size()},{"collection_completed",failure.isEmpty()},{"error",failure},{"visual_acceptance","pending image review"},{"scope","owned native Qt Widgets only; no desktop capture, OS input, DSP execution or hardware discovery; popup modality not accepted"},{"captures",captures},{"checks",checks}};
    QFile file(directory.filePath("report.json"));if(!file.open(QIODevice::WriteOnly))return 5;
    const auto bytes=QJsonDocument(report).toJson();if(file.write(bytes)!=bytes.size() || !file.flush())return 5;
    return failure.isEmpty()?0:1;
}

static int collect_widget_matrix(auto& languages,auto& themes,auto& platform,auto& directory,auto& elapsed,auto& captures,auto& checks,auto& failure,QWidget& host,NumericEditorGraphModel& graph,QtNodes::DataFlowGraphicsScene& scene,QGraphicsView*& canvas,QScrollArea*& inspector) {
    QTimer deadline;deadline.setSingleShot(true);QObject::connect(&deadline,&QTimer::timeout,&host,[&]{failure="45 second deadline";host.hide();});deadline.start(45000);
    auto bounded=[&]{if(elapsed.elapsed()>=45000 || !failure.isEmpty())throw std::runtime_error("probe deadline");};
    auto settle=[&]{bounded();QCoreApplication::processEvents(QEventLoop::ExcludeUserInputEvents,5);bounded();};
    auto record=[&](QString name,QWidget* widget,QRect region={}){record_widget(name,widget,region,bounded,directory,captures,platform);};
    const std::array<const char*,30> kinds{{"AudioSource","SignalGenerator","SampleClock","PcmResampler","Window","WindowFunction","StereoFFT","BarkWeighting","Mel","SpectrumGain","Mixer","InputSelector","Switch","Gate","Spectrogram","IndexExtract","ScalarReduce","Converter","AudioView","WindowView","WindowedView","FFTView","MelView","SpectrogramView","ScalarView","NetworkInput","NetworkOutput","WLEDEncode","WLEDOutput","ChannelComparison"}};
    try{
        QStringList expected;for(auto kind:kinds)expected.append(kind);expected.sort();auto actual=numeric_editor::catalog();actual.sort();if(actual!=expected)throw std::runtime_error("catalogue differs from product inventory");
        host.show();settle();
        for(const auto& language:std::as_const(languages))for(const auto& theme:std::as_const(themes)){
            ui::Language::instance().set(language);ui::Theme::instance().set(theme);settle();
            for(const auto* kind:kinds){ collect_kind(kind,language,theme,graph,scene,canvas,inspector,bounded,settle,record,checks,host); }
        }
    }catch(const std::exception& error){if(failure.isEmpty())failure=QString::fromUtf8(error.what());}
    deadline.stop();host.hide();
return write_probe_report(platform,languages,themes,elapsed,captures,failure,checks,directory);
}

int main(int argc,char** argv){
    QApplication app(argc,argv);
    app.setQuitOnLastWindowClosed(false);
    if(argc!=2 && argc!=4)return 2;
    QStringList languages{"ru","en"},themes{"light","dark"};
    if(argc==4){
        const auto language=QString::fromLocal8Bit(argv[2]),theme=QString::fromLocal8Bit(argv[3]);
        if(!languages.contains(language) || !QStringList{"light","dark","system"}.contains(theme))return 2;
        languages={language};themes={theme};
    }
    const auto platform=QGuiApplication::platformName();
    if(platform=="offscreen" || platform=="minimal")return 3;
    QDir directory(QString::fromLocal8Bit(argv[1]));if(!directory.mkpath("."))return 4;
    QElapsedTimer elapsed;elapsed.start();QJsonArray captures,checks;QString failure;
    QWidget host(nullptr,Qt::Window|Qt::WindowDoesNotAcceptFocus);
    host.setAttribute(Qt::WA_ShowWithoutActivating);host.setWindowTitle("Owned block UI evidence probe");host.resize(1480,860);
    QObject::connect(&app,&QGuiApplication::focusWindowChanged,&host,[&](QWindow* window){if(window && failure.isEmpty())failure="owned probe acquired native focus";});
    auto* root=new QHBoxLayout(&host);auto* splitter=new QSplitter(&host);root->addWidget(splitter);
    auto registry=std::make_shared<QtNodes::NodeDelegateModelRegistry>();
    for(const auto catalog=numeric_editor::catalog();const auto& kind:catalog)registry->registerModel([kind]{return std::make_unique<AudioNodeModel>(kind);});
    NumericEditorGraphModel graph(registry);QtNodes::DataFlowGraphicsScene scene(graph);
    scene.setNodeGeometry(std::make_unique<EditorNodeGeometry>(graph,[]{return QJsonObject{};}));
    scene.setNodePainter(std::make_unique<EditorNodePainter>());
    auto* canvas=new QGraphicsView(&scene,splitter);canvas->setMinimumWidth(740);
    auto* inspector=new QScrollArea(splitter);inspector->setWidgetResizable(false);inspector->setMinimumWidth(580);
    splitter->setSizes({820,620});
return collect_widget_matrix(languages,themes,platform,directory,elapsed,captures,checks,failure,host,graph,scene,canvas,inspector);
}
