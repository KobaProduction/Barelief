#include "visualization/charts/freq.h"
#include <cmath>
FreqChartView::FreqChartView(qreal step){set_plot_style("bars");update_freq_step(step);}
void FreqChartView::update_freq_step(qreal step){if(!std::isfinite(step)||step<=0)return;_freq_step=step;_refresh_x_range();refresh_presentation();}
