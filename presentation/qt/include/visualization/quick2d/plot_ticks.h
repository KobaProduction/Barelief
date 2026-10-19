#pragma once
#include <visualization/axis_tick_labels.h>
#include <visualization/ui_text.h>
#include <QFontMetrics>
#include <QVariantList>
#include <functional>
namespace quick_plot_tick_detail {
inline QLocale locale() {return QLocale(ui::Language::instance().code()=="ru"?"ru_RU":"en_US");}
inline std::vector<double> positions(int count,const std::function<double(double)>& placement={}) {
    std::vector<double> result;
    for(int i=0;i<count;++i) {
        const double fraction=count==1?.5:double(i)/(count-1);
        result.push_back(placement?placement(fraction):fraction);
    }
    return result;
}
inline QStringList labels(const std::vector<double>& positions,const std::function<double(double)>& coordinate,int precision) {
    std::vector<double> values;
    for(const auto position:positions)values.push_back(coordinate(position));
    return axisTickLabels(values,locale(),precision);
}
inline QVariantList ticks(const std::vector<double>& positions,const QStringList& labels) {
    QVariantList result;
    for(size_t i=0;i<positions.size();++i)result.append(QVariantMap{{"position",positions[i]},{"text",labels[int(i)]}});
    return result;
}
inline int divisions(double extent,const QFontMetrics& metrics,bool horizontal,int maximum) {
    // Sparse major-grid rhythm; powers of two preserve zero/half divisions.
    const double spacing=horizontal?std::max(80,metrics.horizontalAdvance(QStringLiteral("00000000"))+24):metrics.height()*7+12;
    const int capacity=std::clamp(int(std::max(1.,extent)/spacing),1,std::max(1,maximum));
    int result=1;
    while(result<=capacity/2)result*=2;
    return result;
}
inline int requiredSpacing(const QStringList& labels,const QFontMetrics& metrics,bool horizontal) {
    int needed=metrics.height();
    if(horizontal)for(const auto& label:labels)needed=std::max(needed,metrics.horizontalAdvance(label));
    return needed+12;
}
}
inline QVariantList quickPlotTicks(double extent,const QFont& font,int precision,const std::function<double(double)>& coordinate,bool horizontal,int maximum=32,const std::function<double(double)>& placement={},int requested=0) {
    if(requested<0)return {};
    if(requested>0){
        const auto positions=quick_plot_tick_detail::positions(std::clamp(requested,1,128));
        return quick_plot_tick_detail::ticks(positions,quick_plot_tick_detail::labels(positions,coordinate,precision));
    }
    const QFontMetrics metrics(font);
    int divisions=quick_plot_tick_detail::divisions(extent,metrics,horizontal,maximum);
    for(;;) {
        const auto positions=quick_plot_tick_detail::positions(divisions+1,placement);
        const auto labels=quick_plot_tick_detail::labels(positions,coordinate,precision);
        if(divisions==1 || extent/divisions>=quick_plot_tick_detail::requiredSpacing(labels,metrics,horizontal))return quick_plot_tick_detail::ticks(positions,labels);
        --divisions;
    }
}
inline double quickPlotLeftMargin(const QVariantList& ticks,const QFont& font,int width) {
    const QFontMetrics metrics(font);int longest=0;
    for(const auto& tick:ticks)longest=std::max(longest,metrics.horizontalAdvance(tick.toMap().value("text").toString()));
    return std::min(std::max(82,longest+38),std::max(82,width/2));
}

// Explicit line and label counts are independent, including zero-label grids.
inline QVariantList quickPlotGrid(const QVariantList& ticks,int requested) {
    if(requested==0)return ticks;
    QVariantList result;for(int i=0;i<std::clamp(requested,0,128);++i)
        result.append(QVariantMap{{"position",requested==1?.5:double(i)/(requested-1)}});
    return result;
}
