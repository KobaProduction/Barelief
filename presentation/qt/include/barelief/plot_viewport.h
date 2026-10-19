#pragma once
#include <QRectF>
#include <QString>
#include <algorithm>
#include <cmath>
#include <utility>

// A transient presentation crop. Source samples, DSP and saved axis preferences
// are independent of navigation. The full configured range is [0,1] per axis.
namespace plot_viewport_detail {
inline std::pair<double,double> changeAxis(double start,double span,double position,const QString& action,double amount,bool reverse) {
    if(action=="zoom") {
        const double next=std::clamp(span*std::clamp(amount,.1,10.),1e-6,1.);
        const double anchor=std::clamp(position,0.,1.);
        start+=(reverse?1-anchor:anchor)*(span-next);
        span=next;
    } else start+=(reverse?position:-position)*span;
    return {std::clamp(start,0.,1.-span),span};
}
}
inline QRectF navigatePlotViewport(QRectF view,const QString& action,double x,double y,
                                  double amount,int axes,bool reverseX=false,bool reverseY=false) {
    if(!std::isfinite(x)||!std::isfinite(y)||!std::isfinite(amount))return view;
    if(action=="reset")return {0,0,1,1};
    if(action!="zoom" && action!="pan")return view;
    const auto [left,width]=axes&1?plot_viewport_detail::changeAxis(view.x(),view.width(),x,action,amount,reverseX):std::pair{view.x(),view.width()};
    const auto [top,height]=axes&2?plot_viewport_detail::changeAxis(view.y(),view.height(),y,action,amount,reverseY):std::pair{view.y(),view.height()};
    return {left,top,width,height};
}
