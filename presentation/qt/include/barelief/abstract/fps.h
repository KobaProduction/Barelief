#ifndef BARELIEF_FPS_H
#define BARELIEF_FPS_H

#include <QLabel>

#include "barelief/abstract/abs.h"
#include "barelief/abstract/mixins.h"

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

#endif //BARELIEF_FPS_H
