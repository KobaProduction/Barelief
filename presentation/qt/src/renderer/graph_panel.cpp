#include <core_app/graph_panel.h>
#include <barelief/abstract/abs.h>
#include <barelief/numeric_image_view.h>
#include <barelief/numeric_pie_view.h>
#include <barelief/numeric_piano_view.h>
#include <barelief/numeric_metrics_view.h>
#include <barelief/spectrograms/spectrogram_graph.h>
#include <barelief/surface_settings.h>
#include <barelief/quick/quick_ui.h>
#include <barelief/quick/scene_host.h>
#include <QQmlContext>
#include <barelief/ui_text.h>
#include <QStackedWidget>
#include <QSplitter>
#include <QVBoxLayout>
#include <QTimer>
#include <QSignalBlocker>
#include <QMap>
#include <cmath>
#include <limits>
#include <utility>
namespace {
QJsonObject preference_defaults(bool audio) {
    return {{"grid_lines_x",0},{"grid_lines_y",0},{"axis_labels_x",0},{"axis_labels_y",0},{"value_scale","source"},{"db_reference",1},{"db_factor",0},{"db_floor",-120},{"piano_activity_floor",1e-9},{"line_width",1.25},{"point_size",12},{"bar_width_percent",80},{"pie_labels_visible",true},{"auto_range",true},{"minimum",audio?0:-1},{"maximum",1},{"auto_range_x",true},{"minimum_x",0},{"maximum_x",1},{"reverse_x",false},{"reverse_y",false},{"auto_range_interval_seconds",10},{"frequency_axis_mode","coordinates"},{"time_axis_mode","coordinates"}};
}
bool preference_type_valid(const QJsonValue& supplied,const QJsonValue& fallback) {
    if(fallback.isDouble())return supplied.isDouble() && std::isfinite(supplied.toDouble());
    if(fallback.isBool())return supplied.isBool();
    return supplied.isString();
}
void restore_preference_types(QJsonObject& preferences,const QJsonObject& defaults) {
    for(auto it=defaults.begin();it!=defaults.end();++it)if(!preference_type_valid(preferences.value(it.key()),it.value()))preferences[it.key()]=it.value();
}
void restore_series_preferences(QJsonObject& preferences) {
    for(const auto& key:{QStringLiteral("line_width"),QStringLiteral("point_size"),QStringLiteral("bar_width_percent")}){
        const double maximum=key=="line_width"?20:key=="point_size"?64:100;
        preferences[key]=std::clamp(preferences.value(key).toDouble(),key=="line_width"?.5:1.,maximum);
    }
    if(preferences.value("piano_activity_floor").toDouble()<0 || preferences.value("piano_activity_floor").toDouble()>1e30)preferences["piano_activity_floor"]=1e-9;
    preferences["auto_range_interval_seconds"]=AmplitudeRange::interval(preferences.value("auto_range_interval_seconds").toDouble());
}
bool valid_axis_ranges(const QJsonObject& preferences) {
    for(const auto& pair:{qMakePair(QString("minimum"),QString("maximum")),qMakePair(QString("minimum_x"),QString("maximum_x"))}){
        const double low=preferences.value(pair.first).toDouble(),high=preferences.value(pair.second).toDouble();
        if(!std::isfinite(low) || !std::isfinite(high) || low>=high)return false;
    }
    return true;
}
void restore_axis_ranges(QJsonObject& preferences,const QJsonObject& defaults) {
    for(const auto& pair:{qMakePair(QString("minimum"),QString("maximum")),qMakePair(QString("minimum_x"),QString("maximum_x"))}){
        const double low=preferences.value(pair.first).toDouble(),high=preferences.value(pair.second).toDouble();
        if(!std::isfinite(low) || !std::isfinite(high) || low>=high){preferences[pair.first]=defaults.value(pair.first);preferences[pair.second]=defaults.value(pair.second);}
    }
}
void restore_axis_preferences(QJsonObject& preferences,const QJsonObject& defaults) {
    for(const auto& key:{QStringLiteral("frequency_axis_mode"),QStringLiteral("time_axis_mode")})
        if(preferences.value(key).toString()!="positions")preferences[key]="coordinates";
    restore_axis_ranges(preferences,defaults);
    for(const auto* key:{"grid_lines_x","grid_lines_y","axis_labels_x","axis_labels_y"}){
        const double v=preferences.value(key).toDouble();
        preferences[key]=std::isfinite(v)&&v==std::floor(v)?std::clamp(int(std::clamp(v,-1.,128.)),-1,128):0;
    }
}
void restore_value_scale_preferences(QJsonObject& preferences) {
    if(!QStringList{"source","linear","db"}.contains(preferences.value("value_scale").toString()))preferences["value_scale"]="source";
    if(preferences.value("db_reference").toDouble()<1e-30 || preferences.value("db_reference").toDouble()>1e30)preferences["db_reference"]=1;
    if(preferences.value("db_floor").toDouble()>=0 || preferences.value("db_floor").toDouble()<-300)preferences["db_floor"]=-120;
    if(!QList<int>{0,10,20}.contains(preferences.value("db_factor").toInt()))preferences["db_factor"]=0;
}
bool valid_value_scale_change(const QString& key,const QJsonValue& value) {
    if(key=="value_scale")return QStringList{"source","linear","db"}.contains(value.toString());
    if(key=="db_factor")return value.toDouble()==0 || value.toDouble()==10 || value.toDouble()==20;
    if(key=="db_reference")return value.toDouble()>=1e-30 && value.toDouble()<=1e30;
    if(key=="db_floor")return value.toDouble()<0 && value.toDouble()>=-300;
    return true;
}
bool valid_series_change(const QString& key,const QJsonValue& value) {
    if(key=="piano_activity_floor")return value.toDouble()>=0 && value.toDouble()<=1e30;
    if(key=="auto_range_interval_seconds")return value.toDouble()>=1 && value.toDouble()<=60;
    if(key=="line_width")return value.toDouble()>=.5 && value.toDouble()<=20;
    if(key=="point_size")return value.toDouble()>=1 && value.toDouble()<=64;
    if(key=="bar_width_percent")return value.toDouble()>=1 && value.toDouble()<=100;
    return true;
}
bool valid_axis_change(const QString& key,const QJsonValue& value) {
    if(key.endsWith("axis_mode"))return value.toString()=="positions" || value.toString()=="coordinates";
    if(key.startsWith("grid_lines_") || key.startsWith("axis_labels_"))return value.toDouble()==std::floor(value.toDouble()) && value.toDouble()>=-1 && value.toDouble()<=128;
    return true;
}
bool valid_preference_change(const QString& key,const QJsonValue& value) {
    const auto defaults=preference_defaults(false);
    if(!defaults.contains(key) || !preference_type_valid(value,defaults.value(key)))return false;
    return valid_axis_change(key,value) && valid_series_change(key,value) && valid_value_scale_change(key,value);
}
double default_y_minimum(bool decibels,bool spectrum){
    if(decibels)return -120;
    return spectrum?0:-1;
}
double automatic_y_minimum(bool decibels,bool spectrum){
    if(decibels)return -120;
    return spectrum?0:-.001;
}
void initialize_settings_scene(ui::quick::SceneHost& settings,QObject* model) {
    settings.rootContext()->setContextProperty("graphModel",model);
    settings.setSource(QUrl("qrc:/common/GraphPanel.qml"));
}
}
GraphPanel::GraphPanel(const QString& id,AbstractChartView* chart,QJsonObject prefs,QWidget* parent,SpectrogramGraph* surface,PlotCapabilities capabilities)
    :QWidget(parent),_chart(chart),_surface(surface),_id(id),_preferences(std::move(prefs)) {
    _allowed_modes=std::move(capabilities.modes);_audio_scales=capabilities.audio_scales;
    if(_allowed_modes.isEmpty())_allowed_modes={"line"};
    setObjectName(id+"GraphPanel");
    build_layout();initialize_ports();bind_panel_events();
    _default_display_mode=_allowed_modes.contains(_chart->plot_style())?_chart->plot_style():_allowed_modes.front();
    restore_presentation(_preferences);
    _default_display_mode=_display_mode;
    initialize_settings_scene(*_settings,this);
    start_information_timer();set_render_visible(true);
}
void GraphPanel::build_layout(){
    auto* outer=new QVBoxLayout(this);outer->setContentsMargins(0,0,0,0);
    _splitter=new QSplitter(this);_splitter->setChildrenCollapsible(false);outer->addWidget(_splitter);
    build_renderers();
    ui::quick::configure();
    _settings=new ui::quick::SceneHost(presentation::RenderPreparationPolicy::plot_host,_splitter);
    _settings->setObjectName("numericGraphSettings");_settings->setMinimumWidth(160);
    // Retain the existing texture scene identity for widget-based UI probes.
    if(auto* texture=_settings->findChild<QQuickWidget*>())texture->setObjectName(_settings->objectName());
    _splitter->setStretchFactor(0,1);_splitter->setStretchFactor(1,0);
}
void GraphPanel::build_renderers(){
    _renderers=new QStackedWidget(_splitter);_renderers->addWidget(_chart);
    _image=new NumericImageView(_renderers);_renderers->addWidget(_image);
    _pie=new NumericPieView(_renderers);_renderers->addWidget(_pie);
    if(_surface)_renderers->addWidget(_surface);
    if(_allowed_modes.contains("piano")){_piano=new NumericPianoView(_renderers);_renderers->addWidget(_piano);}
    if(_allowed_modes.contains("metrics") || _allowed_modes.contains("signedbar")){
        _metrics=new NumericMetricsView(_renderers);_renderers->addWidget(_metrics);
    }
}
void GraphPanel::ensure_optional_renderers(){
    if(!_piano && _allowed_modes.contains("piano")){
        _piano=new NumericPianoView(_renderers);_renderers->addWidget(_piano);
    }
    if(!_metrics && (_allowed_modes.contains("metrics") || _allowed_modes.contains("signedbar"))){
        _metrics=new NumericMetricsView(_renderers);_renderers->addWidget(_metrics);
    }
}
void GraphPanel::set_plot_capabilities(PlotCapabilities capabilities){
    if(capabilities.modes.isEmpty())capabilities.modes={"line"};
    if(_allowed_modes==capabilities.modes && _audio_scales==capabilities.audio_scales)return;
    _allowed_modes=std::move(capabilities.modes);_audio_scales=capabilities.audio_scales;
    ensure_optional_renderers();
    _default_display_mode=_allowed_modes.contains(_display_mode)?_display_mode:_allowed_modes.front();
    const bool restoring=_restoring;_restoring=true;
    set_display_mode(_default_display_mode);
    apply_preferences();_restoring=restoring;emit state_changed();
}
void GraphPanel::bind_panel_events(){
    connect(_image,&NumericImageView::axis_metadata_changed,this,&GraphPanel::refresh_axis_display_controls);
    connect(_splitter,&QSplitter::splitterMoved,this,[this]{
        const auto sizes=_splitter->sizes();const int total=sizes[0]+sizes[1];
        if(total && !_restoring){_preferences["side_ratio"]=double(sizes[1])/total;emit presentation_changed(_id,{{"side_ratio",_preferences["side_ratio"]}});}
    });
    connect(&ui::Language::instance(),&ui::Language::changed,this,&GraphPanel::refresh_captions);
}
void GraphPanel::start_information_timer(){
    _information_timer=new QTimer(this);_information_timer->setInterval(500);
    connect(_information_timer,&QTimer::timeout,this,&GraphPanel::refresh_information);
}
GraphPanel::~GraphPanel(){
    _restoring=true;
    if(_information_timer)_information_timer->stop();
    // The QML root borrows this presentation model. Destroy the consumer while
    // the model, its properties and its renderer ports are still alive.
    delete _settings;_settings=nullptr;
}
QObject* GraphPanel::surface_model() const {return _surface;}
void GraphPanel::set_diagnostic(QString text){
    if(_diagnostic==text)return;
    _diagnostic=std::move(text);emit diagnosticChanged();
}
QString GraphPanel::value_layout() const {
    auto text=_value_layout_key.isEmpty()?QString{}:ui::text(_value_layout_key.toUtf8().constData());
    for(const auto& argument:std::as_const(_value_layout_arguments))text=text.arg(argument);return text;
}
void GraphPanel::set_value_layout(const char* key,QStringList arguments){
    const auto source=QString::fromUtf8(key);if(_value_layout_key==source && _value_layout_arguments==arguments)return;
    _value_layout_key=source;_value_layout_arguments=std::move(arguments);emit state_changed();
}
void GraphPanel::refresh_information(){
    QString text;
    if(_display_mode!="surface3d" && _display_mode!="metrics" && _display_mode!="signedbar"){
        const auto [count,peak]=active_value_summary();
        text=ui::text("Values: %1 · Peak: %2").arg(qulonglong(count)).arg(peak,0,'g',4);
    }
    if(_information==text)return;
    _information=std::move(text);emit informationChanged();
}
std::pair<size_t,float> GraphPanel::active_value_summary() const {
    if(active_chart())return {_chart->value_count(),_chart->peak_value()};
    if(piano_view())return {_piano->value_count(),_piano->peak_value()};
    if(pie_view())return {_pie->value_count(),_pie->peak_value()};
    if(image_view())return {_image->value_count(),_image->peak_value()};
    return {};
}
QString GraphPanel::caption(QObject* target,const char* source,QStringList args,bool loss,bool unavailable){
    target->setProperty("numericCaptionSource",QString::fromUtf8(source));target->setProperty("numericCaptionArguments",args);
    target->setProperty("numericCaptionLoss",loss);target->setProperty("numericCaptionUnavailable",unavailable);
    auto text=*source?ui::text(source):QString{};for(const auto& arg:std::as_const(args))text=text.arg(arg);
    if(loss)text+=(text.isEmpty()?QString{}:QStringLiteral(" · "))+ui::text("Renderer precision loss");
    if(unavailable)text+=(text.isEmpty()?QString{}:QStringLiteral(" · "))+ui::text("Renderer projection unavailable");
    return text;
}
void GraphPanel::refresh_captions(){
    const auto refresh=[this](QObject* object){return caption(object,object->property("numericCaptionSource").toString().toUtf8().constData(),object->property("numericCaptionArguments").toStringList(),object->property("numericCaptionLoss").toBool(),object->property("numericCaptionUnavailable").toBool());};
    set_diagnostic(refresh(this));refresh_information();_chart->set_projection_status(refresh(_chart));_image->set_projection_status(refresh(_image));_pie->set_projection_status(refresh(_pie));
    if(_surface)_surface->set_projection_status(refresh(_surface));if(_piano)_piano->set_projection_status(refresh(_piano));
    refresh_value_titles();emit state_changed();
}
ValueDisplayScale GraphPanel::current_value_source()const {
    if(!_allowed_modes.contains("image2d"))return _value_source;
    return active_spectrogram()?_surface->source_axis_metadata().value_scale:_image->source_axis_metadata().value_scale;
}
ValueDisplayScale GraphPanel::display_scale(ValueDisplayScale source) const {
    const auto mode=_audio_scales?_preferences.value("value_scale").toString("source"):QStringLiteral("source");
    source.mode=mode=="db"?ValueDisplayScale::Mode::Decibels:mode=="linear"?ValueDisplayScale::Mode::Linear:ValueDisplayScale::Mode::Source;
    source.reference=_preferences.value("db_reference").toDouble(1);
    source.floor=_preferences.value("db_floor").toDouble(-120);
    const int overrideFactor=_preferences.value("db_factor").toInt(0);
    source.override_factor=overrideFactor;
    return source;
}
SpectrogramAxisMetadata GraphPanel::display_metadata(SpectrogramAxisMetadata source) const {
    source.value_scale=display_scale(source.value_scale);
    return source;
}
void GraphPanel::refresh_value_titles(){
    const auto scale=display_scale(_value_source);
    const QString y=scale.decibels()?"Level, dB (Y)":scale.source_decibels?"Linear value (Y)":_y_title_key;
    _chart->set_axis_titles(ui::text(_x_title_key.toUtf8().constData()),ui::text(y.toUtf8().constData()));
}
void GraphPanel::initialize_ports(){
    _caption_port.caption=[this](const char* s,QStringList a,bool l,bool u){set_diagnostic(caption(this,s,std::move(a),l,u));};
    initialize_chart_port();initialize_surface_port();initialize_matrix_port();initialize_auxiliary_ports();initialize_metric_port();
}
void GraphPanel::initialize_chart_port(){
    _chart_port.caption=[this](const char* s,QStringList a,bool l,bool u){_chart->set_projection_status(caption(_chart,s,std::move(a),l,u));};
    _chart_port.update=[this](const auto& v){_chart->update(v);};
    _chart_port.set_scalar_readout=[this](const QString& s,bool scalar,bool available){_chart->set_scalar_readout(s,scalar,available);};
    _chart_port.scalar_readout_enabled=[this]{return _chart->scalar_readout_enabled();};
    _chart_port.set_x_coordinates=[this](const auto& v){return _chart->set_x_coordinates(v);};
    _chart_port.set_value_source=[this](bool db,int factor){set_value_source(db,factor);};
    _chart_port.set_axis_title_keys=[this](const QString& x,const QString& y){set_axis_title_keys(x,y);};
}
void GraphPanel::set_value_source(bool db,int factor){
    _value_source_supplied=true;
    if(_value_source.source_decibels==db && _value_source.factor==factor)return;
    _value_source.source_decibels=db;_value_source.factor=factor;apply_preferences();emit state_changed();
}
void GraphPanel::set_axis_title_keys(const QString& x,const QString& y){
    const bool changed=_y_title_key!=y;_x_title_key=x;_y_title_key=y;
    if(changed){apply_preferences();emit state_changed();}
    refresh_value_titles();
}
void GraphPanel::initialize_surface_port(){
    _surface_port.caption=[this](const char* s,QStringList a,bool l,bool u){if(_surface)_surface->set_projection_status(caption(_surface,s,std::move(a),l,u));};
    _surface_port.paused=[this]{return _surface && _surface->paused();};
    _surface_port.update_frame_history=[this](const auto& v){if(_surface)_surface->update_frame_history(v);};
    _surface_port.set_history_dimensions=[this](size_t a,size_t b){if(_surface)_surface->set_history_dimensions(a,b);};
    _surface_port.set_axis_metadata=[this](const auto& v){return _surface && _surface->set_axis_metadata(display_metadata(v));};
    _surface_port.update_snapshot=[this](auto v){if(_surface)_surface->update_snapshot(std::move(v));};
}
void GraphPanel::initialize_matrix_port(){
    _image_port.caption=[this](const char* s,QStringList a,bool l,bool u){_image->set_projection_status(caption(_image,s,std::move(a),l,u));};
    _image_port.set_matrix=[this](const auto& v){_image->set_matrix(v);};
    _image_port.set_axis_metadata=[this](const auto& v){return _image->set_axis_metadata(display_metadata(v));};
    _image_port.set_snapshot=[this](auto v,bool temporal){_image->set_snapshot(std::move(v),temporal);};
}
void GraphPanel::initialize_auxiliary_ports(){
    _pie_port.caption=[this](const char* s,QStringList a,bool l,bool u){_pie->set_projection_status(caption(_pie,s,std::move(a),l,u));};
    _pie_port.set_values=[this](const auto& v){return _pie->set_values(v);};
    _piano_port.caption=[this](const char* s,QStringList a,bool l,bool u){if(_piano)_piano->set_projection_status(caption(_piano,s,std::move(a),l,u));};
    _piano_port.set_values=[this](const auto& values){return _piano && _piano->set_values(values);};
    _piano_port.set_frequencies=[this](const auto& hz){return _piano && _piano->set_frequencies(hz);};
}
void GraphPanel::initialize_metric_port(){
    _metrics_port.clear_values=[this]{if(_metrics)_metrics->clear_values();};
    _metrics_port.set_unavailable=[this](quint16 p){if(_metrics)_metrics->set_unavailable(p);};
    _metrics_port.set_value=[this](quint16 p,const QString& v){if(_metrics)_metrics->set_value(p,v);};
}
void GraphPanel::set_axis_permissions(presentation::NumericAxisPermissions axes){
    if(_axis_permissions.frequency==axes.frequency && _axis_permissions.time==axes.time)return;
    _axis_permissions=axes;refresh_axis_display_controls();
}
presentation::NumericRenderTarget GraphPanel::render_target(bool visible){
    const bool metrics_active=_display_mode=="metrics"||_display_mode=="signedbar";
    return {active_chart()?&_chart_port:nullptr,active_spectrogram()?&_surface_port:nullptr,&_caption_port,visible,image_view()?&_image_port:nullptr,pie_view()?&_pie_port:nullptr,metrics_active&&_metrics?&_metrics_port:nullptr,_axis_permissions,piano_view()?&_piano_port:nullptr,_frequency_grid,_plot_presentation};
}
presentation::NumericRenderTarget GraphPanel::all_render_targets(){return {&_chart_port,_surface?&_surface_port:nullptr,&_caption_port,true,&_image_port,&_pie_port,_metrics?&_metrics_port:nullptr,_axis_permissions,_piano?&_piano_port:nullptr,_frequency_grid,_plot_presentation};}
void GraphPanel::apply_preferences(){
    const auto scale=display_scale(current_value_source());
    _image->set_axis_metadata(display_metadata(_image->source_axis_metadata()));
    if(_surface)_surface->set_value_scale(display_scale(_surface->source_axis_metadata().value_scale));
    _chart->set_axis_density(_preferences.value("grid_lines_x").toInt(),_preferences.value("grid_lines_y").toInt(),_preferences.value("axis_labels_x").toInt(),_preferences.value("axis_labels_y").toInt());
    _image->set_axis_density(_preferences.value("grid_lines_x").toInt(),_preferences.value("grid_lines_y").toInt(),_preferences.value("axis_labels_x").toInt(),_preferences.value("axis_labels_y").toInt());
    refresh_value_titles();
    _chart->set_series_style(_preferences.value("line_width").toDouble(1.25),_preferences.value("point_size").toDouble(12),_preferences.value("bar_width_percent").toDouble(80));
    if(_piano)_piano->set_activity_floor(_preferences.value("piano_activity_floor").toDouble(1e-9));
    _pie->set_slice_labels_visible(_preferences.value("pie_labels_visible").toBool(true));
    apply_value_range(scale);apply_axis_preferences();
}
void GraphPanel::apply_value_range(ValueDisplayScale scale){
    const bool spectrum=_audio_scales,automatic=_preferences.value("auto_range").toBool(true),decibels=scale.decibels();
    const bool scaleChanged=_chart->value_scale()!=scale;
    // Defaults follow the displayed quantity; explicit manual limits remain intact.
    if(!_minimum_explicit)_preferences["minimum"]=default_y_minimum(decibels,spectrum);
    if(!_maximum_explicit)_preferences["maximum"]=decibels?0.:1.;
    const double low=_preferences.value("minimum").toDouble(),high=_preferences.value("maximum").toDouble();
    _chart->set_bar_baseline_minimum(decibels);
    const double interval=_preferences.value("auto_range_interval_seconds").toDouble(10);
    const bool wasAutomatic=_chart->automatic_y_range();
    _chart->set_auto_resizing(automatic);
    _chart->set_auto_gain(automatic,decibels?0:.001f,automatic_y_minimum(decibels,spectrum));
    _chart->set_auto_range_interval(interval);
    if(!automatic || !wasAutomatic || scaleChanged)
        _chart->set_range(automatic?automatic_y_minimum(decibels,spectrum):low,automatic?(decibels?0:.001):high);
    _chart->set_value_scale(scale);
    _image->set_y_range(automatic,low,high,interval);
}
void GraphPanel::apply_axis_preferences(){
    _chart->set_x_range(_preferences.value("auto_range_x").toBool(true),_preferences.value("minimum_x").toDouble(0),_preferences.value("maximum_x").toDouble(1));
    const bool reverseX=_preferences.value("reverse_x").toBool(),reverseY=_preferences.value("reverse_y").toBool();
    _chart->set_reverse_axes(reverseX,reverseY);_image->set_reverse_axes(reverseX,reverseY);
    const auto frequency=_preferences.value("frequency_axis_mode").toString("coordinates"),time=_preferences.value("time_axis_mode").toString("coordinates");
    _image->set_axis_display_modes(frequency,time);if(_surface)_surface->set_axis_display_modes(frequency,time);
}
void GraphPanel::restore_presentation(const QJsonObject& prefs){
    _restoring=true;_minimum_explicit=prefs.contains("minimum");_maximum_explicit=prefs.contains("maximum");_preferences=prefs;
    const auto defaults=preference_defaults(_audio_scales);
    restore_preference_types(_preferences,defaults);
    restore_series_preferences(_preferences);
    restore_axis_preferences(_preferences,defaults);
    restore_value_scale_preferences(_preferences);
    apply_preferences();
    set_display_mode(prefs.value("display_mode").toString(_default_display_mode));
    restore_surface_preferences();
    const double ratio=prefs.value("side_ratio").toDouble(.25);if(std::isfinite(ratio) && ratio>=.1 && ratio<=.6)_splitter->setSizes({int(800*(1-ratio)),int(800*ratio)});
    _restoring=false;emit state_changed();
}
void GraphPanel::changePreference(const QString& key,const QVariant& value){
    const auto json=QJsonValue::fromVariant(value);
    if(!valid_preference_change(key,json))return;
    auto next=_preferences;next[key]=json;
    const bool scaleSwitch=key=="value_scale" && next!=_preferences;
    if(scaleSwitch)adjust_scale_range(next,value.toString());
    if(!valid_axis_ranges(next))return;
    publish_preference_change(key,next,scaleSwitch);
}
void GraphPanel::adjust_scale_range(QJsonObject& next,const QString& mode) const {
    const auto source=current_value_source();
    const bool db=mode=="db" || (mode=="source" && source.source_decibels);
    next["minimum"]=db?next.value("db_floor").toDouble(-120):0.;next["maximum"]=db?0.:1.;
}
void GraphPanel::publish_preference_change(const QString& key,const QJsonObject& next,bool scaleSwitch){
    if(scaleSwitch){_minimum_explicit=true;_maximum_explicit=true;}
    if(key=="minimum")_minimum_explicit=true;if(key=="maximum")_maximum_explicit=true;
    if(next==_preferences)return;_preferences=next;apply_preferences();
    if(scaleSwitch && _surface && _surface->surface_settings().value("y_range_mode").toString()=="manual")
        _surface->set_y_range(float(next["minimum"].toDouble()),float(next["maximum"].toDouble()));
    emit state_changed();emit presentation_changed(_id,scaleSwitch?QJsonObject{{key,next.value(key)},{"minimum",next["minimum"]},{"maximum",next["maximum"]}}:QJsonObject{{key,next.value(key)}});
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
    prepare_surface_renderer();
    restore_surface_preferences();
    bind_surface_events();_restoring=wasRestoring;
}
void GraphPanel::prepare_surface_renderer(){
    _surface->set_controls_embedded(true);
    connect(_surface,&SpectrogramGraph::axis_metadata_changed,this,&GraphPanel::refresh_axis_display_controls);
    if(_preferences.contains("surface_options"))_surface->set_surface_settings(SurfaceSettings::normalized(_preferences.value("surface_options").toObject()));
    _surface->set_render_fps(_render_fps);_surface->initialize({160,120},{4096,4096});
    auto options=_surface->surface_settings();options["axis_precision"]=_axis_precision;_surface->set_surface_settings(options);apply_preferences();
}
void GraphPanel::bind_surface_events(){
    connect(_surface,&SpectrogramGraph::surface_settings_changed,this,[this](const QJsonObject& options){
        store_surface_preference("surface_options",options);
    });
    connect(_surface,&SpectrogramGraph::presentation_changed,this,[this](const QJsonObject& state){
        store_surface_preference("renderer",state);
    });
    connect(_surface,&SpectrogramGraph::camera_changed,this,[this](const QJsonObject& state){
        // Camera pose is persisted, but no sidebar control displays it. The
        // broad notifier rebuilds QML Repeater models for unrelated controls.
        store_surface_preference("camera",state,false);
    });
    connect(_surface,&SpectrogramGraph::side_panel_resized,this,[this](float ratio){
        store_surface_preference("surface_side_ratio",double(ratio));
    });
}
void GraphPanel::store_surface_preference(const QString& key,const QJsonValue& value,bool notify_state){
    if(_restoring || (!notify_state && _preferences.value(key)==value))return;
    _preferences[key]=value;
    if(notify_state)emit state_changed();
    emit presentation_changed(_id,{{key,value}});
}
void GraphPanel::set_display_mode(const QString& requested){
    const QString mode=_allowed_modes.contains(requested)?requested:_allowed_modes.front();const bool changed=_display_mode!=mode;_display_mode=mode;if(_metrics)_metrics->set_signed_bar(mode=="signedbar");
    if(mode=="surface3d")initialize_surface();
    select_renderer_widget();
    _preferences["display_mode"]=mode;set_render_visible(_render_visible);
    _physical_frequency_available=physical_frequency();_physical_time_available=physical_time();emit state_changed();
    if(changed)notify_renderer_change();
}
void GraphPanel::notify_renderer_change(){
    if(!_restoring)emit presentation_changed(_id,{{"display_mode",_display_mode}});
    emit renderer_changed();
}
QWidget* GraphPanel::selected_renderer_widget(){
    if(active_chart()){_chart->set_plot_style(_display_mode);return _chart;}
    if(piano_view())return _piano;
    if(image_view())return _image;
    if(pie_view())return _pie;
    if(_display_mode=="metrics" || _display_mode=="signedbar")return _metrics;
    return _surface;
}
void GraphPanel::select_renderer_widget(){
    _renderers->setCurrentWidget(selected_renderer_widget());
}
void GraphPanel::set_numeric_plane_count(size_t count,size_t selected){
    if(count>size_t(std::numeric_limits<int>::max()) || selected>=count){count=0;selected=0;}
    if(_plane_count==count && _selected_plane==selected)return;_plane_count=count;_selected_plane=selected;emit state_changed();
}
void GraphPanel::selectPlane(int plane){if(plane<0 || size_t(plane)>=_plane_count || size_t(plane)==_selected_plane)return;_selected_plane=size_t(plane);emit state_changed();emit channel_requested(plane);}
void GraphPanel::set_render_visible(bool visible){
    _render_visible=visible;_chart->set_render_visible(visible && active_chart());_image->set_render_visible(visible && image_view());_pie->set_render_visible(visible && pie_view());
    if(_piano)_piano->set_render_visible(visible && piano_view());
    if(_metrics)_metrics->set_render_visible(visible && (_display_mode=="metrics" || _display_mode=="signedbar"));
    if(_surface)_surface->set_render_visible(visible && active_spectrogram());if(_information_timer){if(visible)_information_timer->start();else _information_timer->stop();}
}
void GraphPanel::set_render_fps(int fps){_render_fps=std::clamp(fps,1,1000);_chart->set_render_fps(_render_fps);if(_piano)_piano->set_render_fps(_render_fps);if(_surface)_surface->set_render_fps(_render_fps);emit state_changed();}
void GraphPanel::set_axis_precision(int precision){_axis_precision=std::clamp(precision,0,8);_chart->set_axis_precision(_axis_precision);_image->set_axis_precision(_axis_precision);if(_surface){auto options=_surface->surface_settings();options["axis_precision"]=_axis_precision;const QSignalBlocker block(_surface);_surface->set_surface_settings(options);}emit state_changed();}
qreal GraphPanel::display_fps() const {
    if(!_render_visible)return 0;
    if(active_spectrogram())return _surface->display_fps();
    if(piano_view())return _piano->display_fps();
    const auto* quick=active_chart()?static_cast<QuickPlotWidget*>(_chart):image_view()?static_cast<QuickPlotWidget*>(_image):pie_view()?static_cast<QuickPlotWidget*>(_pie):static_cast<QuickPlotWidget*>(_metrics);
    return quick?quick->rendered_fps():0;
}
AbstractChartView* GraphPanel::active_chart() const{return _display_mode=="line" || _display_mode=="points" || _display_mode=="bars"?_chart:nullptr;}
SpectrogramGraph* GraphPanel::active_spectrogram() const{return _display_mode=="surface3d"?_surface:nullptr;}
NumericImageView* GraphPanel::image_view() const{return _display_mode=="image2d"?_image:nullptr;}
NumericPieView* GraphPanel::pie_view() const{return _display_mode=="pie"?_pie:nullptr;}
NumericPianoView* GraphPanel::piano_view() const{return _display_mode=="piano"?_piano:nullptr;}
void GraphPanel::set_first_midi(unsigned first){if(_piano)_piano->set_first_midi(first);}
bool GraphPanel::physical_frequency() const{const auto& a=active_spectrogram()?_surface->source_axis_metadata():_image->source_axis_metadata();return _axis_permissions.frequency && a.column_coordinates;}
bool GraphPanel::physical_time() const{const auto& a=active_spectrogram()?_surface->source_axis_metadata():_image->source_axis_metadata();return _axis_permissions.time && a.time_coordinates;}
void GraphPanel::refresh_axis_display_controls(){
    const bool frequency=physical_frequency(),time=physical_time();
    if(frequency==_physical_frequency_available && time==_physical_time_available)return;
    _physical_frequency_available=frequency;_physical_time_available=time;emit state_changed();
}
QString GraphPanel::modeLabel(const QString& mode) const{static const QMap<QString,const char*> labels{{"piano","Piano"},{"line","Line"},{"points","Points"},{"bars","Bars"},{"pie","Pie"},{"image2d","Image 2D"},{"surface3d","Surface 3D"},{"metrics","Value graph"},{"signedbar","Signed bar"}};return ui::text(labels.value(mode,"Line"));}
