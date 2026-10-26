#include <barelief/numeric_series_view.h>
#include <barelief/quick2d/plot_ticks.h>
#include <QPainter>
#include <QPainterPath>
#include <algorithm>
#include <array>
#include <limits>

namespace {
// Preserve first/last and both extrema in source order for each pixel column.
// A gap flushes the envelope and starts a new subpath, never a connecting line.
class PixelEnvelope {
public:
    void append(std::size_t index,QPointF point){
        const auto column=std::floor(point.x());
        if(occupied_ && column!=column_)flush();
        const Sample sample{index,point};
        if(!occupied_){points_.fill(sample);occupied_=true;column_=column;}
        points_[3]=sample;
        if(point.y()<points_[1].point.y())points_[1]=sample;
        if(point.y()>points_[2].point.y())points_[2]=sample;
    }
    void gap(){flush();connected_=false;}
    QPainterPath take_path(){flush();return std::move(path_);}
private:
    struct Sample {std::size_t index;QPointF point;};
    std::array<Sample,4> points_{};
    QPainterPath path_;
    bool occupied_=false,connected_=false;
    double column_=0;
    void flush(){
        if(!occupied_)return;
        std::sort(points_.begin(),points_.end(),[](const Sample& a,const Sample& b){return a.index<b.index;});
        for(std::size_t i=0;i<points_.size();++i){
            if(i && points_[i].index==points_[i-1].index)continue;
            if(connected_)path_.lineTo(points_[i].point);else path_.moveTo(points_[i].point);
            connected_=true;
        }
        occupied_=false;
    }
};
}

NumericSeriesView::NumericSeriesView(QWidget* parent):QuickPlotWidget(parent) {
    setMinimumSize(180,170);
    QObject::connect(presentation_model(),&QuickPlotModel::hoverRequested,this,[this](double x,double y){hover(x,y);});
}
void NumericSeriesView::set_series(std::vector<double> coordinates,std::vector<Series> series) {
    const auto finite=[](double value){return std::isfinite(value);};
    const auto ordered=[](double a,double b){return a>=b;};
    if(!std::all_of(coordinates.begin(),coordinates.end(),finite) || std::adjacent_find(coordinates.begin(),coordinates.end(),ordered)!=coordinates.end()){
        coordinates.clear();series.clear();
    }
    coordinates_=std::move(coordinates);series_=std::move(series);range_dirty_=true;refresh_presentation();
}
void NumericSeriesView::set_scale(ValueDisplayScale scale,bool automatic,double low,double high) {
    range_dirty_=range_dirty_ || scale_!=scale;
    scale_=scale;automatic_=automatic;low_=low;high_=high;refresh_presentation();
}
void NumericSeriesView::set_titles(QString x,QString y) {x_title_=std::move(x);y_title_=std::move(y);refresh_presentation();}
void NumericSeriesView::set_logarithmic_x(bool enabled) {logarithmic_=enabled;viewport_={0,0,1,1};refresh_presentation();}
void NumericSeriesView::set_bands(std::vector<Band> bands) {bands_=std::move(bands);refresh_presentation();}
void NumericSeriesView::set_horizontal_view(double left,double width) {
    if(viewport_.left()==left && viewport_.width()==width)return;
    viewport_.setLeft(left);viewport_.setWidth(width);refresh_presentation();
}
double NumericSeriesView::map_x(double value)const {return logarithmic_?std::log10(std::max(value,1e-9)):value;}
std::pair<double,double> NumericSeriesView::x_range()const {
    if(coordinates_.size()<2)return {0,1};
    const auto first=logarithmic_?std::find_if(coordinates_.begin(),coordinates_.end(),[](double v){return v>0;}):coordinates_.begin();
    if(first==coordinates_.end())return {0,1};
    return {map_x(*first),map_x(coordinates_.back())};
}
double NumericSeriesView::coordinate(double fraction)const {
    const auto [low,high]=x_range();const auto value=low+(viewport_.left()+fraction*viewport_.width())*(high-low);
    return logarithmic_?std::pow(10.,value):value;
}
std::pair<double,double> NumericSeriesView::data_range()const {
    double low=std::numeric_limits<double>::infinity(),high=-low;
    for(const auto& series:series_)for(const auto value:series.values)if(std::isfinite(value) && value>0){
        const auto projected=scale_.map(value);low=std::min(low,projected);high=std::max(high,projected);
    }
    return {low,high};
}
void NumericSeriesView::observe_range() {
    if(!automatic_)return;
    if(range_dirty_){observed_range_=data_range();range_dirty_=false;}
    const auto [low,high]=observed_range_;
    if(!std::isfinite(low)){low_=-120;high_=20;return;}
    low_=std::max(scale_.floor,std::floor((std::max(low,high-120)-5)/10)*10);
    high_=std::ceil((high+5)/10)*10;
}
void NumericSeriesView::draw_bands(QPainter& painter,const QSize& size,double minimum,double maximum)const {
    for(const auto& band:bands_){
        auto color=palette().color(band.emphasized?QPalette::Highlight:QPalette::Mid);color.setAlpha(band.emphasized?45:65);
        const auto left=(map_x(band.first)-minimum)/(maximum-minimum)*size.width();
        const auto right=(map_x(band.last)-minimum)/(maximum-minimum)*size.width();
        painter.fillRect(QRectF(left,0,right-left,size.height()),color);
    }
}
std::pair<std::size_t,std::size_t> NumericSeriesView::visible_indices(std::size_t count,double minimum,double maximum)const {
    const auto begin=coordinates_.begin(),end=begin+std::min(count,coordinates_.size());
    auto first=std::lower_bound(begin,end,logarithmic_?std::pow(10.,minimum):minimum);
    auto last=std::upper_bound(first,end,logarithmic_?std::pow(10.,maximum):maximum);
    if(first!=begin)--first;
    if(last!=end)++last;
    if(logarithmic_)first=std::max(first,std::upper_bound(begin,last,0.));
    return {std::size_t(first-begin),std::size_t(last-begin)};
}
QPainterPath NumericSeriesView::series_path(const Series& series,const QSize& size,double minimum,double maximum)const {
    if(maximum<=minimum || high_<=low_ || size.isEmpty())return {};
    const auto y0=low_+(1-viewport_.bottom())*(high_-low_),span=viewport_.height()*(high_-low_);
    const auto [first,last]=visible_indices(series.values.size(),minimum,maximum);
    PixelEnvelope envelope;
    for(auto i=first;i<last;++i){
        if(!std::isfinite(series.values[i])){envelope.gap();continue;}
        const QPointF point((map_x(coordinates_[i])-minimum)/(maximum-minimum)*size.width(),
            size.height()*(1-(scale_.map(series.values[i])-y0)/span));
        envelope.append(i,point);
    }
    return envelope.take_path();
}
void NumericSeriesView::draw_series(QPainter& painter,const QSize& size,double minimum,double maximum)const {
    const QPalette::ColorRole roles[]{QPalette::Highlight,QPalette::Link,QPalette::LinkVisited,QPalette::Text};
    for(std::size_t s=0;s<series_.size();++s){
        const auto& series=series_[s];
        painter.setPen(QPen(palette().color(roles[s%4]),s?1.5:1.8,series.dashed?Qt::DashLine:Qt::SolidLine));
        painter.drawPath(series_path(series,size,minimum,maximum));
    }
}
QImage NumericSeriesView::raster(double minimum,double maximum)const {
    const QSize pixels(std::clamp(width()-110,1,2048),std::clamp(height()-86,1,2048));
    QImage image(pixels,QImage::Format_ARGB32_Premultiplied);image.fill(palette().color(QPalette::Base));
    QPainter painter(&image);painter.setRenderHint(QPainter::Antialiasing);draw_bands(painter,pixels,minimum,maximum);
    if(maximum>minimum && high_>low_)draw_series(painter,pixels,minimum,maximum);
    return image;
}
void NumericSeriesView::refresh_presentation() {
    if(update_depth_){refresh_pending_=true;return;}
    refresh_pending_=false;
    observe_range();const auto [start,end]=x_range();const auto span=end-start;
    const auto minimum=start+viewport_.left()*span,maximum=start+viewport_.right()*span;
    publish_image(raster(minimum,maximum));
    const auto x=quickPlotNiceTicks(width()-110,font(),[this](double f){return coordinate(f);},true,logarithmic_);
    const auto y=quickPlotNiceTicks(height()-86,font(),[this](double f){return low_+(1-viewport_.top()-f*viewport_.height())*(high_-low_);},false);
    QStringList names;for(const auto& series:series_)names.append(series.name);
    publish({{"kind","image"},{"available",!coordinates_.empty()},{"xTicks",x},{"yTicks",y},
        {"xGrid",quickPlotMinorGrid(x,width()-110)},{"yGrid",quickPlotMinorGrid(y,height()-86)},
        {"xTitle",x_title_},{"yTitle",y_title_},{"plotLeft",quickPlotLeftMargin(y,font(),width())},{"title",names.join(" · ")}});
}
void NumericSeriesView::hover(double x,double) {
    if(coordinates_.empty())return;
    const auto value=coordinate(x);const auto at=std::lower_bound(coordinates_.begin(),coordinates_.end(),value);
    const auto index=std::min(std::size_t(at-coordinates_.begin()),coordinates_.size()-1);
    QStringList parts{QString::number(coordinates_[index],'g',7)};
    for(const auto& series:series_)if(index<series.values.size())
        parts.append(series.name+": "+(std::isfinite(series.values[index])?QString::number(scale_.map(series.values[index]),'g',6):QStringLiteral("—")));
    auto state=presentation_model()->state();state["title"]=parts.join(" · ");presentation_model()->publish(state);
}
