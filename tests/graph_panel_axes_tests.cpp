#include "core_app/graph_panel.h"
#include "visualization/charts/freq.h"
#include "visualization/spectrograms/spectrogram_graph.h"
#include "visualization/axis_control_widget.h"
#include <QComboBox>
#include <QApplication>
#include <QCheckBox>
#include <QDoubleSpinBox>
#include <QSlider>
#include <QLabel>
#include <QScrollArea>
#include <QVBoxLayout>
#include <cstdio>

int main(int argc, char **argv) {
    QApplication app(argc, argv);
    auto *chart = new FreqChartView(10);
    GraphPanel panel("fft", chart, {{"auto_range_x", false}, {"minimum_x", 100},
                                  {"maximum_x", 500}, {"reverse_x", true}, {"axis_precision", 2}}, nullptr);
    chart->set_axis_precision(2);
    chart->update(std::vector<float>(200, 1));
    chart->update_freq_step(20);
    QValueAxis *horizontal = nullptr;
    for (auto *axis : chart->chart()->axes(Qt::Horizontal))
        if (axis->isVisible()) horizontal = qobject_cast<QValueAxis *>(axis);
    if (!horizontal || horizontal->min() != 100 || horizontal->max() != 500 ||
        !horizontal->isReverse() || horizontal->labelFormat() != "%.2f") return 1;
    auto *reverseY = panel.findChild<QCheckBox *>("fftReverseY");
    auto *autoX = panel.findChild<QCheckBox *>("fftAutoRangeX");
    if (!reverseY || !autoX) return 2;
    reverseY->setChecked(true);
    for (auto *axis : chart->chart()->axes(Qt::Vertical)) if (!axis->isReverse()) return 3;
    autoX->setChecked(true);
    if (horizontal->min() != 0 || horizontal->max() != 4000) return 4;
    auto *minX = panel.findChild<QDoubleSpinBox *>("fftMinimumX");
    minX->setValue(600);
    if (horizontal->min() != 0 || horizontal->max() != 4000) return 5;
    SpectrogramGraph spectrogram;
    // Renderer initialization deliberately requires a native session. Validate
    // settings lifecycle here without creating an OpenGL widget/context.
    int userEdits = 0;
    QObject::connect(&spectrogram, &SpectrogramGraph::surface_settings_changed, &spectrogram,
                     [&userEdits](const QJsonObject &) { ++userEdits; });
    auto options = spectrogram.surface_settings();
    options["y_range_mode"] = "manual"; options["y_min"] = -.5; options["y_max"] = 4;
    spectrogram.set_surface_settings(options);
    if (userEdits || spectrogram.y_range().first != -.5 || spectrogram.y_range().second != 4)
        return 7;
    AxisControlWidget axis("Amplitude (Y)");
    int writes = 0;
    QObject::connect(&axis, &AxisControlWidget::range_changed, &axis, [&writes] { ++writes; });
    axis.set_state(.123456789123, 1, false, true);
    if (writes || std::abs(axis.minimum_box()->value() - .123456789123) > 1e-12 ||
        axis.maximum_box()->text().contains("000000") || axis.minimumSizeHint().width() > 160) {
        std::fprintf(stderr, "Compact axis: writes=%d value=%.15g maximum=%s minimum width=%d\n",
                     writes, axis.minimum_box()->value(), qPrintable(axis.maximum_box()->text()),
                     axis.minimumSizeHint().width());
        return 8;
    }
    axis.maximum_box()->setValue(.01);
    if (writes != 1 || axis.maximum_box()->value() <= axis.minimum_box()->value()) return 9;
    QScrollArea scroll;
    scroll.setWidgetResizable(true);scroll.setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    auto *body=new QWidget;
    body->setSizePolicy(QSizePolicy::Ignored,QSizePolicy::Preferred);
    auto *bodyLayout=new QVBoxLayout(body);
    auto *compact=new AxisControlWidget("Frequency, Mel bands (Z)",body);
    compact->set_hint("Range (%)\nHistory: 400 / 400 frames");
    bodyLayout->addWidget(compact);bodyLayout->addStretch();scroll.setWidget(body);
    scroll.resize(200,220);scroll.show();app.processEvents();
    for(auto *label:compact->findChildren<QLabel *>()) {
        if(label->isVisible() && label->wordWrap() && label->height()<label->heightForWidth(label->width())) {
            std::fprintf(stderr,"Wrapped label clipped: %s %dx%d requires %d\n",qPrintable(label->text()),
                         label->width(),label->height(),label->heightForWidth(label->width()));return 10;
        }
    }
    std::puts("GraphPanel axes: restore, dynamic FFT range, reverse, precision and validation passed");
    return 0;
}
