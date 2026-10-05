#include "core_app/graph_panel.h"
#include "visualization/ui_text.h"
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

GraphPanel::GraphPanel(const QString &id, AbstractChartView *chart, QJsonObject preferences, QWidget *parent)
    : QWidget(parent) {
    setObjectName(id+"GraphPanel");
    auto update_theme=[chart] { chart->chart()->setTheme(QApplication::palette().color(QPalette::Window).lightness()<128 ? QChart::ChartThemeDark : QChart::ChartThemeLight); };
    update_theme(); connect(&ui::Theme::instance(),&ui::Theme::changed,this,update_theme);
    auto *outer=new QVBoxLayout(this); outer->setContentsMargins(0,0,0,0);
    auto *splitter=new QSplitter(this); splitter->setChildrenCollapsible(false);
    outer->addWidget(splitter); splitter->addWidget(chart);
    auto *scroll=new QScrollArea(splitter); scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame); scroll->setMinimumWidth(180);
    auto *settings=new QWidget(scroll); auto *form=new QFormLayout(settings);
    form->setRowWrapPolicy(QFormLayout::WrapAllRows);
    form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    scroll->setWidget(settings); splitter->addWidget(scroll);
    splitter->setStretchFactor(0,1); splitter->setStretchFactor(1,0); splitter->setSizes({650,210});
    auto *title=ui::make<QLabel>("Display",settings);
    title->setStyleSheet("font-weight: 600; padding: 4px 0;"); form->addRow(title);
    auto *automatic=ui::make<QCheckBox>("Automatic Y range",settings);
    automatic->setObjectName(id+"AutoRange");
    automatic->setChecked(preferences.value("auto_range").toBool(id!="windowFunction"));
    form->addRow(automatic);
    auto *lower=new QDoubleSpinBox(settings), *upper=new QDoubleSpinBox(settings);
    for (auto *spin : {lower,upper}) { spin->setRange(-1000000,1000000); spin->setDecimals(4); spin->setSingleStep(0.1); }
    lower->setObjectName(id+"Minimum"); upper->setObjectName(id+"Maximum");
    const bool spectrum=id=="fft" || id=="mel";
    lower->setValue(preferences.value("minimum").toDouble(spectrum ? 0 : -1));
    upper->setValue(preferences.value("maximum").toDouble(1));
    if (lower->value()>=upper->value()) { lower->setValue(spectrum ? 0 : -1); upper->setValue(1); }
    form->addRow(ui::make<QLabel>("Minimum",settings),lower);
    form->addRow(ui::make<QLabel>("Maximum",settings),upper);
    auto *range_error=ui::make<QLabel>("Graph range invalid",settings);
    range_error->setWordWrap(true); range_error->setVisible(false); form->addRow(range_error);
    auto apply=[this,id,chart,automatic,lower,upper,range_error] {
        const bool valid=lower->value()<upper->value();
        range_error->setVisible(!valid);
        lower->setEnabled(!automatic->isChecked()); upper->setEnabled(!automatic->isChecked());
        if (!valid) return;
        chart->set_auto_resizing(automatic->isChecked());
        const bool positive=id=="fft" || id=="mel";
        chart->set_auto_gain(automatic->isChecked(),0.001f,positive ? 0 : -0.001f);
        chart->set_range(automatic->isChecked() ? (positive ? 0 : -0.001) : lower->value(),
                         automatic->isChecked() ? 0.001 : upper->value());
        emit presentation_changed(id,{{"auto_range",automatic->isChecked()},{"minimum",lower->value()},{"maximum",upper->value()}});
    };
    apply(); // Apply restored view preferences before subscribing to changes.
    connect(automatic,&QCheckBox::toggled,this,apply);
    connect(lower,&QDoubleSpinBox::valueChanged,this,apply);
    connect(upper,&QDoubleSpinBox::valueChanged,this,apply);
    const auto ratio=preferences.value("side_ratio").toDouble(0.25);
    if (std::isfinite(ratio) && ratio>=0.1 && ratio<=0.6) splitter->setSizes({int(800*(1-ratio)),int(800*ratio)});
    connect(splitter,&QSplitter::splitterMoved,this,[this,id,splitter] {
        const auto sizes=splitter->sizes(); const int total=sizes[0]+sizes[1];
        if (total>0) emit presentation_changed(id,{{"side_ratio",double(sizes[1])/total}});
    });
    if (id=="fft") {
        _channel=new QComboBox(settings); _channel->setObjectName("fftPanelChannel");
        ui::items(_channel,{"Left","Right","Merged"});
        form->addRow(ui::make<QLabel>("FFT graph channel",settings),_channel);
        connect(_channel,&QComboBox::currentIndexChanged,this,&GraphPanel::channel_requested);
    }
    if (spectrum) {
        _merge=new QComboBox(settings); _merge->setObjectName(id+"PanelMerge");
        ui::items(_merge,{"Mean","Maximum","Energy (RMS)"});
        form->addRow(ui::make<QLabel>("Spectrum merge",settings),_merge);
        connect(_merge,&QComboBox::currentIndexChanged,this,&GraphPanel::merge_requested);
    }
    auto *units=ui::make<QLabel>(id=="fft" ? "X: frequency, Hz · Y: magnitude" : id=="mel" ? "X: Mel band · Y: magnitude" : "X: sample · Y: amplitude",settings);
    units->setWordWrap(true); form->addRow(units);
    auto *info=new QLabel(settings); info->setObjectName(id+"GraphInfo"); info->setWordWrap(true); form->addRow(info);
    _information_timer=new QTimer(this); _information_timer->setInterval(500);
    connect(_information_timer,&QTimer::timeout,this,[chart,info] {
        if (chart->isVisible()) info->setText(ui::text("Values: %1 · Peak: %2").arg(chart->value_count()).arg(chart->peak_value(),0,'g',4));
    });
}
void GraphPanel::set_render_visible(bool visible) {
    if (visible) _information_timer->start(); else _information_timer->stop();
}
void GraphPanel::set_processing_choices(int channel,int merge) {
    if (_channel) { const QSignalBlocker blocker(_channel); _channel->setCurrentIndex(channel); }
    if (_merge) { const QSignalBlocker blocker(_merge); _merge->setCurrentIndex(merge); }
}
