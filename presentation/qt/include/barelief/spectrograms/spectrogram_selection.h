#pragma once
#include <QPointF>
#include <QFontMetricsF>
#include <QTextLayout>
#include <QRectF>
#include <QSizeF>
#include <QVector>
#include <QStringList>
#include <barelief/ui_text.h>
#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>

struct SpectrogramSelectionBounds {int firstFrame,lastFrame,firstBand,lastBand;};
struct SpectrogramProjectedSample {QPointF point;double depth=0;bool visible=false;};

template<class Project>
struct SpectrogramNearestSearch {
    QPointF cursor;
    Project project;
    QPoint best{-1,-1};
    double distance=std::numeric_limits<double>::infinity();
    double depth=std::numeric_limits<double>::infinity();

    void consider(int frame,int band) {
        const auto candidate=project(frame,band);
        if(!candidate.visible)return;
        const auto delta=candidate.point-cursor;
        const double d=delta.x()*delta.x()+delta.y()*delta.y();
        if(d<distance || (d==distance && candidate.depth<depth)) {
            distance=d;
            depth=candidate.depth;
            best={frame,band};
        }
    }

    void scan(SpectrogramSelectionBounds area,int frameStep,int bandStep) {
        for(int band=area.firstBand;;band=std::min(area.lastBand,band+bandStep)) {
            for(int frame=area.firstFrame;;frame=std::min(area.lastFrame,frame+frameStep)) {
                consider(frame,band);
                if(frame==area.lastFrame)break;
            }
            if(band==area.lastBand)break;
        }
    }
};

// Bounded screen-space search, followed by exact neighbouring sample search.
// Both cropped endpoints participate even when the dimensions exceed the grid.
template<class Project>
QPoint spectrogramNearestPoint(QPointF cursor,SpectrogramSelectionBounds bounds,Project project,
                              double maximumDistance=std::numeric_limits<double>::infinity()) {
    if(bounds.firstFrame>bounds.lastFrame || bounds.firstBand>bounds.lastBand)return {-1,-1};
    SpectrogramNearestSearch<Project> search{cursor,std::move(project)};
    const int frameStep=std::max(1,(bounds.lastFrame-bounds.firstFrame+126)/127);
    const int bandStep=std::max(1,(bounds.lastBand-bounds.firstBand+126)/127);
    search.scan(bounds,frameStep,bandStep);
    if(search.best.x()>=0)search.scan({std::max(bounds.firstFrame,search.best.x()-frameStep),std::min(bounds.lastFrame,search.best.x()+frameStep),
                        std::max(bounds.firstBand,search.best.y()-bandStep),std::min(bounds.lastBand,search.best.y()+bandStep)},1,1);
    return search.distance<=maximumDistance*maximumDistance?search.best:QPoint{-1,-1};
}

inline const char* spectrogramCoordinateName(const QString& columnTitle) {
    return columnTitle=="Frequency, Hz (Z)"?"Frequency (Hz)":
        columnTitle=="Frequency, Mel bands (Z)"?"Mel band":
        columnTitle=="Frequency bin (Z)"?"Frequency bin":
        columnTitle=="Sample (Z)"?"Sample":"Value index";
}
inline QString spectrogramAgeRow(const QString& age,bool seconds,bool ageAxis,const QString& rowTitle) {
    auto rowName=ui::text(rowTitle.toUtf8().constData());if(rowName.endsWith(" (X)"))rowName.chop(4);
    return ageAxis?ui::text(seconds?"Time delay (s): %1":"Temporal frame: %1").arg(age):rowName+QStringLiteral(": ")+age;
}
inline QStringList spectrogramSelectionRows(const QString& age,const QString& column,const QString& value,
                                          bool seconds,const QString& columnTitle,const QString& valueTitle,bool ageAxis=true,const QString& rowTitle={}) {
    const char* quantity=valueTitle=="Amplitude (Y)"?"Amplitude":valueTitle=="Frames (Y)"?"Frames":"Value";
    return {spectrogramAgeRow(age,seconds,ageAxis,rowTitle),
            ui::text(spectrogramCoordinateName(columnTitle))+QStringLiteral(": ")+column,
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
inline void appendSpectrogramReadoutRow(SpectrogramReadoutLayout& result,const QString& row,const QFont& font,
                                        qreal width,qreal margin,const QFontMetricsF& metrics,qreal& y,qreal& maxWidth) {
    QTextLayout layout(row,font);
    QTextOption option;
    option.setWrapMode(QTextOption::WrapAtWordBoundaryOrAnywhere);
    layout.setTextOption(option);
    layout.beginLayout();
    for(;;) {
        auto line=layout.createLine();
        if(!line.isValid())break;
        line.setLineWidth(width);
        result.lines.push_back(row.mid(line.textStart(),line.textLength()));
        result.baselines.push_back({margin,y+metrics.ascent()});
        maxWidth=std::max(maxWidth,line.naturalTextWidth());
        y+=metrics.height();
    }
    layout.endLayout();
}
inline SpectrogramReadoutLayout spectrogramReadoutAtFont(const QStringList& rows,const QFont& font,
                                                        qreal width,qreal margin,qreal top) {
    SpectrogramReadoutLayout result;
    result.font=font;
    QFontMetricsF metrics(font);
    qreal y=top,maxWidth=0;
    for(const auto& row:rows)appendSpectrogramReadoutRow(result,row,font,width,margin,metrics,y,maxWidth);
    result.bounds={margin,top,maxWidth,y-top};
    return result;
}
inline SpectrogramReadoutLayout spectrogramReadoutLayout(const QStringList& rows,QFont font,QSizeF viewport) {
    if(font.pointSizeF()>10)font.setPointSizeF(10);
    const qreal margin=std::min<qreal>(12,std::max<qreal>(0,viewport.width()/10));
    const qreal width=std::max<qreal>(1,viewport.width()-2*margin);
    const qreal top=std::min<qreal>(8,std::max<qreal>(0,viewport.height()/10));
    for(;;) {
        auto result=spectrogramReadoutAtFont(rows,font,width,margin,top);
        if(result.bounds.bottom()<=viewport.height()-top || font.pointSizeF()<=1 || font.pointSizeF()<0)return result;
        font.setPointSizeF(std::max<qreal>(1,font.pointSizeF()*.9));
    }
}
