#pragma once
#include "barelief/charts/line.h"
class FreqChartView final : public LineChartView {
public:
    explicit FreqChartView(qreal step=1);
    void update_freq_step(qreal step);
protected:
    qreal _x_step() const override {return _freq_step;}
private:
    qreal _freq_step=1;
};
