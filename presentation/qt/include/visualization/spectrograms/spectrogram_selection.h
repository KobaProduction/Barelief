#pragma once
#include <QPointF>
#include <QFontMetricsF>
#include <QTextLayout>
#include <QRectF>
#include <QSizeF>
#include <QVector>
#include <QStringList>
#include <visualization/ui_text.h>
#include <algorithm>
#include <cmath>
#include <limits>

struct SpectrogramSelectionBounds {int firstFrame,lastFrame,firstBand,lastBand;};
struct SpectrogramProjectedSample {QPointF point;double depth=0;bool visible=false;};

// Bounded screen-space search, followed by exact neighbouring sample search.
// Both cropped endpoints participate even when the dimensions exceed the grid.
template<class Project>
QPoint spectrogramNearestPoint(QPointF cursor,SpectrogramSelectionBounds bounds,Project project,
                              double maximumDistance=std::numeric_limits<double>::infinity()) {
    if(bounds.firstFrame>bounds.lastFrame || bounds.firstBand>bounds.lastBand)return {-1,-1};
    QPoint best{-1,-1};double distance=std::numeric_limits<double>::infinity(),depth=distance;
    const auto consider=[&](int frame,int band) {
        const auto candidate=project(frame,band);if(!candidate.visible)return;
        const auto delta=candidate.point-cursor;
        const double d=delta.x()*delta.x()+delta.y()*delta.y();
        if(d<distance || (d==distance && candidate.depth<depth)) {distance=d;depth=candidate.depth;best={frame,band};}
    };
    const auto scan=[&](SpectrogramSelectionBounds area,int frameStep,int bandStep) {
        for(int band=area.firstBand;;band=std::min(area.lastBand,band+bandStep)) {
            for(int frame=area.firstFrame;;frame=std::min(area.lastFrame,frame+frameStep)) {
                consider(frame,band);if(frame==area.lastFrame)break;
            }
            if(band==area.lastBand)break;
        }
    };
    const int frameStep=std::max(1,(bounds.lastFrame-bounds.firstFrame+126)/127);
    const int bandStep=std::max(1,(bounds.lastBand-bounds.firstBand+126)/127);
    scan(bounds,frameStep,bandStep);
    if(best.x()>=0)scan({std::max(bounds.firstFrame,best.x()-frameStep),std::min(bounds.lastFrame,best.x()+frameStep),
                        std::max(bounds.firstBand,best.y()-bandStep),std::min(bounds.lastBand,best.y()+bandStep)},1,1);
    return distance<=maximumDistance*maximumDistance?best:QPoint{-1,-1};
}

inline QStringList spectrogramSelectionRows(const QString& age,const QString& column,const QString& value,
                                          bool seconds,const QString& columnTitle,const QString& valueTitle,bool ageAxis=true,const QString& rowTitle={}) {
    const char* coordinate=columnTitle=="Frequency, Hz (Z)"?"Frequency (Hz)":
        columnTitle=="Frequency, Mel bands (Z)"?"Mel band":
        columnTitle=="Frequency bin (Z)"?"Frequency bin":
        columnTitle=="Sample (Z)"?"Sample":"Value index";
    const char* quantity=valueTitle=="Amplitude (Y)"?"Amplitude":valueTitle=="Frames (Y)"?"Frames":"Value";
    auto rowName=ui::text(rowTitle.toUtf8().constData());if(rowName.endsWith(" (X)"))rowName.chop(4);
    return {ageAxis?ui::text(seconds?"Time delay (s): %1":"Temporal frame: %1").arg(age):rowName+QStringLiteral(": ")+age,
            ui::text(coordinate)+QStringLiteral(": ")+column,
            ui::text(quantity)+QStringLiteral(": ")+value};
}

// Layout full strings, including long physical coordinates, without elision.
// Typical readouts occupy three lines; narrow viewports wrap rather than drop digits.
struct SpectrogramReadoutLayout {
    QFont font;
    QStringList lines;
    QVector<QPointF> baselines;
    QRectF bounds;
};
inline SpectrogramReadoutLayout spectrogramReadoutLayout(const QStringList& rows,QFont font,QSizeF viewport) {
    if(font.pointSizeF()>10)font.setPointSizeF(10);
    const qreal margin=std::min<qreal>(12,std::max<qreal>(0,viewport.width()/10));
    const qreal width=std::max<qreal>(1,viewport.width()-2*margin);
    const qreal top=std::min<qreal>(8,std::max<qreal>(0,viewport.height()/10));
    SpectrogramReadoutLayout result;
    for(;;) {
        result={};result.font=font;QFontMetricsF metrics(font);qreal y=top,maxWidth=0;
        for(const auto& row:rows) {
            QTextLayout layout(row,font);QTextOption option;
            option.setWrapMode(QTextOption::WrapAtWordBoundaryOrAnywhere);layout.setTextOption(option);
            layout.beginLayout();
            for(;;) {
                auto line=layout.createLine();if(!line.isValid())break;
                line.setLineWidth(width);
                result.lines.push_back(row.mid(line.textStart(),line.textLength()));
                result.baselines.push_back({margin,y+metrics.ascent()});
                maxWidth=std::max(maxWidth,line.naturalTextWidth());y+=metrics.height();
            }
            layout.endLayout();
        }
        result.bounds={margin,top,maxWidth,y-top};
        if(y<=viewport.height()-top || font.pointSizeF()<=1 || font.pointSizeF()<0)break;
        font.setPointSizeF(std::max<qreal>(1,font.pointSizeF()*.9));
    }
    return result;
}
