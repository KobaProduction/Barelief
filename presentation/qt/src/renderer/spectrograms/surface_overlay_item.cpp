#include <barelief/quick3d/surface_overlay_item.h>
#include <barelief/spectrograms/spectrogram_graph.h>
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
QBrush SurfaceOverlayItem::decodeBrush(const QVariantMap& brush){
    if(brush.value("none").toBool())return QBrush(Qt::NoBrush);
    if(!brush.contains("stops"))return QBrush(QColor(brush.value("color").toString()));
    QLinearGradient gradient(brush.value("start").toPointF(),brush.value("end").toPointF());
    const auto stops=brush.value("stops").toList();
    for(const auto& stop:stops){
        const auto value=stop.toMap();gradient.setColorAt(value.value("position").toDouble(),QColor(value.value("color").toString()));
    }
    return QBrush(gradient);
}
QFont SurfaceOverlayItem::decodeFont(const QVariantMap& value){
    QFont font(value.value("font").toString());
    font.setPixelSize(std::max(1,value.value("fontPixels").toInt()));font.setBold(value.value("fontBold").toBool());
    return font;
}
QPainterPath SurfaceOverlayItem::decodePath(const QString& kind,const QVariantMap& value,const QRectF& rect){
    QPainterPath path;
    if(kind=="rect")path.addRect(rect);
    else if(kind=="roundedRect")path.addRoundedRect(rect,value.value("radiusX").toDouble(),value.value("radiusY").toDouble());
    else if(kind=="ellipse")path.addEllipse(rect);
    else if(kind=="line" || kind=="polygon")appendPointPath(path,kind,value);
    return path;
}
void SurfaceOverlayItem::appendPointPath(QPainterPath& path,const QString& kind,const QVariantMap& value){
    const auto points=value.value("points").toList();
    if(points.size()<2)return;
    path.moveTo(points[0].toDouble(),points[1].toDouble());
    for(qsizetype index=2;index+1<points.size();index+=2)
        path.lineTo(points[index].toDouble(),points[index+1].toDouble());
    if(kind=="polygon")path.closeSubpath();
}
void SurfaceOverlayItem::decodeGeometry(Command& command,const QVariantMap& value){
    const auto kind=value.value("kind").toString();command.isText=kind=="text";
    if(!command.isText){command.path=decodePath(kind,value,command.rect);command.bounds=command.path.boundingRect();return;}
    command.text=value.value("text").toString();command.flags=value.value("flags").toInt();command.baseline=value.value("baseline").toBool();
    const QFontMetricsF metrics(command.font);
    command.bounds=command.baseline?metrics.boundingRect(command.text).translated(command.rect.topLeft()):metrics.boundingRect(command.rect,command.flags,command.text);
}
QRectF SurfaceOverlayItem::strokeBounds(const Command& command){
    QPainterPathStroker stroker;stroker.setWidth(std::max(1.,command.pen.widthF()));
    stroker.setCapStyle(command.pen.capStyle());stroker.setJoinStyle(command.pen.joinStyle());stroker.setMiterLimit(command.pen.miterLimit());
    return command.transform.map(stroker.createStroke(command.path)).boundingRect();
}

SurfaceOverlayItem::Command SurfaceOverlayItem::decode(const QVariantMap& value){
    Command command;
    command.pen=QPen(QColor(value.value("pen").toString()),value.value("penWidth").toDouble());
    if(value.value("noPen").toBool())command.pen=Qt::NoPen;
    command.brush=decodeBrush(value.value("brush").toMap());command.font=decodeFont(value);
    command.transform=decodeTransform(value);
    command.clip=value.value("clip").toRectF();
    command.rect={value.value("x").toDouble(),value.value("y").toDouble(),value.value("width").toDouble(),value.value("height").toDouble()};
    decodeGeometry(command,value);completeBounds(command);
    return command;
}
QTransform SurfaceOverlayItem::decodeTransform(const QVariantMap& value){
    const auto transform=value.value("transform").toList();
    if(transform.size()!=6)return {};
    return QTransform(transform[0].toDouble(),transform[1].toDouble(),transform[2].toDouble(),
                      transform[3].toDouble(),transform[4].toDouble(),transform[5].toDouble());
}
void SurfaceOverlayItem::completeBounds(Command& command){
    command.bounds=command.transform.mapRect(command.bounds);
    if(!command.isText && command.pen.style()!=Qt::NoPen)
        command.bounds=command.bounds.united(strokeBounds(command));
    command.bounds=command.bounds.adjusted(-4,-4,4,4);
    if(!command.clip.isEmpty())command.bounds=command.bounds.intersected(command.clip.adjusted(-1,-1,1,1));
}
void SurfaceOverlayItem::appendCommand(std::vector<Command>& next,QRectF& damage,
                                       const QVariantList& commands,qsizetype index){
    const bool changed=index>=commands_.size() || commands[index]!=commands_[index];
    next.push_back(changed?decode(commands[index].toMap()):decoded_[size_t(index)]);
    if(!changed)return;
    damage=damage.united(next.back().bounds);
    if(size_t(index)<decoded_.size())damage=damage.united(decoded_[size_t(index)].bounds);
}
QRectF SurfaceOverlayItem::removedCommandBounds(qsizetype count) const {
    QRectF damage;
    for(size_t index=size_t(count);index<decoded_.size();++index)damage=damage.united(decoded_[index].bounds);
    return damage;
}
void SurfaceOverlayItem::setCommands(const QVariantList& commands){
    if(commands_==commands)return;
    std::vector<Command> next;next.reserve(size_t(commands.size()));
    QRectF damage;
    for(qsizetype i=0;i<commands.size();++i)appendCommand(next,damage,commands,i);
    damage=damage.united(removedCommandBounds(commands.size()));
    commands_=commands;decoded_=std::move(next);
    if(!damage.isEmpty())update(damage.toAlignedRect());
    emit commandsChanged();
}
void SurfaceOverlayItem::paintCommand(QPainter* painter,const Command& command){
    painter->save();
    // Preserve QQuickPaintedItem's damage clip when applying the display list.
    if(!command.clip.isEmpty())painter->setClipRect(command.clip,Qt::IntersectClip);
    painter->setWorldTransform(command.transform,true);painter->setPen(command.pen);painter->setBrush(command.brush);
    if(command.isText){painter->setFont(command.font);if(command.baseline)painter->drawText(command.rect.topLeft(),command.text);else painter->drawText(command.rect,command.flags,command.text);}
    else painter->drawPath(command.path);
    painter->restore();
}

void SurfaceOverlayItem::paint(QPainter* painter){
    core::perf::Scope trace(core::perf::Stage::QuickSurfaceOverlayPaint);
    paint_engine_type_.store(painter->paintEngine()?int(painter->paintEngine()->type()):-1,std::memory_order_relaxed);
    QMetaObject::invokeMethod(this,[this]{emit paintTelemetryChanged();},Qt::QueuedConnection);
    const auto damage=painter->hasClipping()?painter->clipBoundingRect():boundingRect();
    for(const auto& command:decoded_)if(command.bounds.intersects(damage))paintCommand(painter,command);
}
