#ifndef QT_VISUALIZATION_FPS_H
#define QT_VISUALIZATION_FPS_H

#include <QLabel>

#include "visualization/abstract/abs.h"
#include "visualization/abstract/mixins.h"

class FPSChartView : public AbstractChartView, public FPSMixin {
    Q_OBJECT
public:
    explicit FPSChartView();
    qreal display_fps() const { return rendered_fps(); }
signals:
    void fps_changed(qreal fps);

protected:
    qreal _fps=0;

    void _setFPS(qreal fps);

    void paintEvent(QPaintEvent *event) override;
};

#endif //QT_VISUALIZATION_FPS_H
