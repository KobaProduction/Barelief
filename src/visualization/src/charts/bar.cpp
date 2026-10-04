#include "core/performance.h"
#include "visualization/charts/bar.h"

#include <QBarSeries>

BarChartView::BarChartView() {
    chart()->setTitle("Bar chart");
    _series = new QBarSeries;
    _set = new QBarSet("");
    _pen = _set->pen();
    _pen.setColor(Qt::blue);

    reinterpret_cast<QBarSeries *>(_series)->append(_set);
    _init_series();
}

void BarChartView::_update_chart_data() {
    core::perf::Scope trace(core::perf::Stage::ChartGeometry);
    if (_set) reinterpret_cast<QBarSeries *>(_series)->remove(_set);
    _set = new QBarSet("");_set->setPen(_pen);
    QList<qreal> values;values.reserve(qsizetype(_data.size()));
    for(float value:_data) values.append(value);
    _set->append(values);
    reinterpret_cast<QBarSeries *>(_series)->append(_set);
}
