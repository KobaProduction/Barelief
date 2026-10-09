#pragma once
#include <QVector3D>
#include <QVector4D>
#include <QJsonObject>
#include <algorithm>
#include <vector>
#include <utility>
#include <QColor>
#include <QRectF>
#include <QVector>
#include <cmath>
#include <limits>

inline QColor spectrogramTextColor(const QColor &background) {
    auto linear=[](double value){return value<=.04045 ? value/12.92 : std::pow((value+.055)/1.055,2.4);};
    const double luminance=.2126*linear(background.redF())+.7152*linear(background.greenF())+.0722*linear(background.blueF());
    return luminance>.179 ? QColor(Qt::black) : QColor(Qt::white);
}

inline QRectF spectrogramSlicePanel(QSizeF viewport, QPointF position = {1,1}) {
    const double padding = std::min(8.0, std::min(viewport.width(), viewport.height()) / 10);
    const double width = std::min(std::max(0.0, viewport.width() - 2 * padding),
                                  std::clamp(viewport.width() * .5, 280.0, 440.0));
    const double height = std::min(std::max(0.0, viewport.height() - 2 * padding),
                                   std::clamp(viewport.height() * .42, 180.0, 280.0));
    return {padding + std::clamp(position.x(),0.0,1.0) * std::max(0.0, viewport.width()-2*padding-width),
            padding + std::clamp(position.y(),0.0,1.0) * std::max(0.0, viewport.height()-2*padding-height), width, height};
}

inline QRectF spectrogramSlicePlot(QRectF panel) {
    return panel.adjusted(std::min(48.0, panel.width() * .25), std::min(24.0, panel.height() * .2),
                          -std::min(12.0, panel.width() * .1), -std::min(44.0, panel.height() * .3));
}

inline QRectF spectrogramAxisTitleRect(QPointF anchor,QPointF outward,QSizeF size,QRectF viewport,
                                      const QVector<QRectF> &occupied) {
    const double length=std::hypot(outward.x(),outward.y());
    outward=length>1e-6 ? outward/length : QPointF(0,1);
    const QPointF sideways(-outward.y(),outward.x());
    size.setWidth(std::min(size.width(),viewport.width()));size.setHeight(std::min(size.height(),viewport.height()));
    double bestScore=std::numeric_limits<double>::infinity();QRectF best;
    for(int step=0;step<8;++step) for(int offset:{0,-1,1,-2,2}) {
        const auto center=anchor+outward*(28+step*24)+sideways*(offset*32);
        QRectF candidate(center.x()-size.width()/2,center.y()-size.height()/2,size.width(),size.height());
        candidate.moveLeft(std::clamp(candidate.left(),viewport.left(),viewport.right()-size.width()));
        candidate.moveTop(std::clamp(candidate.top(),viewport.top(),viewport.bottom()-size.height()));
        double score=0;
        for(const auto &other:occupied) {const auto overlap=candidate.intersected(other);score+=overlap.width()*overlap.height();}
        if(score==0) return candidate;
        if(score<bestScore) {bestScore=score;best=candidate;}
    }
    return bestScore==0 ? best : QRectF{};
}

// Keep labels near their tick; omit crowded/offscreen labels instead of making
// a distant value appear to belong to another grid division.
inline QRectF spectrogramTickLabelRect(QRectF bounds,QRectF viewport,const QVector<QRectF> &occupied) {
    if(bounds.width()>viewport.width() || bounds.height()>viewport.height()) return {};
    auto placed=bounds;
    placed.moveLeft(std::clamp(placed.left(),viewport.left(),viewport.right()-placed.width()));
    placed.moveTop(std::clamp(placed.top(),viewport.top(),viewport.bottom()-placed.height()));
    if(std::hypot(placed.left()-bounds.left(),placed.top()-bounds.top())>24) return {};
    for(const auto &other:occupied) if(placed.adjusted(-3,-3,3,3).intersects(other)) return {};
    return placed;
}

template<class Matrix=std::vector<std::vector<float>>>
inline float spectrogramHistorySample(const Matrix &data,size_t band,size_t age,size_t capacity) {
    if(band>=data.size()) return 0;
    const auto &row=data[band];
    return age>=std::min(capacity,row.size()) ? 0 : row[row.size()-1-age];
}

// Automatic display extent includes zero and all actual finite samples. Empty
// or all-zero history uses 0..1; signed histories retain their negative extent.
inline std::pair<float,float> spectrogramAutomaticRange(const std::vector<std::vector<float>>& data) {
    float low=0,high=0;
    for(const auto& row:data)for(float value:row)if(std::isfinite(value)){low=std::min(low,value);high=std::max(high,value);}
    if(low==0 && high==0)return {0,1};
    return {low,high};
}

// Increasing X is frame age. Fixed visual ratios are independent of counts
// and crop; sample-count aspect is a separate, explicitly selected policy.
// Axis ranges describe sample centres: 0..count-1.
struct SpectrogramSceneGeometry {
    float xMin, xMax, zMin, zMax, scaleX, scaleZ;
    float yMin,yMax;
    bool clipY;
    bool reverseTime,reverseBands,reverseAmplitude;
    SpectrogramSceneGeometry(size_t history, size_t bands, const int *ranges,bool reverse_time=false,bool reverse_bands=false,
                             float y_min=0,float y_max=2,bool clip_y=false,bool reverse_amplitude=false,
                             bool sample_count_aspect=false,float time_aspect=2,float band_aspect=1)
        : yMin(y_min),yMax(y_max),clipY(clip_y),reverseTime(reverse_time),reverseBands(reverse_bands),reverseAmplitude(reverse_amplitude) {
        const float oldest=history ? float(history-1) : 0;
        xMin = oldest * ranges[0] / 100.f; xMax = oldest * ranges[1] / 100.f;
        const float lastBand=bands ? float(bands-1) : 0;
        zMin = lastBand * ranges[2] / 100.f; zMax = lastBand * ranges[3] / 100.f;
        const float xRange = std::max(.001f, xMax-xMin), zRange = std::max(.001f, zMax-zMin);
        scaleX = sample_count_aspect ? 2*xRange/std::max(xRange,zRange) :
            std::isfinite(time_aspect)?std::clamp(time_aspect,.25f,4.f):2.f;
        scaleZ = sample_count_aspect ? 2*zRange/std::max(xRange,zRange) :
            std::isfinite(band_aspect)?std::clamp(band_aspect,.25f,4.f):1.f;
    }
    QVector3D point(float frame, float band, float value) const {
        const float height=valueFraction(value);
        return {(reverseTime ? -1.f : 1.f)*scaleX * (xMax>xMin ? -1 + 2*(frame-xMin)/(xMax-xMin) : 0),
                (reverseAmplitude ? -1.f : 1.f)*(2*(clipY ? std::clamp(height,0.f,1.f) : height)-1),
                (reverseBands ? -1.f : 1.f)*scaleZ * (zMax>zMin ? 1 - 2*(band-zMin)/(zMax-zMin) : 0)};
    }
    float timeTick(float fraction) const {return xMin+(reverseTime ? 1-fraction : fraction)*(xMax-xMin);}
    float bandTick(float fraction) const {return zMin+(reverseBands ? 1-fraction : fraction)*(zMax-zMin);}
    float valueFraction(float value) const {
        const double fraction=(double(value)-yMin)/std::max(double(std::numeric_limits<float>::denorm_min()),double(yMax)-yMin);
        return float(std::clamp(fraction,-double(std::numeric_limits<float>::max()),double(std::numeric_limits<float>::max())));
    }
    float signalTick(float fraction) const {return float(double(yMin)+(reverseAmplitude ? 1-double(fraction) : double(fraction))*(double(yMax)-yMin));}
    float verticalFraction(float value) const {const float f=valueFraction(value);return reverseAmplitude ? 1-f : f;}
};


// Shared renderer/telemetry projection policy. Options have presentation
// semantics only; physical metadata, samples and camera are not modified.
inline SpectrogramSceneGeometry spectrogramSceneGeometry(size_t history,size_t bands,const int* ranges,
    const QJsonObject& options,std::pair<float,float> automatic) {
    const bool manual=options["y_range_mode"]=="manual";
    return {history,bands,ranges,options["reverse_time"].toBool(),options["reverse_bands"].toBool(),
        manual?float(options["y_min"].toDouble()):automatic.first,
        manual?float(options["y_max"].toDouble(1)):automatic.second,true,
        options["reverse_amplitude"].toBool(),options["scene_aspect_mode"]=="sample_counts",
        float(options["scene_time_aspect"].toDouble(2)),float(options["scene_band_aspect"].toDouble(1))};
}

// Coordinates beyond the filled prefix are unknown, even when the surface has
// a fixed capacity. Never extrapolate seconds using GUI FPS or sample rate.
inline std::vector<double> spectrogramValidTimeTicks(const SpectrogramSceneGeometry& scene,size_t filled,int sections=5,bool integralPositions=false) {
    std::vector<double> ticks(size_t(std::clamp(sections,1,64)+1),std::numeric_limits<double>::quiet_NaN());
    if(!filled)return ticks;
    const double low=scene.xMin,high=std::min(double(scene.xMax),double(filled-1));
    if(high<low)return ticks;
    for(size_t i=0;i<ticks.size();++i) {
        const double fraction=double(i)/(ticks.size()-1);
        ticks[i]=low+(scene.reverseTime ? 1-fraction : fraction)*(high-low);
        if(integralPositions && high-low>=double(ticks.size()-1) && i && i+1<ticks.size())
            ticks[i]=std::clamp(std::round(ticks[i]),low,high);
    }
    return ticks;
}

// Density is a layout policy, deliberately independent of camera projection.
// Rotation, zoom and culling may hide labels, but never change grid divisions.
inline int spectrogramAxisSections(QSizeF viewport,QSizeF label,int axis) {
    const double budget=axis==2 ? viewport.height()*.65 : viewport.width()*(axis==0?.8:.5);
    const double spacing=axis==2 ? label.height()+14 : label.width()+16;
    if(!std::isfinite(budget) || !std::isfinite(spacing))return 1;
    int sections=std::clamp(int(std::floor(std::max(0.0,budget)/std::max(20.0,spacing))),1,64);
    if(axis==2 && viewport.height()>=220)sections=std::max(4,sections);
    return sections;
}

// Bound divisions by available positions. Physical amplitude spans prefer a
// 1/2/2.5/5 step, retaining exact range endpoints. Integer frame rounding is
// applied to actual tick indices, so prime capacities do not collapse density.
inline int spectrogramReadableSections(double span,int maximum,bool positions) {
    if(!std::isfinite(span) || span<=0)return 1;
    maximum=std::clamp(maximum,1,64);
    if(positions)return std::min(maximum,std::max(1,int(std::min(64.0,std::floor(span)))));
    const double raw=span/maximum,power=std::pow(10.0,std::floor(std::log10(raw)));
    for(double factor:{1.0,2.0,2.5,5.0,10.0}) {
        const double step=power*factor;
        if(step+raw*1e-9<raw)continue;
        const double count=span/step;
        if(count>=1 && count<=maximum && std::abs(count-std::round(count))<1e-5)
            return int(std::round(count));
    }
    return maximum;
}


// Rendering reduction retains the largest absolute sample in each disjoint
// bucket instead of dropping narrow spectral/time peaks between stride points.
struct SpectrogramPeakSample {size_t band=0,age=0;float value=0;};
template<class Matrix=std::vector<std::vector<float>>>
inline SpectrogramPeakSample spectrogramPeakSample(const Matrix& data,
    size_t bandBegin,size_t bandEnd,size_t ageBegin,size_t ageEnd,size_t capacity) {
    SpectrogramPeakSample peak{bandBegin,ageBegin,0};
    bool seen=false;
    for(size_t band=bandBegin;band<bandEnd;++band)
        for(size_t age=ageBegin;age<ageEnd;++age) {
            const float value=spectrogramHistorySample(data,band,age,capacity);
            if(!seen || std::abs(value)>std::abs(peak.value)){peak={band,age,value};seen=true;}
        }
    return peak;
}

// Match the horizontal cross-section to the visible direction of its 3D axis.
// Camera orientation and axis reversal both affect this projection. For an axis
// seen exactly edge-on, use top-to-bottom; coincident endpoints retain source order.
inline bool spectrogramSliceReversed(QVector4D first,QVector4D last) {
    if(first.w()<=0 || last.w()<=0)return false;
    const double dx=double(last.x())/last.w()-double(first.x())/first.w();
    const double dy=double(last.y())/last.w()-double(first.y())/first.w();
    return std::abs(dx)>1e-6 ? dx<0 : dy>1e-6;
}
