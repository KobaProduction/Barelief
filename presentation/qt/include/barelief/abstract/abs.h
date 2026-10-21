#pragma once
#include <barelief/quick2d/plot_model.h>
#include <barelief/amplitude_range.h>
#include <barelief/rendering/qt_frame_pump.h>
#include <vector>
#include <barelief/value_display_scale.h>
#include <QPolygonF>
#include <QVariantMap>

class AbstractChartView : public QuickPlotWidget {
public:
    AbstractChartView();
    ~AbstractChartView() override;
    void set_auto_resizing(bool value);
    bool automatic_y_range() const noexcept {return _auto_resizing;}
    void refresh_theme() {refresh_presentation();}
    void set_scalar_readout(const QString& exact,bool scalar,bool available=true);
    bool scalar_readout_enabled() const noexcept {return _scalar_mode;}
    QString scalar_readout_text() const {return _scalar_text;}
    void set_plot_style(const QString& style);
    void set_bar_baseline_minimum(bool value){if(_bar_baseline_minimum==value)return;_bar_baseline_minimum=value;refresh_presentation();}
    bool bar_baseline_minimum()const{return _bar_baseline_minimum;}
    void set_series_style(double lineWidth,double pointSize,double barWidth);
    QString plot_style() const {return _plot_style;}
    void set_auto_gain(bool value,float min_max=.001f,float min_min=-.001f,size_t speed=10);
    void set_auto_range_interval(double seconds) {_auto_range_interval=AmplitudeRange::interval(seconds);}
    double auto_range_interval() const {return _auto_range_interval;}
    void set_range(qreal min,qreal max);
    void set_x_range(bool automatic,qreal minimum,qreal maximum);
    void set_axis_precision(int precision);
    bool set_x_coordinates(const std::vector<double>& coordinates);
    double projected_x_coordinate(size_t index) const {return _x_coordinates.size()!=_data.size()||index>=_x_coordinates.size()?double(index)*_x_step():_x_coordinates[index];}
    void update(const std::vector<float>& data);
    void set_value_scale(const ValueDisplayScale& scale){if(_value_scale==scale)return;_value_scale=scale;_amplitude_range.reset();update(_source_data);refresh_presentation();}
    const ValueDisplayScale& value_scale()const{return _value_scale;}
    void set_render_fps(int fps);
    void set_render_visible(bool visible);
    size_t value_count() const {return _data.size();}
    float peak_value() const;
    void setTitle(const QString& title) {if(_title==title)return;_title=title;_need_update=true;refresh_presentation();}
    QString title() const {return _title;}
    void set_projection_status(const QString& status) {setTitle(status);}
    void set_axis_titles(const QString& x,const QString& y) {if(_x_title==x&&_y_title==y)return;_x_title=x;_y_title=y;refresh_presentation();}
    void set_reverse_axes(bool x,bool y) {if(_reverse_x==x&&_reverse_y==y)return;_reverse_x=x;_reverse_y=y;refresh_presentation();}
    bool reverse_x() const {return _reverse_x;}
    bool reverse_y() const {return _reverse_y;}
    QString x_title() const {return _x_title;}
    QString y_title() const {return _y_title;}
    int axis_precision() const {return _axis_precision;}
    std::pair<double,double> x_range() const {return {_x_min,_x_max};}
    std::pair<double,double> y_range() const {return {_last_min_value,_last_max_value};}
    std::pair<double,double> visible_x_range() const {return {_x_min+viewport_.left()*(_x_max-_x_min),_x_min+viewport_.right()*(_x_max-_x_min)};}
    std::pair<double,double> visible_y_range() const {return {_last_min_value+viewport_.top()*(_last_max_value-_last_min_value),_last_min_value+viewport_.bottom()*(_last_max_value-_last_min_value)};}
    size_t projected_point_count() const {return size_t(_points.size());}
    QList<QPointF> projected_points() const {return _points;}
protected:
    std::vector<float> _data,_source_data;
    ValueDisplayScale _value_scale;
    std::vector<double> _x_coordinates;
    QList<QPointF> _points;
    bool _need_update=false,_render_visible=true,_auto_resizing=false,_auto_gain=false,_automatic_x_range=true;
    qreal _manual_x_min=0,_manual_x_max=1,_x_min=0,_x_max=1;
    qreal _gain_min_max=.001,_gain_min_min=-.001,_last_min_value=-1,_last_max_value=1;
    AmplitudeRange _amplitude_range;
    double _auto_range_interval=10;
    bool _bar_baseline_minimum=false;
    bool _scalar_mode=false,_scalar_available=false,_reverse_x=false,_reverse_y=false;
    QString _scalar_text,_plot_style="line",_title,_x_title,_y_title;
    barelief::rendering::QtFramePump _frame_pump;
    void publish_presentation();
    void frame_completed(quint64 token) override {_frame_pump.complete(token);}
    void refresh_presentation() override;
    void paintEvent(QPaintEvent* event) override;
    void showEvent(QShowEvent* event) override;
    void hideEvent(QHideEvent* event) override;
    void _refresh_x_range();
    void _schedule_dirty_presentation();
    void map_source_data(const std::vector<float>& data);
    void observe_amplitude_range();
    void append_projected_point(size_t index);
    std::pair<size_t,size_t> visible_data_interval() const;
    void append_bucket_extrema(size_t first,size_t last);
    void append_bar_points(size_t begin,size_t end);
    void append_line_points(size_t begin,size_t end,size_t buckets);
    void update_axis_ticks(qreal x_min,qreal x_max,qreal y_min,qreal y_max);
    QPolygonF normalized_points(qreal x_min,qreal y_min,double span_x,double span_y) const;
    QVariantMap vector_presentation(double y_min,double span_y) const;
    virtual qreal _x_step() const {return 1;}
    QVariantList _axis_signature,_cached_x_ticks,_cached_y_ticks;
    double _cached_plot_left=82;
    int _axis_precision=0;
    double _line_width=1.25,_point_size=12,_bar_width=80;
    virtual void _update_chart_data();
    virtual void _on_chart_updated() {}
    virtual void _on_updated_data_size(size_t) {}
};
