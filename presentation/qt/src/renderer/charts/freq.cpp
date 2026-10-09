#include "core/performance.h"
#include <QLineSeries>
#include <algorithm>
#include "visualization/charts/freq.h"
#include <cmath>

FreqChartView::FreqChartView(const qreal freq_step) {
    set_plot_style("bars");
    chart()->setTitle("Freq Chart");

    chart()->addAxis(&_freq_axis, Qt::AlignBottom);
    _freq_axis.setLabelFormat("%.0f");
    _series->detachAxis(&_axis_x);
    _series->attachAxis(&_freq_axis);
    _axis_x.hide();
    refresh_theme();
    update_freq_step(freq_step);
    _refresh_axis_labels();
}


FreqChartView::~FreqChartView() { chart()->removeAxis(&_freq_axis); }

void FreqChartView::_on_updated_data_size(const size_t new_size) {
    AbstractChartView::_on_updated_data_size(new_size);
    std::vector<QValueAxis *> axes;
    axes.push_back(&_freq_axis);
    axes.push_back(&_axis_x);
    for (const auto axis: axes) {
        axis->setTickCount(16);
    }

    update_freq_step(_freq_step);
}

void FreqChartView::update_freq_step(const qreal freq_step) {
    if (!std::isfinite(freq_step) || freq_step <= 0) return;
    if(_freq_step!=freq_step) _need_update=true;
    _freq_step = freq_step;
    _refresh_x_range();
    _freq_axis.show();
}


void FreqChartView::_update_chart_data() {
    if(plot_style()!="bars"){LineChartView::_update_chart_data();return;}
    static_cast<QLineSeries *>(_series)->setPointsVisible(false);
    core::perf::Scope trace(core::perf::Stage::ChartGeometry);
    _points.clear();
    const size_t count=_data.size();
    const size_t buckets=std::min(count,static_cast<size_t>(std::max(1,int(chart()->plotArea().width()))));
    _points.reserve(qsizetype(3*buckets));
    // One batched path of vertical spectrum stems, instead of thousands of bar items.
    // Maxima per screen column preserve narrow peaks; x positions remain in Hz.
    for(size_t bucket=0;bucket<buckets;++bucket) {
        const size_t first=bucket*count/buckets,end=(bucket+1)*count/buckets;
        const auto peak=std::max_element(_data.begin()+first,_data.begin()+end,[](float a,float b) { return std::abs(a)<std::abs(b); });
        const auto index=size_t(std::distance(_data.begin(),peak));
        const qreal x=_x_coordinates.size()!=_data.size()?index*_freq_step:projected_x_coordinate(index);
        _points.append(QPointF(x,0));_points.append(QPointF(x,*peak));_points.append(QPointF(x,0));
    }
    static_cast<QLineSeries *>(_series)->replace(_points);
}
