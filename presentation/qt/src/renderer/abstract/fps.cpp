#include "visualization/abstract/fps.h"

void FPSChartView::_setFPS(const qreal fps) { _fps=fps; emit fps_changed(fps); }

FPSChartView::FPSChartView() : FPSMixin(this) {
    setFPSCallback([this](qreal fps) {
        _setFPS(fps);
    });
}

void FPSChartView::paintEvent(QPaintEvent *event) {
    AbstractChartView::paintEvent(event);
    _frameCall();
}
