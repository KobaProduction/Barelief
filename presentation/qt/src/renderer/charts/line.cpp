#include "core/performance.h"
#include "visualization/charts/line.h"

#include <QLineSeries>
#include <algorithm>

LineChartView::LineChartView() {
    chart()->setTitle("Line chart");
    _series = new QLineSeries;
    _init_series();
}

void LineChartView::_update_chart_data() {
    core::perf::Scope trace(core::perf::Stage::ChartGeometry);
    _points.clear();
    const auto count=_data.size();
    const size_t buckets=std::max(1,static_cast<int>(chart()->plotArea().width()));
    _points.reserve(static_cast<qsizetype>(std::min(count,2*buckets+2)));
    if(plot_style()=="bars") {
        const auto columns=std::min(count,buckets);
        for(size_t bucket=0;bucket<columns;++bucket) {
            const auto first=bucket*count/columns,end=(bucket+1)*count/columns;
            const auto peak=std::max_element(_data.begin()+first,_data.begin()+end,[](float a,float b){return std::abs(a)<std::abs(b);});
            const auto index=size_t(peak-_data.begin());const auto x=projected_x_coordinate(index);
            _points.append({x,0});_points.append({x,*peak});_points.append({x,0});
        }
    } else if (count<=2*buckets) {
        for (size_t i=0;i<count;++i) _points.append(QPointF(projected_x_coordinate(i),_data[i]));
    } else {
        _points.append(QPointF(projected_x_coordinate(0),_data.front()));
        for (size_t bucket=0;bucket<buckets;++bucket) {
            const auto first=bucket*count/buckets, end=(bucket+1)*count/buckets;
            const auto [low,high]=std::minmax_element(_data.begin()+first,_data.begin()+end);
            const auto low_index=static_cast<size_t>(low-_data.begin()), high_index=static_cast<size_t>(high-_data.begin());
            auto append=[&](size_t index) { _points.append(QPointF(projected_x_coordinate(index),_data[index])); };
            if (low_index<=high_index) { append(low_index); if (low_index!=high_index) append(high_index); }
            else { append(high_index); append(low_index); }
        }
        _points.append(QPointF(projected_x_coordinate(count-1),_data.back()));
    }
    auto* line=static_cast<QLineSeries *>(_series);
    line->setPointsVisible(count==1||plot_style()=="points");line->setMarkerSize(6);line->replace(_points);
}


