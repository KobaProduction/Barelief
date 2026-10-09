#pragma once
#include "numeric_editor_model.h"
#include "visualization/ui_text.h"
#include <QtNodes/internal/DefaultConnectionPainter.hpp>
#include <QtNodes/internal/ConnectionGraphicsObject.hpp>
#include <QApplication>
#include <QFontMetricsF>

class EditorConnectionPainter final : public QtNodes::DefaultConnectionPainter {
public:
    QPainterPath getPainterStroke(const QtNodes::ConnectionGraphicsObject& wire) const override {
        auto stroke=DefaultConnectionPainter::getPainterStroke(wire);
        const auto* model=dynamic_cast<const NumericEditorGraphModel*>(&wire.graphModel());
        if(model && !wire.connectionState().requiresPort() && model->connection_visual_state(wire.connectionId())){
            QPainterPath curve(wire.out());const auto controls=wire.pointsC1C2();curve.cubicTo(controls.first,controls.second,wire.in());
            const auto state=model->connection_visual_state(wire.connectionId());auto font=QApplication::font();font.setPointSizeF(8);const QFontMetricsF metrics(font);const auto point=curve.pointAtPercent(.5);
            const auto text=ui::text(state==1?"↔ Convert":state==2?"! Risk":"× Error");const auto width=metrics.horizontalAdvance(text)+10;
            stroke.addRect(QRectF(point.x()-width/2,point.y()-metrics.height()/2-2,width,metrics.height()+4));
        }
        return stroke;
    }
    void paint(QPainter* painter,const QtNodes::ConnectionGraphicsObject& wire) const override {
        const auto* model=dynamic_cast<const NumericEditorGraphModel*>(&wire.graphModel());
        if(!model || wire.connectionState().requiresPort() || wire.connectionState().frozen()){DefaultConnectionPainter::paint(painter,wire);return;}
        const int state=model->connection_visual_state(wire.connectionId());
        const auto palette=QApplication::palette();const auto controls=wire.pointsC1C2();QPainterPath path(wire.out());path.cubicTo(controls.first,controls.second,wire.in());
        painter->save();painter->setRenderHint(QPainter::Antialiasing);painter->setBrush(Qt::NoBrush);
        if(wire.isSelected() || wire.connectionState().hovered()) {QPen halo(palette.color(QPalette::Highlight),6);halo.setCosmetic(true);painter->setPen(halo);painter->drawPath(path);}
        QPen pen(palette.color(state==1?QPalette::Link:QPalette::Text),state?2.5:1.8);pen.setCosmetic(true);
        if(state==1)pen.setStyle(Qt::DashLine);else if(state==2)pen.setStyle(Qt::DashDotLine);else if(state==3)pen.setStyle(Qt::DotLine);painter->setPen(pen);painter->drawPath(path);
        if(state){const auto mid=path.pointAtPercent(.5);auto font=QApplication::font();font.setPointSizeF(8);painter->setFont(font);
            const auto text=ui::text(state==1?"↔ Convert":state==2?"! Risk":"× Error");const QFontMetricsF metrics(font);
            const QRectF box(mid.x()-metrics.horizontalAdvance(text)/2-5,mid.y()-metrics.height()/2-2,metrics.horizontalAdvance(text)+10,metrics.height()+4);
            painter->setPen(QPen(palette.color(QPalette::Text),1));painter->setBrush(palette.color(QPalette::Base));painter->drawRoundedRect(box,3,3);painter->drawText(box,Qt::AlignCenter,text);
        }
        painter->restore();
    }
};
