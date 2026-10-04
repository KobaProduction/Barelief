#include "core/performance.h"
#include "visualization/abstract/abs.h"
#include <algorithm>
#include <cmath>

AbstractChartView::AbstractChartView() {
    setChart(new QChart);
    chart()->legend()->hide();

    set_auto_gain(false);

    chart()->addAxis(&_axis_x, Qt::AlignBottom);
    chart()->addAxis(&_axis_y, Qt::AlignLeft);
    chart()->setTitle("Abstract chart");

    _last_max_value = -std::numeric_limits<qreal>::infinity();
    _last_min_value = std::numeric_limits<qreal>::infinity();

    _axis_y.setRange(-1, 1);
    _axis_x.setLabelFormat("%.0f");

    connect(&_data_update_timer, &QTimer::timeout, this, [this] () {
        if (!isVisible() || !_need_update) return;
        _update_chart_data();
        _on_chart_updated();
        _need_update = false;
    });

    _data_update_timer.setInterval(17);
    _data_update_timer.setTimerType(Qt::PreciseTimer);
}

AbstractChartView::~AbstractChartView() {
    _data_update_timer.stop();
    chart()->removeAxis(&_axis_x);
    chart()->removeAxis(&_axis_y);
}

void AbstractChartView::_init_series() {
    chart()->addSeries(_series);
    _series->attachAxis(&_axis_x);
    _series->attachAxis(&_axis_y);
    std::vector<float> initData(10, 0);
    update(initData);
}

void AbstractChartView::update(const std::vector<float> &data) {
    core::perf::Scope trace(core::perf::Stage::ChartData);

    if (data.size() not_eq _data.size()) {
        _data.resize(data.size(), 0);
        _axis_x.setRange(0, static_cast<qreal>(_data.size()));
        _on_updated_data_size(data.size());
        _need_update = true;
    }
    for (int i = 0; i < data.size(); ++i) {
        const float value = std::isfinite(data[i]) ? data[i] : 0.0f;
        if (value != _data[i]) _need_update = true;
        _data[i] = value;
    }
    if (_auto_resizing && !data.empty()) {
        const auto max = std::ranges::max_element(_data);
        const auto min = std::ranges::min_element(_data);

        bool needUpdate = false;

        if (_auto_gain and _gain_counter++ > _gain_speed) {
            _last_max_value *= 0.9;
            _last_min_value *= 0.9;

            if (_last_max_value < _gain_min_max) _last_max_value = _gain_min_max;
            if (_last_min_value > _gain_min_min) _last_min_value = _gain_min_min;
            _gain_counter = 0;
            needUpdate = true;
        }

        if (*max > _last_max_value) {
            _last_max_value = *max;
            needUpdate = true;
        }
        if (*min < _last_min_value) {
            _last_min_value = *min;
            needUpdate = true;
        }
        if (needUpdate) {
            _axis_y.setRange(_last_min_value, _last_max_value);
        }
    }
}

void AbstractChartView::set_auto_resizing(bool value) {
    _auto_resizing = value;
}

void AbstractChartView::set_auto_gain(const bool value, const float min_max, const float min_min, const size_t speed) {
    _auto_gain = value;
    _gain_min_max = min_max;
    _gain_min_min = min_min;
    _gain_speed = speed;
}

void AbstractChartView::set_range(const qreal min, const qreal max) {
    _last_min_value = min;
    _last_max_value = max;
    _axis_y.setRange(min, max);
}

void AbstractChartView::paintEvent(QPaintEvent *event) { core::perf::Scope trace(core::perf::Stage::ChartPaint);QChartView::paintEvent(event); }

void AbstractChartView::showEvent(QShowEvent *event) { QChartView::showEvent(event); _data_update_timer.start(); }
void AbstractChartView::hideEvent(QHideEvent *event) { _data_update_timer.stop(); QChartView::hideEvent(event); }

void AbstractChartView::resizeEvent(QResizeEvent *event) { QChartView::resizeEvent(event); _need_update=true; }
