#pragma once
#include <QVector3D>
#include <algorithm>
#include <vector>
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
    return best;
}

inline float spectrogramHistorySample(const std::vector<std::vector<float>> &data,size_t band,size_t age,size_t capacity) {
    if(band>=data.size()) return 0;
    const auto &row=data[band];
    return age>=std::min(capacity,row.size()) ? 0 : row[row.size()-1-age];
}

// AspectRatio=2, automatic horizontal aspect, increasing X for frame age;
// Y range 0..2. Axis ranges describe sample coordinates, not sample counts-1.
struct SpectrogramSceneGeometry {
    float xMin, xMax, zMin, zMax, scaleX, scaleZ;
    float yMin,yMax;
    bool clipY;
    bool reverseTime,reverseBands,reverseAmplitude;
    SpectrogramSceneGeometry(size_t history, size_t bands, const int *ranges,bool reverse_time=false,bool reverse_bands=false,
                             float y_min=0,float y_max=2,bool clip_y=false,bool reverse_amplitude=false)
        : yMin(y_min),yMax(y_max),clipY(clip_y),reverseTime(reverse_time),reverseBands(reverse_bands),reverseAmplitude(reverse_amplitude) {
        const float oldest=history ? float(history-1) : 0;
        xMin = oldest * ranges[0] / 100.f; xMax = oldest * ranges[1] / 100.f;
        zMin = bands * ranges[2] / 100.f; zMax = bands * ranges[3] / 100.f;
        const float xRange = std::max(.001f, xMax-xMin), zRange = std::max(.001f, zMax-zMin);
        scaleX = 2 * xRange / std::max(xRange,zRange);
        scaleZ = 2 * zRange / std::max(xRange,zRange);
    }
    QVector3D point(float frame, float band, float value) const {
        const float height=valueFraction(value);
        return {(reverseTime ? -1.f : 1.f)*scaleX * (-1 + 2*(frame-xMin)/std::max(.001f,xMax-xMin)),
                (reverseAmplitude ? -1.f : 1.f)*(2*(clipY ? std::clamp(height,0.f,1.f) : height)-1),
                (reverseBands ? -1.f : 1.f)*scaleZ * (1 - 2*(band-zMin)/std::max(.001f,zMax-zMin))};
    }
    float timeTick(float fraction) const {return xMin+(reverseTime ? 1-fraction : fraction)*(xMax-xMin);}
    float bandTick(float fraction) const {return zMin+(reverseBands ? 1-fraction : fraction)*(zMax-zMin);}
    float valueFraction(float value) const {return (value-yMin)/std::max(1e-6f,yMax-yMin);}
    float signalTick(float fraction) const {return yMin+(reverseAmplitude ? 1-fraction : fraction)*(yMax-yMin);}
    float verticalFraction(float value) const {const float f=valueFraction(value);return reverseAmplitude ? 1-f : f;}
};
