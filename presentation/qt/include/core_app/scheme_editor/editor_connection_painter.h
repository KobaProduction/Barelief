#pragma once
#include "numeric_editor_model.h"
#include "barelief/ui_text.h"
#include <QtNodes/internal/DefaultConnectionPainter.hpp>
#include <QtNodes/internal/ConnectionGraphicsObject.hpp>
#include <QApplication>
#include <QFontMetricsF>
#include <core/telemetry.h>

class EditorConnectionPainter final : public QtNodes::DefaultConnectionPainter {
public:
    QPainterPath getPainterStroke(const QtNodes::ConnectionGraphicsObject& wire) const override {
        auto stroke=DefaultConnectionPainter::getPainterStroke(wire);
        const auto* model=dynamic_cast<const NumericEditorGraphModel*>(&wire.graphModel());
        if(model && !wire.connectionState().requiresPort() && model->connection_visual_state(wire.connectionId())){
            const auto text=badge_text(model->connection_visual_state(wire.connectionId()));
            stroke.addRect(badge_bounds(connection_curve(wire).pointAtPercent(.5),text));
        }
        return stroke;
    }
    void paint(QPainter* painter,const QtNodes::ConnectionGraphicsObject& wire) const override {
        BARELIEF_PROFILE_FUNCTION();
        const auto* model=dynamic_cast<const NumericEditorGraphModel*>(&wire.graphModel());
        if(!model || wire.connectionState().requiresPort() || wire.connectionState().frozen()){DefaultConnectionPainter::paint(painter,wire);return;}
        paint_ready_wire(*painter,wire,model->connection_visual_state(wire.connectionId()));
    }
private:
    static void paint_ready_wire(QPainter& target,const QtNodes::ConnectionGraphicsObject& wire,int state) {
        auto* painter=&target;
        const auto path=connection_curve(wire);
        painter->save();painter->setRenderHint(QPainter::Antialiasing);painter->setBrush(Qt::NoBrush);
        if(wire.isSelected() || wire.connectionState().hovered())paint_halo(*painter,path);
        painter->setPen(connection_pen(state));painter->drawPath(path);
        if(state)paint_badge(*painter,path.pointAtPercent(.5),state);
        painter->restore();
    }
    static QPainterPath connection_curve(const QtNodes::ConnectionGraphicsObject& wire) {
        const auto controls=wire.pointsC1C2();
        QPainterPath path(wire.out());path.cubicTo(controls.first,controls.second,wire.in());return path;
    }
    static QFont badge_font() {auto font=QApplication::font();font.setPointSizeF(8);return font;}
    static QString badge_text(int state) {return ui::text(state==1?"↔ Convert":state==2?"! Risk":"× Error");}
    static QRectF badge_bounds(const QPointF& point,const QString& text) {
        const QFontMetricsF metrics(badge_font());const auto width=metrics.horizontalAdvance(text)+10;
        return {point.x()-width/2,point.y()-metrics.height()/2-2,width,metrics.height()+4};
    }
    static QPen connection_pen(int state) {
        QPen pen(QApplication::palette().color(state==1?QPalette::Link:QPalette::Text),state?2.5:1.8);pen.setCosmetic(true);
        if(state==1)pen.setStyle(Qt::DashLine);
        else if(state==2)pen.setStyle(Qt::DashDotLine);
        else if(state==3)pen.setStyle(Qt::DotLine);
        return pen;
    }
    static void paint_halo(QPainter& painter,const QPainterPath& path) {
        QPen pen(QApplication::palette().color(QPalette::Highlight),6);pen.setCosmetic(true);painter.setPen(pen);painter.drawPath(path);
    }
    static void paint_badge(QPainter& painter,const QPointF& point,int state) {
        const auto text=badge_text(state);const auto box=badge_bounds(point,text);const auto palette=QApplication::palette();
        painter.setFont(badge_font());painter.setPen(QPen(palette.color(QPalette::Text),1));painter.setBrush(palette.color(QPalette::Base));
        painter.drawRoundedRect(box,3,3);painter.drawText(box,Qt::AlignCenter,text);
    }
};
