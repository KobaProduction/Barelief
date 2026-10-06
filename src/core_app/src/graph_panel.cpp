#include "core_app/graph_panel.h"
#include "visualization/ui_text.h"
#include "visualization/axis_control_widget.h"
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

GraphPanel::GraphPanel(const QString &id, AbstractChartView *chart, QJsonObject preferences, QWidget *parent)
    : QWidget(parent) {
    setObjectName(id + "GraphPanel");
    auto update_theme = [chart] {
        chart->chart()->setTheme(QApplication::palette().color(QPalette::Window).lightness() < 128
                                     ? QChart::ChartThemeDark
                                     : QChart::ChartThemeLight);
    };
    update_theme();
    connect(&ui::Theme::instance(), &ui::Theme::changed, this, update_theme);
    auto *outer = new QVBoxLayout(this);
    outer->setContentsMargins(0, 0, 0, 0);
    auto *splitter = new QSplitter(this);
    splitter->setChildrenCollapsible(false);
    outer->addWidget(splitter);
    splitter->addWidget(chart);
    auto *scroll = new QScrollArea(splitter);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setMinimumWidth(140);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    auto *settings = new QWidget(scroll);
    auto *form = new QFormLayout(settings);
    form->setRowWrapPolicy(QFormLayout::WrapAllRows);
    form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    scroll->setWidget(settings);
    splitter->addWidget(scroll);
    splitter->setStretchFactor(0, 1);
    splitter->setStretchFactor(1, 0);
    splitter->setSizes({650, 210});
    auto *axisXGroup = ui::make<QGroupBox>("Axis range X", settings);
    auto *axisYGroup = ui::make<QGroupBox>("Axis range Y", settings);
    auto *axisXLayout = new QVBoxLayout(axisXGroup);
    auto *axisYLayout = new QVBoxLayout(axisYGroup);
    form->addRow(axisXGroup);form->addRow(axisYGroup);
    const bool fft = id == "fft" || preferences.value("kind").toString() == "FFTView";
    const bool mel = id == "mel" || preferences.value("kind").toString() == "MelView";
    const bool spectrum = fft || mel;
    auto *xAxis = new AxisControlWidget(fft ? "Frequency, Hz (X)" : mel ? "Frequency, Mel bands (X)" : "Samples (X)", axisXGroup);
    auto *yAxis = new AxisControlWidget("Amplitude (Y)", axisYGroup);
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
    auto applyAxes = [this, id, chart, xAxis, yAxis, spectrum] {
        const double low = yAxis->minimum_box()->value(), high = yAxis->maximum_box()->value();
        const bool automatic = yAxis->auto_box()->isChecked();
        chart->set_auto_resizing(automatic);
        chart->set_auto_gain(automatic, .001f, spectrum ? 0 : -.001f);
        chart->set_range(automatic ? (spectrum ? 0 : -.001) : low, automatic ? .001 : high);
        chart->set_x_range(xAxis->auto_box()->isChecked(), xAxis->minimum_box()->value(), xAxis->maximum_box()->value());
        for (auto *axis : chart->chart()->axes())
            axis->setReverse(axis->orientation() == Qt::Horizontal ? xAxis->reverse_box()->isChecked() : yAxis->reverse_box()->isChecked());
        emit presentation_changed(id, {{"auto_range", automatic}, {"minimum", low}, {"maximum", high},
            {"auto_range_x", xAxis->auto_box()->isChecked()}, {"minimum_x", xAxis->minimum_box()->value()},
            {"maximum_x", xAxis->maximum_box()->value()}, {"reverse_x", xAxis->reverse_box()->isChecked()},
            {"reverse_y", yAxis->reverse_box()->isChecked()}});
    };
    applyAxes();
    connect(xAxis, &AxisControlWidget::range_changed, this, applyAxes);
    connect(xAxis, &AxisControlWidget::reverse_changed, this, applyAxes);
    connect(yAxis, &AxisControlWidget::range_changed, this, applyAxes);
    connect(yAxis, &AxisControlWidget::reverse_changed, this, applyAxes);
    const auto ratio = preferences.value("side_ratio").toDouble(0.25);
    if (std::isfinite(ratio) && ratio >= 0.1 && ratio <= 0.6)
        splitter->setSizes({int(800 * (1 - ratio)), int(800 * ratio)});
    connect(splitter, &QSplitter::splitterMoved, this, [this, id, splitter] {
        const auto sizes = splitter->sizes();
        const int total = sizes[0] + sizes[1];
        if (total > 0)
            emit presentation_changed(id, {{"side_ratio", double(sizes[1]) / total}});
    });
    if (fft) {
        _channel = new QComboBox(settings);
        _channel->setObjectName("fftPanelChannel");
        ui::items(_channel, {"Left", "Right", "Merged"});
        form->addRow(ui::make<QLabel>("FFT graph channel", settings), _channel);
        connect(_channel, &QComboBox::currentIndexChanged, this, &GraphPanel::channel_requested);
    }
    if (spectrum) {
        _merge = new QComboBox(settings);
        _merge->setObjectName(id + "PanelMerge");
        ui::items(_merge, {"Mean", "Maximum", "Energy (RMS)"});
        form->addRow(ui::make<QLabel>("Spectrum merge", settings), _merge);
        connect(_merge, &QComboBox::currentIndexChanged, this, &GraphPanel::merge_requested);
    }
    auto *units = ui::make<QLabel>(fft   ? "X: frequency, Hz · Y: magnitude"
                                   : mel ? "X: Mel band · Y: magnitude"
                                         : "X: sample · Y: amplitude",
                                   settings);
    units->setWordWrap(true);
    form->addRow(units);
    auto *info = new QLabel(settings);
    info->setObjectName(id + "GraphInfo");
    info->setWordWrap(true);
    form->addRow(info);
    _information_timer = new QTimer(this);
    _information_timer->setInterval(500);
    connect(_information_timer, &QTimer::timeout, this, [chart, info] {
        if (chart->isVisible())
            info->setText(
                ui::text("Values: %1 · Peak: %2").arg(chart->value_count()).arg(chart->peak_value(), 0, 'g', 4));
    });
}
void GraphPanel::set_render_visible(bool visible) {
    if (visible)
        _information_timer->start();
    else
        _information_timer->stop();
}
void GraphPanel::set_processing_choices(int channel, int merge) {
    if (_channel) {
        const QSignalBlocker blocker(_channel);
        _channel->setCurrentIndex(channel);
    }
    if (_merge) {
        const QSignalBlocker blocker(_merge);
        _merge->setCurrentIndex(merge);
    }
}
