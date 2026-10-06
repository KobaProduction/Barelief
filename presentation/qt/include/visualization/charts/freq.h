#ifndef QT_VISUALIZATION_FREQ_H
#define QT_VISUALIZATION_FREQ_H

#include "visualization/charts/line.h"

class FreqChartView final: public LineChartView {
protected:
    qreal _freq_step = 0;
    QValueAxis  _freq_axis;
    void _on_updated_data_size(size_t new_size) override;
    void _update_chart_data() override;

public:
    explicit FreqChartView(qreal freq_step = 1);
    ~FreqChartView() override;

    void update_freq_step(qreal freq_step);
};

#endif //QT_VISUALIZATION_FREQ_H
