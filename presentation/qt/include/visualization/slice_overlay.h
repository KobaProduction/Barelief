#pragma once
#include "visualization/spectrograms/spectrogram_scene_geometry.h"

// Shared interaction state for both cross-sections. Coordinates are logical pixels.
class SliceOverlay {
public:
    QPointF position{1,1};
    QSizeF preferredSize;
    bool active=false;
    QRectF rect(QSizeF viewport) const {
        const auto available=spectrogramSlicePanel(viewport,{0,0});
        if(!preferredSize.isValid()) return spectrogramSlicePanel(viewport,position);
        const double padding=available.left();
        const QSizeF extent(std::max(0.0,viewport.width()-2*padding),std::max(0.0,viewport.height()-2*padding));
        const QSizeF size(std::min(extent.width(),std::max(180.0,preferredSize.width())),
                          std::min(extent.height(),std::max(120.0,preferredSize.height())));
        return {padding+std::clamp(position.x(),0.0,1.0)*(extent.width()-size.width()),
                padding+std::clamp(position.y(),0.0,1.0)*(extent.height()-size.height()),size.width(),size.height()};
    }
    Qt::Edges edges(QSizeF viewport,QPointF point) const {
        const auto panel=rect(viewport);
        if(!panel.contains(point)) return {};
        Qt::Edges result;
        if(point.x()-panel.left()<7) result|=Qt::LeftEdge;
        else if(panel.right()-point.x()<7) result|=Qt::RightEdge;
        if(point.y()-panel.top()<7) result|=Qt::TopEdge;
        else if(panel.bottom()-point.y()<7) result|=Qt::BottomEdge;
        return result;
    }
    Qt::CursorShape cursor(QSizeF viewport,QPointF point) const {
        const auto edge=edges(viewport,point);
        if((edge.testFlag(Qt::LeftEdge)&&edge.testFlag(Qt::TopEdge)) ||
           (edge.testFlag(Qt::RightEdge)&&edge.testFlag(Qt::BottomEdge))) return Qt::SizeFDiagCursor;
        if((edge.testFlag(Qt::RightEdge)&&edge.testFlag(Qt::TopEdge)) ||
           (edge.testFlag(Qt::LeftEdge)&&edge.testFlag(Qt::BottomEdge))) return Qt::SizeBDiagCursor;
        if(edge & (Qt::LeftEdge|Qt::RightEdge)) return Qt::SizeHorCursor;
        if(edge & (Qt::TopEdge|Qt::BottomEdge)) return Qt::SizeVerCursor;
        return rect(viewport).contains(point) ? Qt::SizeAllCursor : Qt::ArrowCursor;
    }
    bool begin(QSizeF viewport,QPointF point) {
        start=rect(viewport);if(!start.contains(point)) return false;
        anchor=point;resizeEdges=edges(viewport,point);active=true;return true;
    }
    void move(QSizeF viewport,QPointF point) {
        if(!active) return;
        auto panel=start;const auto delta=point-anchor;
        const double padding=spectrogramSlicePanel(viewport,{0,0}).left();
        const QRectF bounds(padding,padding,std::max(0.0,viewport.width()-2*padding),std::max(0.0,viewport.height()-2*padding));
        const double minWidth=std::min(180.0,bounds.width()),minHeight=std::min(120.0,bounds.height());
        if(resizeEdges) {
            if(resizeEdges.testFlag(Qt::LeftEdge)) panel.setLeft(std::clamp(start.left()+delta.x(),bounds.left(),start.right()-minWidth));
            if(resizeEdges.testFlag(Qt::RightEdge)) panel.setRight(std::clamp(start.right()+delta.x(),start.left()+minWidth,bounds.right()));
            if(resizeEdges.testFlag(Qt::TopEdge)) panel.setTop(std::clamp(start.top()+delta.y(),bounds.top(),start.bottom()-minHeight));
            if(resizeEdges.testFlag(Qt::BottomEdge)) panel.setBottom(std::clamp(start.bottom()+delta.y(),start.top()+minHeight,bounds.bottom()));
            preferredSize=panel.size();
        } else {
            panel.translate(delta);
            panel.moveLeft(std::clamp(panel.left(),bounds.left(),bounds.right()-panel.width()));
            panel.moveTop(std::clamp(panel.top(),bounds.top(),bounds.bottom()-panel.height()));
        }
        const auto span=bounds.size()-panel.size();
        position={span.width()>0 ? (panel.left()-padding)/span.width() : 0,
                  span.height()>0 ? (panel.top()-padding)/span.height() : 0};
    }
    void end() {active=false;}
private:
    QRectF start;
    QPointF anchor;
    Qt::Edges resizeEdges;
};
