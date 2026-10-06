#ifndef QT_VISUALIZATION_BAR_H
#define QT_VISUALIZATION_BAR_H

#include <QBarSet>

#include "visualization/abstract/fps.h"

class BarChartView : public FPSChartView {
protected:
    QBarSet *_set;
    QPen _pen;

public:
    BarChartView();

    void _update_chart_data() override;
};

#endif //QT_VISUALIZATION_BAR_H
