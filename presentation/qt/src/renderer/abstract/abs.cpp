#include <barelief/abstract/abs.h>
#include <core/performance.h>
#include <barelief/quick2d/plot_ticks.h>
#include <barelief/ui_text.h>
#include <QPaintEvent>
#include <algorithm>
#include <utility>
#include <cmath>
AbstractChartView::AbstractChartView():_frame_pump([this](auto token){
    mark_submission(token);publish_presentation();_on_chart_updated();
}) {_frame_pump.setVisible(false);}
AbstractChartView::~AbstractChartView(){_frame_pump.setVisible(false);}
void AbstractChartView::set_scalar_readout(const QString& exact,bool scalar,bool available) {
    const QString text=available?exact:QStringLiteral("—");
    if(_scalar_mode==scalar&&_scalar_available==available&&_scalar_text==text)return;
    _scalar_mode=scalar;_scalar_available=available;_scalar_text=text;refresh_presentation();
}
void AbstractChartView::set_plot_style(const QString& style) {
    _plot_style=style=="bars"||style=="points"?style:QStringLiteral("line");_need_update=true;refresh_presentation();
}
void AbstractChartView::set_series_style(double lineWidth,double pointSize,double barWidth) {
    if(_line_width==lineWidth && _point_size==pointSize && _bar_width==barWidth)return;
    _line_width=lineWidth;_point_size=pointSize;_bar_width=barWidth;refresh_presentation();
}
void AbstractChartView::set_render_fps(int fps){_frame_pump.setRate(fps);}
void AbstractChartView::set_render_visible(bool visible) {
    _render_visible=visible;
    QuickPlotWidget::set_render_visible(visible);
    _frame_pump.setVisible(visible && isVisible());
}
float AbstractChartView::peak_value() const {float peak=0;for(float value:_data)peak=std::max(peak,std::abs(value));return peak;}
void AbstractChartView::update(const std::vector<float>& data) {
    core::perf::Scope trace(core::perf::Stage::ChartData);
    if(data.size()>1048576){_source_data.clear();_data.clear();_refresh_x_range();_need_update=true;_schedule_dirty_presentation();return;}
    _source_data=data;
    const auto previousX=x_range(),previousY=y_range();
    map_source_data(data);
    _refresh_x_range();
    observe_amplitude_range();
    if(previousX!=x_range()||previousY!=y_range())_need_update=true;
    _schedule_dirty_presentation();
}
void AbstractChartView::map_source_data(const std::vector<float>& data){
    if(data.size()!=_data.size()){_need_update=true;_data.resize(data.size());_on_updated_data_size(data.size());}
    for(size_t i=0;i<data.size();++i){
        const float value=float(_value_scale.map(data[i]));
        if(_data[i]!=value)_need_update=true;
        _data[i]=value;
    }
}
void AbstractChartView::observe_amplitude_range(){
    if(!_auto_resizing||_data.empty())return;
    const auto [low,high]=std::minmax_element(_data.begin(),_data.end());
    if(!_auto_gain){_last_min_value=std::min(_last_min_value,double(*low));_last_max_value=std::max(_last_max_value,double(*high));return;}
    if(!_amplitude_range.observe(*low,*high,AmplitudeRange::now(),_auto_range_interval,_gain_min_min,_gain_min_max))return;
    const auto bounds=_amplitude_range.range();_last_min_value=bounds.first;_last_max_value=bounds.second;
}
void AbstractChartView::_schedule_dirty_presentation(){
    if(_need_update)_frame_pump.request();
}
void AbstractChartView::set_auto_resizing(bool value){if(value&&!_auto_resizing)_amplitude_range.reset();_auto_resizing=value;}
void AbstractChartView::set_auto_gain(bool value,float maximum,float minimum,size_t){_auto_gain=value;_gain_min_max=std::isfinite(maximum)?maximum:1;_gain_min_min=std::isfinite(minimum)?minimum:-1;}
void AbstractChartView::set_range(qreal minimum,qreal maximum){if(!std::isfinite(minimum)||!std::isfinite(maximum)||minimum>=maximum||(_last_min_value==minimum&&_last_max_value==maximum))return;_last_min_value=minimum;_last_max_value=maximum;refresh_presentation();}
void AbstractChartView::set_x_range(bool automatic,qreal minimum,qreal maximum){if(!std::isfinite(minimum)||!std::isfinite(maximum)||minimum>=maximum)return;_automatic_x_range=automatic;_manual_x_min=minimum;_manual_x_max=maximum;_refresh_x_range();refresh_presentation();}
void AbstractChartView::_refresh_x_range(){
    _x_min=_manual_x_min;_x_max=_manual_x_max;
    if(!_automatic_x_range)return;
    _x_min=0;_x_max=std::max(qreal(1),_x_step()*qreal(_data.empty()?0:_data.size()-1));
    if(_x_coordinates.size()!=_data.size()||_x_coordinates.empty())return;
    _x_min=_x_coordinates.front()-(_scalar_mode&&_data.size()==1?1:0);
    _x_max=_data.size()>1?_x_coordinates.back():_x_coordinates.front()+(_scalar_mode?0:1);
}
bool AbstractChartView::set_x_coordinates(const std::vector<double>& values){
    if(values.size()>1048576)return false;
    for(size_t i=0;i<values.size();++i)if(!std::isfinite(values[i])||(i&&values[i]<=values[i-1]))return false;
    if(_x_coordinates==values)return true;
    _x_coordinates=values;_need_update=true;if(values.empty()||values.size()==_data.size())_refresh_x_range();_schedule_dirty_presentation();return true;
}
void AbstractChartView::set_axis_precision(int precision){_axis_precision=std::clamp(precision,0,8);refresh_presentation();}
void AbstractChartView::_update_chart_data(){
    _points.clear();
    const size_t buckets=size_t(std::clamp(width()-100,1,2048));
    const auto [begin,end]=visible_data_interval();
    if(end<=begin)return;
    if(_plot_style=="bars"){append_bar_points(begin,end);return;}
    append_line_points(begin,end,buckets);
}
std::pair<size_t,size_t> AbstractChartView::visible_data_interval() const {
    // Include one neighbour on each side for clipping.
    const size_t count=_data.size();
    size_t begin=0,end=count;
    const auto [xMin,xMax]=visible_x_range();
    if((!_automatic_x_range || viewport_.width()<1)&&count){while(begin<count&&projected_x_coordinate(begin)<xMin)++begin;if(begin)--begin;
        end=begin;while(end<count&&projected_x_coordinate(end)<=xMax)++end;if(end<count)++end;}
    return {begin,end};
}
void AbstractChartView::append_projected_point(size_t index){
    _points.append({projected_x_coordinate(index),_data[index]});
}
void AbstractChartView::append_bucket_extrema(size_t first,size_t last){
    const auto [low,high]=std::minmax_element(_data.begin()+first,_data.begin()+last);
    const auto a=size_t(low-_data.begin()),b=size_t(high-_data.begin());
    append_projected_point(std::min(a,b));
    if(a!=b)append_projected_point(std::max(a,b));
}
void AbstractChartView::append_bar_points(size_t begin,size_t end){
    const size_t visible=end-begin;
    // Use physical columns so HiDPI displays retain resolvable bins.
    const auto physicalColumns=size_t(std::clamp((double(width())-_cached_plot_left-22)*devicePixelRatioF(),1.,2048.));
    if(visible<=physicalColumns){for(size_t i=begin;i<end;++i)append_projected_point(i);return;}
    // Keep both polarities at their supplied coordinates.
    const size_t columns=std::max(size_t{1},physicalColumns/2);
    for(size_t bucket=0;bucket<columns;++bucket){const auto first=begin+bucket*visible/columns,last=begin+(bucket+1)*visible/columns;
        append_bucket_extrema(first,last);}
}
void AbstractChartView::append_line_points(size_t begin,size_t end,size_t buckets){
    const size_t visible=end-begin;
    if(visible<=2*buckets){for(size_t i=begin;i<end;++i)append_projected_point(i);return;}
    append_projected_point(begin);
    for(size_t bucket=0;bucket<buckets;++bucket){const auto first=begin+bucket*visible/buckets,last=begin+(bucket+1)*visible/buckets;
        append_bucket_extrema(first,last);}
    append_projected_point(end-1);
}
void AbstractChartView::refresh_presentation(){
    _need_update=true;_frame_pump.request();
}
void AbstractChartView::publish_presentation(){
    if(!rendering_enabled_)return;
    core::perf::Scope trace(core::perf::Stage::ChartGeometry);
    const auto [xMin,xMax]=visible_x_range();
    const auto [yMin,yMax]=visible_y_range();
    const double spanX=std::max(1e-300,double(xMax-xMin)),spanY=std::max(1e-300,double(yMax-yMin));
    update_axis_ticks(xMin,xMax,yMin,yMax);
    _update_chart_data();
    presentation_model()->publishVector(vector_presentation(yMin,spanY),normalized_points(xMin,yMin,spanX,spanY));
    _need_update=false;
}
void AbstractChartView::update_axis_ticks(qreal xMin,qreal xMax,qreal yMin,qreal yMax){
    const QVariantList signature{width(),height(),_scalar_mode,_axis_precision,grid_x_,grid_y_,labels_x_,labels_y_,_reverse_x,_reverse_y,xMin,xMax,yMin,yMax,font(),ui::Language::instance().code()};
    if(signature==_axis_signature)return;
    _axis_signature=signature;
    _cached_y_ticks=quickPlotTicks(height()-(_scalar_mode?128:86),font(),_axis_precision,[=,this](double f){return yMin+(yMax-yMin)*(_reverse_y?f:1-f);},false,32,{},labels_y_);
    _cached_plot_left=quickPlotLeftMargin(_cached_y_ticks,font(),width());
    _cached_x_ticks=quickPlotTicks(width()-_cached_plot_left-22,font(),_axis_precision,[=,this](double f){return xMin+(xMax-xMin)*(_reverse_x?1-f:f);},true,32,{},labels_x_);
}
QPolygonF AbstractChartView::normalized_points(qreal xMin,qreal yMin,double spanX,double spanY) const {
    QPolygonF points;points.reserve(_points.size());
    for(const auto& point:std::as_const(_points)){double x=(point.x()-xMin)/spanX,y=(point.y()-yMin)/spanY;
        points.append(QPointF(_reverse_x?1-x:x,_reverse_y?y:1-y));}
    return points;
}
QVariantMap AbstractChartView::vector_presentation(double yMin,double spanY) const {
    return {{"reverseX",_reverse_x},{"reverseY",_reverse_y},{"kind","vector"},{"style",_plot_style},{"xTicks",_cached_x_ticks},{"yTicks",_cached_y_ticks},{"xGrid",quickPlotGrid(_cached_x_ticks,grid_x_)},{"yGrid",quickPlotGrid(_cached_y_ticks,grid_y_)},{"xTitle",_x_title},{"yTitle",_y_title},{"title",_title},
        {"lineWidth",_line_width},{"pointSize",_point_size},{"barWidth",_bar_width},{"plotLeft",_cached_plot_left},{"scalar",_scalar_mode},{"exact",_scalar_text},{"scalarAvailable",_scalar_available},{"baseline",_bar_baseline_minimum?(_reverse_y?0.:1.):(_reverse_y?(0-yMin)/spanY:1-(0-yMin)/spanY)}};
}
void AbstractChartView::paintEvent(QPaintEvent* event){
    if(!_render_visible)return;
    core::perf::Scope widgetPaint(core::perf::Stage::Quick2dCompose);
    core::perf::Scope trace(core::perf::Stage::ChartPaint);
    QuickPlotWidget::paintEvent(event);
}
void AbstractChartView::showEvent(QShowEvent* event){QWidget::showEvent(event);_frame_pump.setVisible(_render_visible);if(_render_visible)refresh_presentation();}
void AbstractChartView::hideEvent(QHideEvent* event){_frame_pump.setVisible(false);QWidget::hideEvent(event);}
