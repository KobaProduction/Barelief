#ifndef QT_VISUALIZATION_ABS_H
#define QT_VISUALIZATION_ABS_H

#include <vector>
#include <QChartView>
#include <QValueAxis>
#include <QTimer>

class AbstractChartView : public QChartView {
public:
    AbstractChartView();
    ~AbstractChartView() override;
    void set_auto_resizing(bool value);
    void set_auto_gain(bool value, float min_max = 100.0f, float min_min = -100.0f, size_t speed = 10);
    void set_range(qreal min, qreal max);
    void update(const std::vector<float> &data);
    size_t value_count() const { return _data.size(); }
    float peak_value() const;
protected:
    std::vector<float> _data;
    bool _need_update = false;
    bool _pending_resize = false;
    QSize _chart_size;
    bool _auto_resizing = false;
    bool _auto_gain = false;
    qreal _gain_min_max{};
    qreal _gain_min_min{};
    size_t _gain_speed{};
    size_t _gain_counter = 0;
    qreal _last_min_value;
    qreal _last_max_value;

    QTimer _data_update_timer;
    QAbstractSeries *_series{};
    QValueAxis _axis_x;
    QValueAxis _axis_y;

    void paintEvent(QPaintEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;
    void showEvent(QShowEvent *event) override;
    void hideEvent(QHideEvent *event) override;
    void _init_series();
    void _flush_chart_resize();
    virtual void _update_chart_data() = 0;
    virtual void _on_chart_updated() {};
    virtual void _on_updated_data_size(size_t new_size) {};
};

#endif //QT_VISUALIZATION_ABS_H
