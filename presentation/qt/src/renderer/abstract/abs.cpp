#include "core/performance.h"
#include "visualization/abstract/abs.h"
#include <algorithm>
#include <cmath>
#include <QResizeEvent>
#include <QLabel>
#include <QXYSeries>
#include <QFontMetrics>
#include <QPainter>
#include <visualization/axis_tick_labels.h>
#include <visualization/ui_text.h>

AbstractChartView::AbstractChartView() {
    setChart(new QChart);
    setRenderHint(QPainter::Antialiasing);
    chart()->legend()->hide();
    chart()->setAnimationOptions(QChart::NoAnimation);
    _scalar_readout=new QLabel(this);_scalar_readout->setObjectName("scalarCurrentValue");
    _scalar_readout->setTextFormat(Qt::PlainText);_scalar_readout->setTextInteractionFlags(Qt::TextSelectableByMouse);
    _scalar_readout->setAlignment(Qt::AlignCenter);_scalar_readout->hide();
    connect(&ui::Theme::instance(),&ui::Theme::changed,this,[this]{refresh_theme();});
    // Qt can finish axis layout after resizeEvent returns. Rebuild decimation
    // against that final plot width, including when no new audio arrives.
    connect(chart(), &QChart::plotAreaChanged, this, [this] { _need_update = true; });

    set_auto_gain(false);

    chart()->addAxis(&_axis_x, Qt::AlignBottom);
    chart()->addAxis(&_axis_y, Qt::AlignLeft);
    chart()->setTitle("Abstract chart");

    _last_max_value = -std::numeric_limits<qreal>::infinity();
    _last_min_value = std::numeric_limits<qreal>::infinity();

    _axis_y.setRange(-1, 1);
    _axis_x.setLabelFormat("%.0f");

    connect(&_data_update_timer, &QTimer::timeout, this, [this] () {
        if (!_render_visible || !isVisible()) return;
        _flush_chart_resize();
        if (!_need_update) return;
        _update_chart_data();
        _on_chart_updated();
        _need_update = false;
    });

    _data_update_timer.setInterval(17);
    _data_update_timer.setTimerType(Qt::PreciseTimer);
    _refresh_axis_labels();
    refresh_theme();
    connect(&ui::Language::instance(),&ui::Language::changed,this,[this]{_refresh_axis_labels();});
}

AbstractChartView::~AbstractChartView() {
    _data_update_timer.stop();
    chart()->removeAxis(&_axis_x);
    chart()->removeAxis(&_axis_y);
}
void AbstractChartView::refresh_theme() {
    const auto colours=palette();
    chart()->setBackgroundBrush(colours.brush(QPalette::Base));
    chart()->setPlotAreaBackgroundBrush(colours.brush(QPalette::Base));
    chart()->setPlotAreaBackgroundVisible(true);
    chart()->setBackgroundPen(Qt::NoPen);chart()->setBackgroundRoundness(0);chart()->setDropShadowEnabled(false);
    chart()->setTitleBrush(colours.brush(QPalette::Text));
    for(auto* axis:chart()->axes()) {
        axis->setLabelsBrush(colours.brush(QPalette::Text));axis->setTitleBrush(colours.brush(QPalette::Text));
        auto grid=colours.color(QPalette::Mid);grid.setAlphaF(.3);
        axis->setGridLinePen(QPen(grid,.75));axis->setLinePen(QPen(colours.color(QPalette::Mid),1));
    }
    for(auto* series:chart()->series())if(auto* xy=qobject_cast<QXYSeries*>(series)) {
        QPen pen(colours.color(QPalette::Highlight),1.25);pen.setCosmetic(true);
        if(_plot_style=="points")pen.setStyle(Qt::NoPen);xy->setPen(pen);
        xy->setBrush(colours.brush(QPalette::Highlight));
    }
    _place_scalar_readout();
}
void AbstractChartView::_place_scalar_readout() {
    if(!_scalar_readout)return;
    _scalar_readout->setGeometry(8,6,std::max(1,width()-16),42);
    auto valueFont=font();valueFont.setBold(true);
    const double base=valueFont.pointSizeF()>0?valueFont.pointSizeF():10;
    valueFont.setPointSizeF(std::max(18.,base*1.6));
    while(valueFont.pointSizeF()>10 && QFontMetrics(valueFont).horizontalAdvance(_scalar_readout->text())>_scalar_readout->width()-8)
        valueFont.setPointSizeF(valueFont.pointSizeF()-1);
    _scalar_readout->setFont(valueFont);_scalar_readout->raise();
}
void AbstractChartView::set_scalar_readout(const QString& exact,bool scalar,bool available) {
    const bool changed=_scalar_mode!=scalar;
    _scalar_mode=scalar;
    _scalar_readout->setText(available?exact:QStringLiteral("—"));
    _scalar_readout->setToolTip(available?exact:ui::text("Current value unavailable"));
    _scalar_readout->setVisible(scalar && available);
    if(changed)chart()->setMargins(scalar?QMargins(8,52,8,8):QMargins(8,8,8,8));
    _place_scalar_readout();
}
QString AbstractChartView::scalar_readout_text() const {return _scalar_readout->text();}
void AbstractChartView::set_plot_style(const QString& style) {
    const QString normalized=style=="bars"||style=="points"?style:QStringLiteral("line");
    if(normalized==_plot_style)return;
    _plot_style=normalized;_need_update=true;refresh_theme();
}
void AbstractChartView::set_render_fps(int fps) {
    const int cap=std::clamp(fps,1,240);
    _data_update_timer.setInterval((1000+cap-1)/cap);
}
void AbstractChartView::set_render_visible(bool visible) {
    if(_render_visible==visible) return;
    _render_visible=visible;
    _data_update_timer.stop();
    setUpdatesEnabled(visible);
    if(visible && isVisible()) {
        _flush_chart_resize();
        _data_update_timer.start();
    }
}
float AbstractChartView::peak_value() const {
    float peak=0;
    for (float value : _data) peak=std::max(peak,std::abs(value));
    return peak;
}

void AbstractChartView::_init_series() {
    chart()->addSeries(_series);
    _series->attachAxis(&_axis_x);
    _series->attachAxis(&_axis_y);
    std::vector<float> initData(10, 0);
    update(initData);
    refresh_theme();
}

void AbstractChartView::update(const std::vector<float> &data) {
    core::perf::Scope trace(core::perf::Stage::ChartData);

    if (data.size() not_eq _data.size()) {
        _data.resize(data.size(), 0);
        _on_updated_data_size(data.size());
        _need_update = true;
    }
    for (int i = 0; i < data.size(); ++i) {
        const float value = std::isfinite(data[i]) ? data[i] : 0.0f;
        if (value != _data[i]) _need_update = true;
        _data[i] = value;
    }
    _refresh_x_range();
    if (_auto_resizing && !data.empty()) {
        const auto [low,high]=std::minmax_element(_data.begin(),_data.end());
        if(!_auto_gain) {
            _last_min_value=std::min(_last_min_value,double(*low));
            _last_max_value=std::max(_last_max_value,double(*high));
            _axis_y.setRange(_last_min_value,std::max(_last_min_value+1e-6,_last_max_value));
        } else {
            if(_amplitude_range.observe(*low,*high,AmplitudeRange::now(),_auto_range_interval,
                                        _gain_min_min,_gain_min_max)) {
                const auto bounds=_amplitude_range.range();
                _last_min_value=bounds.first;_last_max_value=bounds.second;
                if(_axis_y.min()!=bounds.first || _axis_y.max()!=bounds.second)
                    _axis_y.setRange(bounds.first,bounds.second);
            }
        }
    }
}

void AbstractChartView::set_auto_resizing(bool value) {
    if(value && !_auto_resizing)_amplitude_range.reset();
    _auto_resizing = value;
}

void AbstractChartView::set_auto_gain(const bool value, const float min_max, const float min_min, const size_t /*speed*/) {
    _auto_gain = value;
    _gain_min_max = std::isfinite(min_max) ? min_max : 1;
    _gain_min_min = std::isfinite(min_min) ? min_min : -1;
}

void AbstractChartView::set_range(const qreal min, const qreal max) {
    if (!std::isfinite(min) || !std::isfinite(max) || min >= max) return;
    _last_min_value = min;
    _last_max_value = max;
    _axis_y.setRange(min, max);
}

void AbstractChartView::set_x_range(bool automatic, qreal minimum, qreal maximum) {
    if (!std::isfinite(minimum) || !std::isfinite(maximum) || minimum >= maximum) return;
    _automatic_x_range = automatic;
    _manual_x_min = minimum; _manual_x_max = maximum;
    _on_updated_data_size(_data.size());
    _refresh_x_range();
    _need_update = true;
}

void AbstractChartView::_refresh_x_range() {
    qreal lower=_manual_x_min,upper=_manual_x_max;
    if(_automatic_x_range) {
        lower=0;upper=std::max(qreal(1),_x_step()*qreal(_data.empty()?0:_data.size()-1));
        if(_x_coordinates.size()==_data.size() && !_x_coordinates.empty()) {
            lower=_x_coordinates.front()-(_scalar_mode && _x_coordinates.size()==1?1:0);
            upper=_x_coordinates.size()>1?_x_coordinates.back():_x_coordinates.front()+(_scalar_mode?0:1);
        }
    }
    for(auto* axis:chart()->axes(Qt::Horizontal))if(auto* value=qobject_cast<QValueAxis*>(axis))
        if(value->min()!=lower || value->max()!=upper)value->setRange(lower,upper);
}

bool AbstractChartView::set_x_coordinates(const std::vector<double>& coordinates) {
    if(coordinates.size()>1048576)return false;
    for(size_t i=0;i<coordinates.size();++i)
        if(!std::isfinite(coordinates[i]) || (i && coordinates[i]<=coordinates[i-1]))return false;
    if(_x_coordinates==coordinates)return true;
    _x_coordinates=coordinates;_need_update=true;
    // A new-size projection is committed by update(); keep the current axis
    // until its matching data arrives instead of exposing an index range.
    if(_x_coordinates.empty() || _x_coordinates.size()==_data.size())_refresh_x_range();
    return true;
}
void AbstractChartView::set_axis_precision(int precision) {
    _axis_precision=std::clamp(precision,0,8);
    _refresh_axis_labels();
}

void AbstractChartView::_refresh_axis_labels() {
    const QLocale locale(ui::Language::instance().code()=="ru" ? "ru_RU" : "en_US");
    chart()->setLocale(locale);
    chart()->setLocalizeNumbers(true);
    for (auto *axis : chart()->axes())
        if (auto *value = qobject_cast<QValueAxis *>(axis)) {
            if(!value->property("adaptiveAxisLabels").toBool()) {
                value->setProperty("adaptiveAxisLabels",true);
                connect(value,&QValueAxis::rangeChanged,this,[this]{_refresh_axis_labels();});
                connect(value,&QValueAxis::tickCountChanged,this,[this]{_refresh_axis_labels();});
                connect(value,&QValueAxis::tickIntervalChanged,this,[this]{_refresh_axis_labels();});
                connect(value,&QValueAxis::tickAnchorChanged,this,[this]{_refresh_axis_labels();});
                connect(value,&QValueAxis::tickTypeChanged,this,[this]{_refresh_axis_labels();});
            }
            std::vector<double> ticks;
            if(value->tickType()==QValueAxis::TicksDynamic && value->tickInterval()>0) {
                const double step=value->tickInterval();
                const double first=value->tickAnchor()+std::ceil((value->min()-value->tickAnchor())/step)*step;
                ticks={first,first+step,value->max()};
            } else {
                const int count=std::max(2,value->tickCount());
                for(int i=0;i<count;++i) ticks.push_back(value->min()+(value->max()-value->min())*i/(count-1));
            }
            QString format;
            axisTickLabels(ticks,locale,_axis_precision,&format);
            if(value->labelFormat()!=format) value->setLabelFormat(format);
        }
}

void AbstractChartView::changeEvent(QEvent *event) {
    QChartView::changeEvent(event);
    if(event->type()==QEvent::PaletteChange)refresh_theme();
}

void AbstractChartView::paintEvent(QPaintEvent *event) {
    if(!_render_visible) return;
    core::perf::Scope trace(core::perf::Stage::ChartPaint);QChartView::paintEvent(event);
}

void AbstractChartView::showEvent(QShowEvent *event) {
    QChartView::showEvent(event);
    if(!_render_visible) return;
    _flush_chart_resize();
    _data_update_timer.start();
}
void AbstractChartView::hideEvent(QHideEvent *event) { _data_update_timer.stop(); QChartView::hideEvent(event); }

void AbstractChartView::resizeEvent(QResizeEvent *event) {
    // Keep viewport/input geometry current immediately. Chart axis layout and
    // resolution-dependent series rebuilding share the configured display budget.
    QGraphicsView::resizeEvent(event);
    _pending_resize = true;
    _place_scalar_readout();
}

void AbstractChartView::_flush_chart_resize() {
    if (!_pending_resize) return;
    _pending_resize = false;
    QResizeEvent event(size(), _chart_size);
    QChartView::resizeEvent(&event);
    _chart_size = size();
    _need_update = true;
}
