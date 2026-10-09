#pragma once
#include <visualization/axis_tick_labels.h>
#include <visualization/ui_text.h>
#include <QFontMetrics>
#include <QVariantList>
#include <functional>
inline QVariantList quickPlotTicks(double extent,const QFont& font,int precision,const std::function<double(double)>& coordinate,bool horizontal,int maximum=32,const std::function<double(double)>& placement={}) {
    const QFontMetrics metrics(font);
    int divisions=std::clamp(int(std::max(1.,extent)/(horizontal?60.:metrics.height()+10.)),1,std::max(1,maximum));
    QStringList labels;
    const QLocale locale(ui::Language::instance().code()=="ru"?"ru_RU":"en_US");
    const auto position=[&](int i){const double f=double(i)/divisions;return placement?placement(f):f;};
    for(;;){std::vector<double> ticks;for(int i=0;i<=divisions;++i)ticks.push_back(coordinate(position(i)));
        labels=axisTickLabels(ticks,locale,precision);int needed=metrics.height();
        if(horizontal)for(const auto& label:labels)needed=std::max(needed,metrics.horizontalAdvance(label));
        if(divisions==1||extent/divisions>=needed+12)break;--divisions;}
    QVariantList result;for(int i=0;i<=divisions;++i)result.append(QVariantMap{{"position",position(i)},{"text",labels[i]}});return result;
}
inline double quickPlotLeftMargin(const QVariantList& ticks,const QFont& font,int width) {
    const QFontMetrics metrics(font);int longest=0;
    for(const auto& tick:ticks)longest=std::max(longest,metrics.horizontalAdvance(tick.toMap().value("text").toString()));
    return std::min(std::max(82,longest+38),std::max(82,width/2));
}
