#include <core_app/graph_panel.h>
#include <visualization/abstract/abs.h>
#include <visualization/numeric_image_view.h>
#include <visualization/numeric_pie_view.h>
#include <visualization/numeric_metrics_view.h>
#include <visualization/spectrograms/spectrogram_graph.h>
#include <visualization/surface_settings.h>
#include <visualization/quick/quick_ui.h>
#include <visualization/ui_text.h>
#include <QStackedWidget>
#include <QSplitter>
#include <QVBoxLayout>
#include <QTimer>
#include <QSignalBlocker>
#include <QMap>
#include <cmath>
#include <limits>
GraphPanel::GraphPanel(const QString& id,AbstractChartView* chart,QJsonObject prefs,QWidget* parent,SpectrogramGraph* surface)
    :QWidget(parent),_chart(chart),_surface(surface),_id(id),_preferences(std::move(prefs)) {
    const auto kind=_preferences.value("kind").toString();
    if(kind=="ScalarView")_allowed_modes={"metrics"};
    else if(kind=="SpectrogramView" || id=="spectrogram" || surface)_allowed_modes={"image2d","surface3d"};
    else _allowed_modes={"line","points","bars","pie"};
    setObjectName(id+"GraphPanel");
    auto* outer=new QVBoxLayout(this);outer->setContentsMargins(0,0,0,0);
    _splitter=new QSplitter(this);_splitter->setChildrenCollapsible(false);outer->addWidget(_splitter);
    _renderers=new QStackedWidget(_splitter);_renderers->addWidget(chart);
    _image=new NumericImageView(_renderers);_renderers->addWidget(_image);
    _pie=new NumericPieView(_renderers);_renderers->addWidget(_pie);
    if(surface)_renderers->addWidget(surface);
    if(_allowed_modes.contains("metrics")){_metrics=new NumericMetricsView(_renderers);_renderers->addWidget(_metrics);}
    ui::quick::configure();
    _settings=new QQuickWidget(_splitter);_settings->setObjectName("numericGraphSettings");_settings->setMinimumWidth(160);
    _splitter->setStretchFactor(0,1);_splitter->setStretchFactor(1,0);
    initialize_ports();
    connect(_image,&NumericImageView::axis_metadata_changed,this,&GraphPanel::refresh_axis_display_controls);
    connect(_splitter,&QSplitter::splitterMoved,this,[this]{
        const auto sizes=_splitter->sizes();const int total=sizes[0]+sizes[1];
        if(total && !_restoring){_preferences["side_ratio"]=double(sizes[1])/total;emit presentation_changed(_id,{{"side_ratio",_preferences["side_ratio"]}});}
    });
    connect(&ui::Language::instance(),&ui::Language::changed,this,&GraphPanel::refresh_captions);
    _default_display_mode=_allowed_modes.contains(_chart->plot_style())?_chart->plot_style():_allowed_modes.front();
    restore_presentation(_preferences);
    _default_display_mode=_display_mode;
    ui::quick::initialize(_settings,this,QUrl("qrc:/common/GraphPanel.qml"));
    _information_timer=new QTimer(this);_information_timer->setInterval(500);
    connect(_information_timer,&QTimer::timeout,this,[this]{
        if(_display_mode=="surface3d" || _display_mode=="metrics"){if(!_information.isEmpty()){_information.clear();emit state_changed();}return;}
        const size_t count=active_chart()?_chart->value_count():pie_view()?_pie->value_count():image_view()?_image->value_count():0;
        const float peak=active_chart()?_chart->peak_value():pie_view()?_pie->peak_value():image_view()?_image->peak_value():0;
        _information=ui::text("Values: %1 · Peak: %2").arg(qulonglong(count)).arg(peak,0,'g',4);emit state_changed();
    });
    set_render_visible(true);
}
GraphPanel::~GraphPanel(){
    _restoring=true;
    if(_information_timer)_information_timer->stop();
    // The QML root borrows this presentation model. Destroy the consumer while
    // the model, its properties and its renderer ports are still alive.
    delete _settings;_settings=nullptr;
}
QString GraphPanel::caption(QObject* target,const char* source,QStringList args,bool loss,bool unavailable){
    target->setProperty("numericCaptionSource",QString::fromUtf8(source));target->setProperty("numericCaptionArguments",args);
    target->setProperty("numericCaptionLoss",loss);target->setProperty("numericCaptionUnavailable",unavailable);
    auto text=*source?ui::text(source):QString{};for(const auto& arg:args)text=text.arg(arg);
    if(loss)text+=(text.isEmpty()?QString{}:QStringLiteral(" · "))+ui::text("Renderer precision loss");
    if(unavailable)text+=(text.isEmpty()?QString{}:QStringLiteral(" · "))+ui::text("Renderer projection unavailable");
    return text;
}
void GraphPanel::refresh_captions(){
    const auto refresh=[this](QObject* object){return caption(object,object->property("numericCaptionSource").toString().toUtf8().constData(),object->property("numericCaptionArguments").toStringList(),object->property("numericCaptionLoss").toBool(),object->property("numericCaptionUnavailable").toBool());};
    _diagnostic=refresh(this);_chart->set_projection_status(refresh(_chart));_image->set_projection_status(refresh(_image));_pie->set_projection_status(refresh(_pie));
    if(_surface)_surface->set_projection_status(refresh(_surface));
    _chart->set_axis_titles(ui::text(_x_title_key.toUtf8().constData()),ui::text(_y_title_key.toUtf8().constData()));emit state_changed();
}
void GraphPanel::initialize_ports(){
    _caption_port.caption=[this](const char* s,QStringList a,bool l,bool u){_diagnostic=caption(this,s,std::move(a),l,u);emit state_changed();};
    _chart_port.caption=[this](const char* s,QStringList a,bool l,bool u){_chart->set_projection_status(caption(_chart,s,std::move(a),l,u));};
    _chart_port.update=[this](const auto& v){_chart->update(v);};
    _chart_port.set_scalar_readout=[this](const QString& s,bool scalar,bool available){_chart->set_scalar_readout(s,scalar,available);};
    _chart_port.scalar_readout_enabled=[this]{return _chart->scalar_readout_enabled();};
    _chart_port.set_x_coordinates=[this](const auto& v){return _chart->set_x_coordinates(v);};
    _chart_port.set_axis_title_keys=[this](const QString& x,const QString& y){_x_title_key=x;_y_title_key=y;_chart->set_axis_titles(ui::text(x.toUtf8().constData()),ui::text(y.toUtf8().constData()));};
    _surface_port.caption=[this](const char* s,QStringList a,bool l,bool u){if(_surface)_surface->set_projection_status(caption(_surface,s,std::move(a),l,u));};
    _surface_port.paused=[this]{return _surface && _surface->paused();};
    _surface_port.update_frame_history=[this](const auto& v){if(_surface)_surface->update_frame_history(v);};
    _surface_port.set_history_dimensions=[this](size_t a,size_t b){if(_surface)_surface->set_history_dimensions(a,b);};
    _surface_port.set_axis_metadata=[this](const auto& v){return _surface && _surface->set_axis_metadata(v);};
    _surface_port.update_snapshot=[this](auto v){if(_surface)_surface->update_snapshot(std::move(v));};
    _image_port.caption=[this](const char* s,QStringList a,bool l,bool u){_image->set_projection_status(caption(_image,s,std::move(a),l,u));};
    _image_port.set_matrix=[this](const auto& v){_image->set_matrix(v);};
    _image_port.set_axis_metadata=[this](const auto& v){return _image->set_axis_metadata(v);};
    _image_port.set_snapshot=[this](auto v,bool temporal){_image->set_snapshot(std::move(v),temporal);};
    _pie_port.caption=[this](const char* s,QStringList a,bool l,bool u){_pie->set_projection_status(caption(_pie,s,std::move(a),l,u));};
    _pie_port.set_values=[this](const auto& v){return _pie->set_values(v);};
    _metrics_port.clear_values=[this]{if(_metrics)_metrics->clear_values();};
    _metrics_port.set_unavailable=[this](quint16 p){if(_metrics)_metrics->set_unavailable(p);};
    _metrics_port.set_value=[this](quint16 p,const QString& v){if(_metrics)_metrics->set_value(p,v);};
}
presentation::NumericRenderTarget GraphPanel::render_target(bool visible){return {active_chart()?&_chart_port:nullptr,active_spectrogram()?&_surface_port:nullptr,&_caption_port,visible,image_view()?&_image_port:nullptr,pie_view()?&_pie_port:nullptr,_metrics?&_metrics_port:nullptr};}
presentation::NumericRenderTarget GraphPanel::all_render_targets(){return {&_chart_port,_surface?&_surface_port:nullptr,&_caption_port,true,&_image_port,&_pie_port,_metrics?&_metrics_port:nullptr};}
void GraphPanel::apply_preferences(){
    const bool spectrum=_id=="fft" || _id=="mel" || _preferences.value("kind").toString()=="FFTView" || _preferences.value("kind").toString()=="MelView";
    const bool automatic=_preferences.value("auto_range").toBool(_id!="windowFunction");
    const double low=_preferences.value("minimum").toDouble(spectrum?0:-1),high=_preferences.value("maximum").toDouble(1);
    const double interval=_preferences.value("auto_range_interval_seconds").toDouble(10);
    const bool wasAutomatic=_chart->automatic_y_range();
    _chart->set_auto_resizing(automatic);_chart->set_auto_gain(automatic,.001f,spectrum?0:-.001f);_chart->set_auto_range_interval(interval);
    if(!automatic || !wasAutomatic)_chart->set_range(automatic?(spectrum?0:-.001):low,automatic?.001:high);
    _chart->set_x_range(_preferences.value("auto_range_x").toBool(true),_preferences.value("minimum_x").toDouble(0),_preferences.value("maximum_x").toDouble(1));
    const bool reverseX=_preferences.value("reverse_x").toBool(),reverseY=_preferences.value("reverse_y").toBool();
    _chart->set_reverse_axes(reverseX,reverseY);_image->set_y_range(automatic,low,high,interval);_image->set_reverse_axes(reverseX,reverseY);
    const auto frequency=_preferences.value("frequency_axis_mode").toString("coordinates"),time=_preferences.value("time_axis_mode").toString("coordinates");
    _image->set_axis_display_modes(frequency,time);if(_surface)_surface->set_axis_display_modes(frequency,time);
}
void GraphPanel::restore_presentation(const QJsonObject& prefs){
    _restoring=true;_preferences=prefs;
    const bool spectrum=_id=="fft" || _id=="mel" || prefs.value("kind").toString()=="FFTView" || prefs.value("kind").toString()=="MelView";
    const QJsonObject defaults{{"auto_range",_id!="windowFunction"},{"minimum",spectrum?0:-1},{"maximum",1},{"auto_range_x",true},{"minimum_x",0},{"maximum_x",1},{"reverse_x",false},{"reverse_y",false},{"auto_range_interval_seconds",10},{"frequency_axis_mode","coordinates"},{"time_axis_mode","coordinates"}};
    for(auto it=defaults.begin();it!=defaults.end();++it){
        const auto supplied=_preferences.value(it.key());const auto fallback=it.value();
        const bool valid=fallback.isDouble()?(supplied.isDouble() && std::isfinite(supplied.toDouble())):
            fallback.isBool()?supplied.isBool():supplied.isString();
        if(!valid)_preferences[it.key()]=fallback;
    }
    _preferences["auto_range_interval_seconds"]=AmplitudeRange::interval(_preferences.value("auto_range_interval_seconds").toDouble());
    for(const auto& key:{QStringLiteral("frequency_axis_mode"),QStringLiteral("time_axis_mode")})
        if(_preferences.value(key).toString()!="positions")_preferences[key]="coordinates";
    for(const auto& pair:{qMakePair(QString("minimum"),QString("maximum")),qMakePair(QString("minimum_x"),QString("maximum_x"))}){
        const double low=_preferences.value(pair.first).toDouble(),high=_preferences.value(pair.second).toDouble();
        if(!std::isfinite(low) || !std::isfinite(high) || low>=high){_preferences[pair.first]=defaults.value(pair.first);_preferences[pair.second]=defaults.value(pair.second);}
    }
    apply_preferences();
    set_display_mode(prefs.value("display_mode").toString(_default_display_mode));
    restore_surface_preferences();
    const double ratio=prefs.value("side_ratio").toDouble(.25);if(std::isfinite(ratio) && ratio>=.1 && ratio<=.6)_splitter->setSizes({int(800*(1-ratio)),int(800*ratio)});
    _restoring=false;emit state_changed();
}
void GraphPanel::changePreference(const QString& key,const QVariant& value){
    static const QStringList keys{"minimum","maximum","minimum_x","maximum_x","auto_range","auto_range_x","reverse_x","reverse_y","auto_range_interval_seconds","frequency_axis_mode","time_axis_mode"};
    if(!keys.contains(key))return;
    const auto json=QJsonValue::fromVariant(value);
    const bool boolean=(key.startsWith("auto_range") && key!="auto_range_interval_seconds") || key.startsWith("reverse_");
    const bool enumeration=key.endsWith("axis_mode");
    if(boolean?!json.isBool():enumeration?!json.isString():!json.isDouble() || !std::isfinite(json.toDouble()))return;
    auto next=_preferences;next[key]=json;
    for(const auto& pair:{qMakePair(QString("minimum"),QString("maximum")),qMakePair(QString("minimum_x"),QString("maximum_x"))}){
        const double a=next.value(pair.first).toDouble(),b=next.value(pair.second).toDouble();if(!std::isfinite(a) || !std::isfinite(b) || a>=b)return;
    }
    if(key.endsWith("axis_mode") && value.toString()!="positions" && value.toString()!="coordinates")return;
    if(key=="auto_range_interval_seconds" && (!std::isfinite(value.toDouble()) || value.toDouble()<1 || value.toDouble()>60))return;
    if(next==_preferences)return;_preferences=next;apply_preferences();emit state_changed();emit presentation_changed(_id,{{key,next.value(key)}});
}
void GraphPanel::restore_surface_preferences(){
    if(!_surface)return;
    if(_preferences.contains("surface_options"))_surface->set_surface_settings(SurfaceSettings::normalized(_preferences.value("surface_options").toObject()));
    if(_preferences.contains("renderer"))_surface->set_presentation_state(_preferences.value("renderer").toObject());
    if(_preferences.contains("camera"))_surface->set_camera_state(_preferences.value("camera").toObject());
    if(_preferences.contains("surface_side_ratio"))_surface->set_side_panel_ratio(float(_preferences.value("surface_side_ratio").toDouble(.25)));
}
void GraphPanel::initialize_surface(){
    if(_surface_integrated)return;_surface_integrated=true;
    const bool wasRestoring=_restoring;_restoring=true;
    if(!_surface){_surface=new SpectrogramGraph(_renderers);_renderers->addWidget(_surface);}
    connect(_surface,&SpectrogramGraph::axis_metadata_changed,this,&GraphPanel::refresh_axis_display_controls);
    if(_preferences.contains("surface_options"))_surface->set_surface_settings(SurfaceSettings::normalized(_preferences.value("surface_options").toObject()));
    _surface->set_render_fps(_render_fps);_surface->initialize({160,120},{4096,4096});
    auto options=_surface->surface_settings();options["axis_precision"]=_axis_precision;_surface->set_surface_settings(options);apply_preferences();
    restore_surface_preferences();
    connect(_surface,&SpectrogramGraph::surface_settings_changed,this,[this](const QJsonObject& options){
        if(_restoring)return;_preferences["surface_options"]=options;emit state_changed();emit presentation_changed(_id,{{"surface_options",options}});
    });
    connect(_surface,&SpectrogramGraph::presentation_changed,this,[this](const QJsonObject& state){
        if(_restoring)return;_preferences["renderer"]=state;emit state_changed();emit presentation_changed(_id,{{"renderer",state}});
    });
    connect(_surface,&SpectrogramGraph::camera_changed,this,[this](const QJsonObject& state){
        if(_restoring)return;_preferences["camera"]=state;emit state_changed();emit presentation_changed(_id,{{"camera",state}});
    });
    connect(_surface,&SpectrogramGraph::side_panel_resized,this,[this](float ratio){
        if(_restoring)return;_preferences["surface_side_ratio"]=double(ratio);emit state_changed();emit presentation_changed(_id,{{"surface_side_ratio",double(ratio)}});
    });
    _restoring=wasRestoring;
}
void GraphPanel::set_display_mode(const QString& requested){
    const QString mode=_allowed_modes.contains(requested)?requested:_allowed_modes.front();const bool changed=_display_mode!=mode;_display_mode=mode;
    if(mode=="surface3d")initialize_surface();
    if(active_chart()){_chart->set_plot_style(mode);_renderers->setCurrentWidget(_chart);}
    else _renderers->setCurrentWidget(mode=="image2d"?static_cast<QWidget*>(_image):mode=="pie"?static_cast<QWidget*>(_pie):mode=="metrics"?static_cast<QWidget*>(_metrics):static_cast<QWidget*>(_surface));
    _preferences["display_mode"]=mode;set_render_visible(_render_visible);emit state_changed();
    if(changed){if(!_restoring)emit presentation_changed(_id,{{"display_mode",mode}});emit renderer_changed();}
}
void GraphPanel::set_numeric_plane_count(size_t count,size_t selected){
    if(count>size_t(std::numeric_limits<int>::max()) || selected>=count){count=0;selected=0;}
    if(_plane_count==count && _selected_plane==selected)return;_plane_count=count;_selected_plane=selected;emit state_changed();
}
void GraphPanel::selectPlane(int plane){if(plane<0 || size_t(plane)>=_plane_count || size_t(plane)==_selected_plane)return;_selected_plane=size_t(plane);emit state_changed();emit channel_requested(plane);}
void GraphPanel::set_render_visible(bool visible){
    _render_visible=visible;_chart->set_render_visible(visible && active_chart());_image->set_render_visible(visible && image_view());_pie->set_render_visible(visible && pie_view());
    if(_metrics)_metrics->set_render_visible(visible && _display_mode=="metrics");
    if(_surface)_surface->set_render_visible(visible && active_spectrogram());if(_information_timer){if(visible)_information_timer->start();else _information_timer->stop();}
}
void GraphPanel::set_render_fps(int fps){_render_fps=std::clamp(fps,1,240);_chart->set_render_fps(_render_fps);if(_surface)_surface->set_render_fps(_render_fps);emit state_changed();}
void GraphPanel::set_axis_precision(int precision){_axis_precision=std::clamp(precision,0,8);_chart->set_axis_precision(_axis_precision);_image->set_axis_precision(_axis_precision);if(_surface){auto options=_surface->surface_settings();options["axis_precision"]=_axis_precision;const QSignalBlocker block(_surface);_surface->set_surface_settings(options);}emit state_changed();}
AbstractChartView* GraphPanel::active_chart() const{return _display_mode=="line" || _display_mode=="points" || _display_mode=="bars"?_chart:nullptr;}
SpectrogramGraph* GraphPanel::active_spectrogram() const{return _display_mode=="surface3d"?_surface:nullptr;}
NumericImageView* GraphPanel::image_view() const{return _display_mode=="image2d"?_image:nullptr;}
NumericPieView* GraphPanel::pie_view() const{return _display_mode=="pie"?_pie:nullptr;}
bool GraphPanel::physical_frequency() const{const auto& a=active_spectrogram()?_surface->source_axis_metadata():_image->source_axis_metadata();return a.column_coordinates && a.column_title_key=="Frequency, Hz (Z)";}
bool GraphPanel::physical_time() const{const auto& a=active_spectrogram()?_surface->source_axis_metadata():_image->source_axis_metadata();return a.time_coordinates && (a.time_title_key=="Time delay, seconds (X)" || a.time_title_key=="Time, seconds (X)");}
QString GraphPanel::modeLabel(const QString& mode) const{static const QMap<QString,const char*> labels{{"line","Line"},{"points","Points"},{"bars","Bars"},{"pie","Pie"},{"image2d","Image 2D"},{"surface3d","Surface 3D"},{"metrics","Value graph"}};return ui::text(labels.value(mode,"Line"));}
