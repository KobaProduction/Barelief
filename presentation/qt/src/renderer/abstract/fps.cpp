#include "barelief/abstract/fps.h"

void FPSChartView::_setFPS(const qreal fps) { _fps=fps; emit fps_changed(display_fps()); }

FPSChartView::FPSChartView() : FPSMixin(this) {
    setFPSCallback([this](qreal fps) {
        Q_UNUSED(fps);_setFPS(rendered_fps());
    });
}

void FPSChartView::paintEvent(QPaintEvent *event) {
    AbstractChartView::paintEvent(event);

}
