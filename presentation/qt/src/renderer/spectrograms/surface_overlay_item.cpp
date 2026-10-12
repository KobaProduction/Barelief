#include <visualization/quick3d/surface_overlay_item.h>
#include <visualization/spectrograms/spectrogram_graph.h>
#include <core/performance.h>
#include <QPainter>
#include <QFontMetricsF>
#include <QLinearGradient>
#include <QPainterPathStroker>
#include <QPaintEngine>
#include <algorithm>

SurfaceOverlayItem::SurfaceOverlayItem(QQuickItem* parent):QQuickPaintedItem(parent){
    setAntialiasing(true);
    // Qt 6.9+ paints this target directly with the active OpenGL backend.
    // Qt retains its image fallback for software/other graphics backends.
    setRenderTarget(QQuickPaintedItem::FramebufferObject);
    setAcceptedMouseButtons(Qt::NoButton);
}
void SurfaceOverlayItem::setSourceModel(QObject* source){
    auto* model=qobject_cast<SpectrogramGraph*>(source);
    if(source_model_==model)return;
    disconnect(source_changed_);disconnect(source_destroyed_);source_model_=model;
    if(model){
        source_changed_=connect(model,&SpectrogramGraph::overlay_changed,this,[this,model]{setCommands(model->overlay().value("commands").toList());});
        source_destroyed_=connect(model,&QObject::destroyed,this,[this]{setCommands({});emit sourceModelChanged();});
        setCommands(model->overlay().value("commands").toList());
    }else setCommands({});
    emit sourceModelChanged();
}
SurfaceOverlayItem::Command SurfaceOverlayItem::decode(const QVariantMap& value){
    Command c;
    c.pen=QPen(QColor(value.value("pen").toString()),value.value("penWidth").toDouble());
    if(value.value("noPen").toBool())c.pen=Qt::NoPen;
    const auto brush=value.value("brush").toMap();
    if(!brush.value("none").toBool()){
        if(brush.contains("stops")){
            QLinearGradient gradient(brush.value("start").toPointF(),brush.value("end").toPointF());
            for(const auto& stop:brush.value("stops").toList()){
                const auto s=stop.toMap();gradient.setColorAt(s.value("position").toDouble(),QColor(s.value("color").toString()));
            }
            c.brush=gradient;
        }else c.brush=QColor(brush.value("color").toString());
    }else c.brush=Qt::NoBrush;
    c.font=QFont(value.value("font").toString());
    c.font.setPixelSize(std::max(1,value.value("fontPixels").toInt()));c.font.setBold(value.value("fontBold").toBool());
    const auto t=value.value("transform").toList();
    if(t.size()==6)c.transform=QTransform(t[0].toDouble(),t[1].toDouble(),t[2].toDouble(),t[3].toDouble(),t[4].toDouble(),t[5].toDouble());
    c.clip=value.value("clip").toRectF();
    c.rect={value.value("x").toDouble(),value.value("y").toDouble(),value.value("width").toDouble(),value.value("height").toDouble()};
    const auto kind=value.value("kind").toString();
    c.isText=kind=="text";
    if(c.isText){
        c.text=value.value("text").toString();c.flags=value.value("flags").toInt();c.baseline=value.value("baseline").toBool();
        const QFontMetricsF metrics(c.font);
        c.bounds=c.baseline?metrics.boundingRect(c.text).translated(c.rect.topLeft()):metrics.boundingRect(c.rect,c.flags,c.text);
    }else{
        if(kind=="rect")c.path.addRect(c.rect);
        else if(kind=="roundedRect")c.path.addRoundedRect(c.rect,value.value("radiusX").toDouble(),value.value("radiusY").toDouble());
        else if(kind=="ellipse")c.path.addEllipse(c.rect);
        else if(kind=="line" || kind=="polygon"){
            const auto points=value.value("points").toList();
            if(points.size()>=2){c.path.moveTo(points[0].toDouble(),points[1].toDouble());
                for(qsizetype i=2;i+1<points.size();i+=2)c.path.lineTo(points[i].toDouble(),points[i+1].toDouble());
                if(kind=="polygon")c.path.closeSubpath();}
        }
        c.bounds=c.path.boundingRect();
    }
    c.bounds=c.transform.mapRect(c.bounds);
    if(!c.isText && c.pen.style()!=Qt::NoPen){
        QPainterPathStroker stroker;stroker.setWidth(std::max(1.,c.pen.widthF()));
        stroker.setCapStyle(c.pen.capStyle());stroker.setJoinStyle(c.pen.joinStyle());stroker.setMiterLimit(c.pen.miterLimit());
        c.bounds=c.bounds.united(c.transform.map(stroker.createStroke(c.path)).boundingRect());
    }
    c.bounds=c.bounds.adjusted(-4,-4,4,4);
    if(!c.clip.isEmpty())c.bounds=c.bounds.intersected(c.clip.adjusted(-1,-1,1,1));
    return c;
}
void SurfaceOverlayItem::setCommands(const QVariantList& commands){
    if(commands_==commands)return;
    std::vector<Command> next;next.reserve(size_t(commands.size()));
    QRectF damage;
    for(qsizetype i=0;i<commands.size();++i){
        const bool changed=i>=commands_.size() || commands[i]!=commands_[i];
        next.push_back(changed?decode(commands[i].toMap()):decoded_[size_t(i)]);
        if(changed){damage=damage.united(next.back().bounds);if(size_t(i)<decoded_.size())damage=damage.united(decoded_[size_t(i)].bounds);}
    }
    for(size_t i=size_t(commands.size());i<decoded_.size();++i)damage=damage.united(decoded_[i].bounds);
    commands_=commands;decoded_=std::move(next);
    if(!damage.isEmpty())update(damage.toAlignedRect());
    emit commandsChanged();
}
void SurfaceOverlayItem::paint(QPainter* painter){
    core::perf::Scope trace(core::perf::Stage::QuickSurfaceOverlayPaint);
    paint_engine_type_.store(painter->paintEngine()?int(painter->paintEngine()->type()):-1,std::memory_order_relaxed);
    const auto damage=painter->hasClipping()?painter->clipBoundingRect():boundingRect();
    for(const auto& c:decoded_){
        if(!c.bounds.intersects(damage))continue;
        painter->save();
        // Keep QQuickPaintedItem's damage clip while applying display-list clips.
        if(!c.clip.isEmpty())painter->setClipRect(c.clip,Qt::IntersectClip);
        painter->setWorldTransform(c.transform,true);painter->setPen(c.pen);painter->setBrush(c.brush);
        if(c.isText){painter->setFont(c.font);if(c.baseline)painter->drawText(c.rect.topLeft(),c.text);else painter->drawText(c.rect,c.flags,c.text);}
        else painter->drawPath(c.path);
        painter->restore();
    }
}
