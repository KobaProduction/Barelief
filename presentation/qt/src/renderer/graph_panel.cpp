#include "core_app/graph_panel.h"
#include "visualization/ui_text.h"
#include "visualization/axis_control_widget.h"
#include "visualization/auto_range_interval_control.h"
#include "visualization/numeric_image_view.h"
#include "visualization/numeric_pie_view.h"
#include "visualization/numeric_metrics_view.h"
#include "visualization/surface_settings.h"
#include <QSplitter>
#include <QFormLayout>
#include <QVBoxLayout>
#include <QCheckBox>
#include <QDoubleSpinBox>
#include <QPushButton>
#include <QLabel>
#include <QScrollArea>
#include <QSignalBlocker>
#include <cmath>
#include <QApplication>
#include <QGroupBox>
#include <QSlider>
#include <QStackedWidget>
#include <QStandardItemModel>
#include <limits>

GraphPanel::GraphPanel(const QString &id, AbstractChartView *chart, QJsonObject preferences, QWidget *parent,SpectrogramGraph* supplied_surface)
    : QWidget(parent) {
    _id=id;_chart=chart;_preferences=preferences;
    const auto kind=preferences.value("kind").toString();
    if(kind=="ScalarView")_allowed_modes={"metrics"};
    else if(kind=="SpectrogramView" || id=="spectrogram" || supplied_surface)_allowed_modes={"image2d","surface3d"};
    else _allowed_modes={"line","points","bars","pie"};
    setObjectName(id + "GraphPanel");
    chart->refresh_theme();
    auto *outer = new QVBoxLayout(this);
    outer->setContentsMargins(0, 0, 0, 0);
    auto *splitter = new QSplitter(this);
    splitter->setChildrenCollapsible(false);
    outer->addWidget(splitter, 1);
    _renderers=new QStackedWidget(splitter);_renderers->addWidget(chart);
    _image=new NumericImageView(_renderers);_renderers->addWidget(_image);
    connect(_image,&NumericImageView::axis_metadata_changed,this,&GraphPanel::refresh_axis_display_controls);
    _pie=new NumericPieView(_renderers);_renderers->addWidget(_pie);
    if(supplied_surface){_surface=supplied_surface;_renderers->addWidget(_surface);}
    if(_allowed_modes.contains("metrics")){_metrics=new NumericMetricsView(_renderers);_renderers->addWidget(_metrics);}
    splitter->addWidget(_renderers);
    auto *scroll = new QScrollArea(splitter);
    scroll->setObjectName("numericGraphSettings");
    _settings=scroll;
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setMinimumWidth(140);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    auto *settings = new QWidget(scroll);
    auto *form = new QFormLayout(settings);
    form->setRowWrapPolicy(QFormLayout::WrapAllRows);
    form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    scroll->setWidget(settings);
    _mode=new QComboBox(settings);_mode->setObjectName("numericDisplayMode");
    const auto populateModes=[this](QComboBox* combo) {
        static const QMap<QString,QString> labels{{"line","Line"},{"points","Points"},{"bars","Bars"},{"image2d","Image 2D"},{"surface3d","Surface 3D"},{"pie","Pie"},{"metrics","Value graph"}};
        for(const auto& mode:_allowed_modes)combo->addItem(ui::text(labels.value(mode).toUtf8().constData()),mode);
        connect(&ui::Language::instance(),&ui::Language::changed,combo,[combo]{
            for(int i=0;i<combo->count();++i)combo->setItemText(i,ui::text(labels.value(combo->itemData(i).toString()).toUtf8().constData()));
        });
    };
    populateModes(_mode);_mode->setEnabled(_allowed_modes.size()>1);
    form->addRow(ui::make<QLabel>("Display mode",settings),_mode);
    if(_allowed_modes.contains("image2d")) {
        apply_axis_display_modes(preferences);
        add_axis_display_controls(form,settings);
    }
    splitter->addWidget(scroll);
    splitter->setStretchFactor(0, 1);
    splitter->setStretchFactor(1, 0);
    splitter->setSizes({650, 210});
    auto *axisXGroup = ui::make<QGroupBox>("Axis range X", settings);
    auto *axisYGroup = ui::make<QGroupBox>("Axis range Y", settings);
    auto *axisXLayout = new QVBoxLayout(axisXGroup);
    auto *axisYLayout = new QVBoxLayout(axisYGroup);
    form->addRow(axisXGroup);form->addRow(axisYGroup);
    if(_metrics){axisXGroup->hide();axisYGroup->hide();}
    const bool fft = id == "fft" || preferences.value("kind").toString() == "FFTView";
    const bool mel = id == "mel" || preferences.value("kind").toString() == "MelView";
    const bool spectrum = fft || mel;
    auto *xAxis = new AxisControlWidget("Axis range X", axisXGroup);
    auto *yAxis = new AxisControlWidget("Axis range Y", axisYGroup);
    xAxis->setObjectName(id + "AxisX"); yAxis->setObjectName(id + "AxisY");
    axisXLayout->addWidget(xAxis); axisYLayout->addWidget(yAxis);
    xAxis->minimum_box()->setObjectName(id + "MinimumX"); xAxis->maximum_box()->setObjectName(id + "MaximumX");
    xAxis->auto_box()->setObjectName(id + "AutoRangeX"); xAxis->reverse_box()->setObjectName(id + "ReverseX");
    yAxis->minimum_box()->setObjectName(id + "Minimum"); yAxis->maximum_box()->setObjectName(id + "Maximum");
    yAxis->auto_box()->setObjectName(id + "AutoRange"); yAxis->reverse_box()->setObjectName(id + "ReverseY");
    xAxis->set_state(preferences.value("minimum_x").toDouble(0), preferences.value("maximum_x").toDouble(1),
                      preferences.value("auto_range_x").toBool(true), preferences.value("reverse_x").toBool());
    const double yMin = preferences.value("minimum").toDouble(spectrum ? 0 : -1);
    const double yMax = preferences.value("maximum").toDouble(1);
    yAxis->set_state(yMin < yMax ? yMin : (spectrum ? 0 : -1), yMin < yMax ? yMax : 1,
                    preferences.value("auto_range").toBool(id != "windowFunction"), preferences.value("reverse_y").toBool());
    auto* interval=new ui::AutoRangeIntervalControl(axisYGroup);
    interval->box->setObjectName(id+"AutoRangeIntervalSeconds");
    interval->set_seconds(preferences.value("auto_range_interval_seconds").toDouble(10));
    axisYLayout->addWidget(interval);
    auto applyAxes = [this, id, chart, xAxis, yAxis, spectrum, interval] {
        const double low = yAxis->minimum_box()->value(), high = yAxis->maximum_box()->value();
        const bool automatic = yAxis->auto_box()->isChecked();
        const bool wasAutomatic=chart->automatic_y_range();
        chart->set_auto_resizing(automatic);
        chart->set_auto_gain(automatic, .001f, spectrum ? 0 : -.001f);
        chart->set_auto_range_interval(interval->seconds());
        _image->set_y_range(automatic,low,high,interval->seconds());
        _image->set_reverse_axes(xAxis->reverse_box()->isChecked(),yAxis->reverse_box()->isChecked());
        interval->setEnabled(automatic);
        if(!automatic || !wasAutomatic)chart->set_range(automatic ? (spectrum ? 0 : -.001) : low, automatic ? .001 : high);
        chart->set_x_range(xAxis->auto_box()->isChecked(), xAxis->minimum_box()->value(), xAxis->maximum_box()->value());
        for (auto *axis : chart->chart()->axes())
            axis->setReverse(axis->orientation() == Qt::Horizontal ? xAxis->reverse_box()->isChecked() : yAxis->reverse_box()->isChecked());
        emit presentation_changed(id, {{"auto_range", automatic}, {"minimum", low}, {"maximum", high},
            {"auto_range_x", xAxis->auto_box()->isChecked()}, {"minimum_x", xAxis->minimum_box()->value()},
            {"maximum_x", xAxis->maximum_box()->value()}, {"reverse_x", xAxis->reverse_box()->isChecked()},
            {"reverse_y", yAxis->reverse_box()->isChecked()}, {"auto_range_interval_seconds",interval->seconds()}});
    };
    applyAxes();
    const auto initialMode=preferences.value("display_mode").toString(chart->plot_style());
    set_display_mode(initialMode);
    _default_display_mode=_display_mode;
    _restore_presentation=[this,id,xAxis,yAxis,splitter,spectrum,interval,applyAxes](const QJsonObject& prefs) {
        const QSignalBlocker panelBlock(this),xBlock(xAxis),yBlock(yAxis);
        xAxis->set_state(prefs.value("minimum_x").toDouble(0),prefs.value("maximum_x").toDouble(1),
            prefs.value("auto_range_x").toBool(true),prefs.value("reverse_x").toBool());
        const auto low=prefs.value("minimum").toDouble(spectrum?0:-1),high=prefs.value("maximum").toDouble(1);
        yAxis->set_state(low<high?low:(spectrum?0:-1),low<high?high:1,
            prefs.value("auto_range").toBool(id!="windowFunction"),prefs.value("reverse_y").toBool());
        interval->set_seconds(prefs.value("auto_range_interval_seconds").toDouble(10));
        const auto ratio=prefs.value("side_ratio").toDouble(.25);
        if(std::isfinite(ratio) && ratio>=.1 && ratio<=.6)splitter->setSizes({int(800*(1-ratio)),int(800*ratio)});
        applyAxes();
        _preferences=prefs;
        apply_axis_display_modes(prefs);
        if(_surface && prefs.contains("surface_options"))_surface->set_surface_settings(SurfaceSettings::normalized(prefs.value("surface_options").toObject()));
        set_display_mode(prefs.value("display_mode").toString(_default_display_mode));
    };
    connect(xAxis, &AxisControlWidget::range_changed, this, applyAxes);
    connect(xAxis, &AxisControlWidget::reverse_changed, this, applyAxes);
    connect(yAxis, &AxisControlWidget::range_changed, this, applyAxes);
    connect(yAxis, &AxisControlWidget::reverse_changed, this, applyAxes);
    connect(interval->box,&QDoubleSpinBox::valueChanged,this,applyAxes);
    connect(_mode,&QComboBox::currentIndexChanged,this,[this](int index){
        if(index>=0)set_display_mode(_mode->itemData(index).toString());
    });
    const auto ratio = preferences.value("side_ratio").toDouble(0.25);
    if (std::isfinite(ratio) && ratio >= 0.1 && ratio <= 0.6)
        splitter->setSizes({int(800 * (1 - ratio)), int(800 * ratio)});
    connect(splitter, &QSplitter::splitterMoved, this, [this, id, splitter] {
        const auto sizes = splitter->sizes();
        const int total = sizes[0] + sizes[1];
        if (total > 0)
            emit presentation_changed(id, {{"side_ratio", double(sizes[1]) / total}});
    });
    _channel = new QComboBox(settings);
    _channel->setObjectName(fft ? "fftPanelChannel" : id + "PanelChannel");
    _channel->setVisible(false);
    form->addRow(ui::make<QLabel>("Channels", settings), _channel);
    connect(_channel, &QComboBox::currentIndexChanged, this, [this](int index){
        if(index<0)return;_selected_plane=size_t(index);
        if(_surface_channel){const QSignalBlocker block(_surface_channel);_surface_channel->setCurrentIndex(index);}
        emit channel_requested(index);
    });
    connect(&ui::Language::instance(),&ui::Language::changed,this,[this] {
        for(auto* channel:{_channel,_surface_channel})if(channel) {
            const QSignalBlocker blocker(channel);
            for(int i=0;i<channel->count();++i)channel->setItemText(i,ui::text("Channel %1").arg(i));
        }
    });
    auto *info = new QLabel(settings);
    info->setObjectName(id + "GraphInfo");
    info->setWordWrap(true);
    form->addRow(info);
    // Diagnostics belong to the settings sidebar, never a second footer under
    // the whole dock. Scalar values already have a readout inside the chart.
    auto *exact = new QLabel(settings);
    exact->setObjectName("numericExactValue");
    exact->setTextFormat(Qt::PlainText);
    exact->setWordWrap(true);
    exact->setTextInteractionFlags(Qt::TextSelectableByMouse);
    form->addRow(exact);
    if(_metrics){info->hide();exact->hide();}
    _information_timer = new QTimer(this);
    _information_timer->setInterval(500);
    connect(_information_timer, &QTimer::timeout, this, [this,chart, info] {
        if (chart->isVisible())
            info->setText(
                ui::text("Values: %1 · Peak: %2").arg(chart->value_count()).arg(chart->peak_value(), 0, 'g', 4));
        else if(_pie->isVisible())info->setText(ui::text("Values: %1 · Peak: %2").arg(_pie->value_count()).arg(_pie->peak_value(),0,'g',4));
        else if(_image->isVisible())info->setText(ui::text("Values: %1 · Peak: %2").arg(_image->value_count()).arg(_image->peak_value(),0,'g',4));
    });
}
void GraphPanel::restore_presentation(const QJsonObject& preferences) {
    if(_restore_presentation)_restore_presentation(preferences);
}
void GraphPanel::refresh_axis_display_controls() {
    if(!_allowed_modes.contains("image2d"))return;
    const auto& axes=active_spectrogram()?_surface->source_axis_metadata():_image->source_axis_metadata();
    const bool hertz=axes.column_coordinates && axes.column_title_key=="Frequency, Hz (Z)";
    const bool seconds=axes.time_coordinates && (axes.time_title_key=="Time delay, seconds (X)" || axes.time_title_key=="Time, seconds (X)");
    for(const auto& entry:{qMakePair(QStringLiteral("frequencyAxisMode"),hertz),qMakePair(QStringLiteral("timeAxisMode"),seconds)})
        for(auto* combo:findChildren<QComboBox*>(entry.first)) {
            const QSignalBlocker block(combo);
            combo->setItemText(0,ui::text(entry.first=="frequencyAxisMode"?
                (entry.second?"Hertz (source coordinates)":"Hertz unavailable: source has no coordinates"):
                (entry.second?"Delay in seconds (source coordinates)":"Seconds unavailable: source has no timestamps")));
            if(auto* model=qobject_cast<QStandardItemModel*>(combo->model()))if(auto* item=model->item(0))item->setEnabled(entry.second);
        }
}
void GraphPanel::apply_axis_display_modes(const QJsonObject& preferences) {
    _frequency_axis_mode=preferences.value("frequency_axis_mode").toString()=="positions"?"positions":"coordinates";
    _time_axis_mode=preferences.value("time_axis_mode").toString()=="positions"?"positions":"coordinates";
    _image->set_axis_display_modes(_frequency_axis_mode,_time_axis_mode);
    if(_surface)_surface->set_axis_display_modes(_frequency_axis_mode,_time_axis_mode);
    for(const auto& entry:{qMakePair(QStringLiteral("frequencyAxisMode"),_frequency_axis_mode),qMakePair(QStringLiteral("timeAxisMode"),_time_axis_mode)})
        for(auto* combo:findChildren<QComboBox*>(entry.first)) {
            const QSignalBlocker block(combo);combo->setCurrentIndex(combo->findData(entry.second));
        }
}
void GraphPanel::add_axis_display_controls(QFormLayout* form,QWidget* parent) {
    const auto add=[&](const char* name,const char* title,const char* physical,const char* indexed,const char* key,const QString& current) {
        auto* combo=new QComboBox(parent);combo->setObjectName(name);
        ui::items(combo,{physical,indexed});combo->setItemData(0,"coordinates");combo->setItemData(1,"positions");
        combo->setCurrentIndex(combo->findData(current));
        ui::bind(combo,"toolTip","Physical coordinates require source metadata; otherwise positions are shown.");
        form->addRow(ui::make<QLabel>(title,parent),combo);
        connect(combo,&QComboBox::currentIndexChanged,this,[this,combo,key](int index) {
            if(index<0)return;
            _preferences[key]=combo->itemData(index).toString();apply_axis_display_modes(_preferences);
            emit presentation_changed(_id,{{key,_preferences[key]}});
        });
    };
    add("frequencyAxisMode","Frequency axis","Hertz (source coordinates)","Positions","frequency_axis_mode",_frequency_axis_mode);
    add("timeAxisMode","Time axis","Delay in seconds (source coordinates)","Temporal frames","time_axis_mode",_time_axis_mode);
    connect(&ui::Language::instance(),&ui::Language::changed,parent,[this]{refresh_axis_display_controls();});
    refresh_axis_display_controls();
}
void GraphPanel::set_render_visible(bool visible) {
    _render_visible=visible;
    _chart->set_render_visible(visible && active_chart());
    _image->set_render_visible(visible && image_view());
    _pie->set_render_visible(visible && pie_view());
    if(_surface)_surface->set_render_visible(visible && active_spectrogram());
    if (!_information_timer)return;
    if (visible)
        _information_timer->start();
    else
        _information_timer->stop();
}
AbstractChartView* GraphPanel::active_chart() const {return _display_mode=="line" || _display_mode=="points" || _display_mode=="bars"?_chart:nullptr;}
SpectrogramGraph* GraphPanel::active_spectrogram() const {return _display_mode=="surface3d"?_surface:nullptr;}
NumericImageView* GraphPanel::image_view() const {return _display_mode=="image2d"?_image:nullptr;}
NumericPieView* GraphPanel::pie_view() const {return _display_mode=="pie"?_pie:nullptr;}
void GraphPanel::set_render_fps(int fps) {
    _render_fps=std::clamp(fps,1,240);_chart->set_render_fps(_render_fps);if(_surface)_surface->set_render_fps(_render_fps);
}
void GraphPanel::set_axis_precision(int precision) {
    _axis_precision=std::clamp(precision,0,8);
    _chart->set_axis_precision(_axis_precision);_image->set_axis_precision(_axis_precision);
    if(_surface){auto options=_surface->surface_settings();options["axis_precision"]=_axis_precision;_surface->set_surface_settings(options);}
}
void GraphPanel::set_display_mode(const QString& requested) {
    const QString mode=_allowed_modes.contains(requested)?requested:_allowed_modes.front();
    const bool changed=_display_mode!=mode;
    _display_mode=mode;
    for(auto* combo:findChildren<QComboBox*>("numericDisplayMode")) {
        const QSignalBlocker block(combo);combo->setCurrentIndex(combo->findData(mode));
    }
    if(mode=="surface3d" && !_surface_integrated) {
        _surface_integrated=true;
        if(!_surface){_surface=new SpectrogramGraph(_renderers);_renderers->addWidget(_surface);}
        connect(_surface,&SpectrogramGraph::axis_metadata_changed,this,&GraphPanel::refresh_axis_display_controls);
        _surface->set_axis_display_modes(_frequency_axis_mode,_time_axis_mode);
        _surface->set_surface_settings(SurfaceSettings::normalized(_preferences.value("surface_options").toObject()));
        _surface->set_render_fps(_render_fps);
        auto preciseOptions=_surface->surface_settings();preciseOptions["axis_precision"]=_axis_precision;_surface->set_surface_settings(preciseOptions);
        const bool native=_surface->initialize({160,120},{4096,4096});
        if(native)if(auto* sidebar=_surface->findChild<QScrollArea*>()) {
            auto* controls=new QWidget(sidebar->widget());auto* controlForm=new QFormLayout(controls);
            controlForm->setContentsMargins(0,0,0,0);controlForm->setRowWrapPolicy(QFormLayout::WrapAllRows);
            auto* combo=new QComboBox(controls);combo->setObjectName("numericDisplayMode");
            ui::items(combo,{"Image 2D","Surface 3D"});combo->setItemData(0,"image2d");combo->setItemData(1,"surface3d");combo->setCurrentIndex(1);
            controlForm->addRow(ui::make<QLabel>("Display mode",controls),combo);
            add_axis_display_controls(controlForm,controls);
            _surface_channel=new QComboBox(controls);_surface_channel->setObjectName("surfacePanelChannel");
            controlForm->addRow(ui::make<QLabel>("Channels",controls),_surface_channel);
            connect(_surface_channel,&QComboBox::currentIndexChanged,this,[this](int index){if(_channel && index>=0)_channel->setCurrentIndex(index);});
            set_numeric_plane_count(_plane_count,_selected_plane);
            auto* rename=ui::make<QPushButton>("Rename graph",controls);rename->setObjectName("surfaceGraphRename");controlForm->addRow(rename);
            connect(rename,&QPushButton::clicked,this,[this]{if(auto* outer=_settings->findChild<QPushButton*>("graphRename"))outer->click();});
            if(auto* form=qobject_cast<QFormLayout*>(sidebar->widget()->layout()))form->insertRow(0,controls);
            else if(auto* box=qobject_cast<QBoxLayout*>(sidebar->widget()->layout()))box->insertWidget(0,controls);
            connect(combo,&QComboBox::currentIndexChanged,this,[this,combo](int index){if(index>=0)set_display_mode(combo->itemData(index).toString());});
            connect(this,&GraphPanel::renderer_changed,combo,[this,combo]{const QSignalBlocker block(combo);combo->setCurrentIndex(combo->findData(_display_mode));});
        }
        connect(_surface,&SpectrogramGraph::surface_settings_changed,this,[this](const QJsonObject& options){
            _preferences["surface_options"]=options;emit presentation_changed(_id,{{"surface_options",options}});
        });
    }
    if(active_chart()){_chart->set_plot_style(mode);_renderers->setCurrentWidget(_chart);}
    else _renderers->setCurrentWidget(mode=="image2d"?static_cast<QWidget*>(_image):mode=="pie"?static_cast<QWidget*>(_pie):mode=="metrics"?static_cast<QWidget*>(_metrics):static_cast<QWidget*>(_surface));
    // Native surface owns a complete sidebar. Offscreen keeps the common one,
    // including the mode choice needed to leave the native-session placeholder.
    _settings->setVisible(mode!="surface3d" || !_surface->findChild<QScrollArea*>());
    set_render_visible(_render_visible);
    refresh_axis_display_controls();
    if(changed){_preferences["display_mode"]=mode;emit presentation_changed(_id,{{"display_mode",mode}});emit renderer_changed();}
}
void GraphPanel::set_numeric_plane_count(size_t count,size_t selected) {
    _plane_count=count;_selected_plane=selected;
    for(auto* channel:{_channel,_surface_channel})if(channel) {
        const QSignalBlocker blocker(channel);
        if(count>size_t(std::numeric_limits<int>::max()) || selected>=count) {channel->clear();channel->hide();continue;}
        if(size_t(channel->count())!=count) {
            channel->clear();for(size_t i=0;i<count;++i)channel->addItem(ui::text("Channel %1").arg(qulonglong(i)));
        }
        channel->setCurrentIndex(int(selected));channel->setVisible(count>1);
    }
}
