#ifndef QT_VISUALIZATION_ABS_H
#define QT_VISUALIZATION_ABS_H

#include <vector>
#include <QChartView>
#include <QValueAxis>
#include <QTimer>
#include <QElapsedTimer>
#include <visualization/amplitude_range.h>
class QLabel;

class AbstractChartView : public QChartView {
public:
    AbstractChartView();
    ~AbstractChartView() override;
    void set_auto_resizing(bool value);
    bool automatic_y_range() const noexcept {return _auto_resizing;}
    void refresh_theme();
    void set_scalar_readout(const QString& exact,bool scalar,bool available=true);
    bool scalar_readout_enabled() const noexcept {return _scalar_mode;}
    QString scalar_readout_text() const;
    void set_plot_style(const QString& style);
    QString plot_style() const {return _plot_style;}
    void set_auto_gain(bool value, float min_max = .001f, float min_min = -.001f, size_t speed = 10);
    void set_auto_range_interval(double seconds) {_auto_range_interval=AmplitudeRange::interval(seconds);}
    double auto_range_interval() const {return _auto_range_interval;}
    void set_range(qreal min, qreal max);
    void set_x_range(bool automatic, qreal minimum, qreal maximum);
    void set_axis_precision(int precision);
    bool set_x_coordinates(const std::vector<double>& coordinates);
    double projected_x_coordinate(size_t index) const {return _x_coordinates.size()!=_data.size() || index>=_x_coordinates.size()?double(index):_x_coordinates[index];}
    void update(const std::vector<float> &data);
    void set_render_fps(int fps);
    void set_render_visible(bool visible);
    size_t value_count() const { return _data.size(); }
    float peak_value() const;
protected:
    std::vector<float> _data;
    std::vector<double> _x_coordinates;
    bool _need_update = false;
    bool _pending_resize = false;
    bool _render_visible = true;
    QSize _chart_size;
    bool _auto_resizing = false;
    bool _auto_gain = false;
    bool _automatic_x_range = true;
    qreal _manual_x_min = 0, _manual_x_max = 1;
    qreal _gain_min_max{};
    qreal _gain_min_min{};
    qreal _last_min_value;
    qreal _last_max_value;

    AmplitudeRange _amplitude_range;
    double _auto_range_interval=10;
    QLabel* _scalar_readout=nullptr;
    bool _scalar_mode=false;
    QString _plot_style="line";
    void _place_scalar_readout();
    QTimer _data_update_timer;
    QAbstractSeries *_series{};
    QValueAxis _axis_x;
    QValueAxis _axis_y;

    void changeEvent(QEvent *event) override;
    void paintEvent(QPaintEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;
    void showEvent(QShowEvent *event) override;
    void hideEvent(QHideEvent *event) override;
    void _init_series();
    void _flush_chart_resize();
    void _refresh_axis_labels();
    void _refresh_x_range();
    virtual qreal _x_step() const {return 1;}
    int _axis_precision = 0;
    virtual void _update_chart_data() = 0;
    virtual void _on_chart_updated() {};
    virtual void _on_updated_data_size(size_t new_size) {};
};

#endif //QT_VISUALIZATION_ABS_H
