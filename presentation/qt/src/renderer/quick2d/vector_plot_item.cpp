#include <visualization/quick2d/vector_plot_item.h>
#include <visualization/quick2d/plot_model.h>
#include <core/performance.h>
#include <QPainter>
#include <QPainterPath>
#include <QPaintEngine>
#include <QOpenGLContext>
#include <QOpenGLFunctions>
#include <QSGGeometryNode>
#include <QSGVertexColorMaterial>
#include <QQuickWindow>
#include <QSGRendererInterface>
#include <algorithm>
#include <cmath>
#include <utility>

VectorGeometryItem::VectorGeometryItem(QQuickItem* parent):VectorPlotItem(parent){setClip(true);}
QSGNode* VectorGeometryItem::updatePaintNode(QSGNode* oldNode,UpdatePaintNodeData* data){
    // Qt's software adaptation retains the existing raster implementation.
    if(window() && window()->rendererInterface()->graphicsApi()==QSGRendererInterface::Software)
        return QQuickPaintedItem::updatePaintNode(oldNode,data);
    core::perf::Scope trace(core::perf::Stage::Quick2dGeometry);
    auto* node=static_cast<QSGGeometryNode*>(oldNode);
    if(!node){
        node=new QSGGeometryNode;
        auto* geometry=new QSGGeometry(QSGGeometry::defaultAttributes_ColoredPoint2D(),0);
        geometry->setDrawingMode(QSGGeometry::DrawTriangles);
        geometry->setVertexDataPattern(QSGGeometry::DynamicPattern);
        geometry->setIndexDataPattern(QSGGeometry::DynamicPattern);
        node->setGeometry(geometry);node->setFlag(QSGNode::OwnsGeometry);
        node->setMaterial(new QSGVertexColorMaterial);node->setFlag(QSGNode::OwnsMaterial);
    }
    QVector<QSGGeometry::ColoredPoint2D> vertices;
    vertices.reserve(int(std::min<qsizetype>(points_.size(),4098))*18+512);
    // Share the four edges of an antialiased segment instead of uploading
    // eighteen repeated vertices. Keep 16-bit indices within a proven budget;
    // oversized external presentations and point clouds retain direct triangles.
    const bool indexed=style_!="points"&&points_.size()!=1&&points_.size()<=4098&&x_ticks_.size()+y_ticks_.size()<=1024;
    QVector<quint16> indices;
    if(indexed)indices.reserve(int(points_.size())*18+512);
    const auto vertex=[&](QPointF p,QColor color,double opacity=1.){
        QSGGeometry::ColoredPoint2D v;const double alpha=color.alphaF()*opacity;
        v.set(float(p.x()),float(p.y()),uchar(std::lround(color.red()*alpha)),uchar(std::lround(color.green()*alpha)),uchar(std::lround(color.blue()*alpha)),uchar(std::lround(255*alpha)));vertices.append(v);
    };
    const auto triangle=[&](QPointF a,QPointF b,QPointF c,QColor color){
        const auto first=vertices.size();vertex(a,color);vertex(b,color);vertex(c,color);
        if(indexed){indices.append(quint16(first));indices.append(quint16(first+1));indices.append(quint16(first+2));}
    };
    const auto rect=[&](QRectF r,QColor color){triangle(r.topLeft(),r.topRight(),r.bottomRight(),color);triangle(r.topLeft(),r.bottomRight(),r.bottomLeft(),color);};
    const auto stroke=[&](QPointF a,QPointF b,double thickness,QColor color){
        const QPointF delta=b-a;const double length=std::hypot(delta.x(),delta.y());if(length<1e-9)return;
        const QPointF normal(-delta.y()/length,delta.x()/length);
        const double inner=std::max(0.,thickness*.5-.5),outer=thickness*.5+.5;
        if(indexed) {
            const auto first=vertices.size();
            for(const auto edge:{std::pair{-outer,0.},std::pair{-inner,1.},std::pair{inner,1.},std::pair{outer,0.}}) {
                vertex(a+normal*edge.first,color,edge.second);vertex(b+normal*edge.first,color,edge.second);
            }
            for(const auto index:{2,3,5,2,5,4,4,5,7,4,7,6,0,1,3,0,3,2})indices.append(quint16(first+index));
            return;
        }
        const auto strip=[&](double left,double right,double alphaLeft,double alphaRight){
            const auto p=a+normal*left,q=b+normal*left,r=b+normal*right,s=a+normal*right;
            vertex(p,color,alphaLeft);vertex(q,color,alphaLeft);vertex(r,color,alphaRight);
            vertex(p,color,alphaLeft);vertex(r,color,alphaRight);vertex(s,color,alphaRight);
        };
        strip(-inner,inner,1,1);strip(inner,outer,1,0);strip(-outer,-inner,0,1);
    };
    auto grid=grid_color_;grid.setAlphaF(grid.alphaF()*.3);
    for(double x:x_ticks_)stroke({x*width(),0},{x*width(),height()},.75,grid);
    for(double y:y_ticks_)stroke({0,y*height()},{width(),y*height()},.75,grid);
    const auto& points=std::as_const(points_);
    const auto scaled=[this](QPointF p){return QPointF(p.x()*width(),p.y()*height());};
    // Clip in normalized double/extended precision BEFORE conversion to GPU
    // floats. Manual axis ranges can put otherwise valid samples far outside
    // the viewport; clipping individual points would distort intersections.
    const auto clippedLine=[](QPointF& a,QPointF& b){
        long double x0=a.x(),y0=a.y(),x1=b.x(),y1=b.y();
        if(!std::isfinite(x0)||!std::isfinite(y0)||!std::isfinite(x1)||!std::isfinite(y1))return false;
        const auto code=[](long double x,long double y){return (x<0?1:0)|(x>1?2:0)|(y<0?4:0)|(y>1?8:0);};
        for(int iteration=0;iteration<16;++iteration){
            const int c0=code(x0,y0),c1=code(x1,y1);
            if(!(c0|c1)){a={double(x0),double(y0)};b={double(x1),double(y1)};return true;}
            if(c0&c1)return false;
            const int c=c0?c0:c1;long double x,y;
            const auto fraction=[](long double edge,long double a,long double b){
                const auto scale=std::max({std::abs(a),std::abs(b),std::abs(edge),1.L});
                return (edge/scale-a/scale)/(b/scale-a/scale);
            };
            if(c&12){y=c&4?0:1;x=std::lerp(x0,x1,fraction(y,y0,y1));}
            else{x=c&1?0:1;y=std::lerp(y0,y1,fraction(x,x0,x1));}
            if(!std::isfinite(x)||!std::isfinite(y))return false;
            if(c0){x0=x;y0=y;}else{x1=x;y1=y;}
        }
        return false;
    };
    if(style_=="bars"){
        if(std::isfinite(baseline_))for(qsizetype i=0;i<points.size();++i){
            if(!std::isfinite(points[i].x())||!std::isfinite(points[i].y())||points[i].x()<-.5||points[i].x()>1.5)continue;
            const auto p=scaled({points[i].x(),std::clamp(points[i].y(),0.,1.)});double slot=width();
            if(i)slot=std::min(slot,std::abs(points[i].x()-points[i-1].x())*width());
            if(i+1<points.size())slot=std::min(slot,std::abs(points[i+1].x()-points[i].x())*width());
            const double pixel=1./std::max(1.,window()?window()->effectiveDevicePixelRatio():1.);
            const double w=std::max(pixel,slot*bar_width_/100.),base=std::clamp(baseline_,0.,1.)*height(),tip=std::clamp(p.y(),0.,height());
            rect({p.x()-w/2,std::min(base,tip),w,std::abs(base-tip)},series_color_);
        }
    }else if(style_=="points"||points.size()==1){
        constexpr int sections=24;constexpr double pi=3.14159265358979323846;
        for(auto point:points){
            const double marginX=point_size_/std::max(1.,width()),marginY=point_size_/std::max(1.,height());
            if(!std::isfinite(point.x())||!std::isfinite(point.y())||point.x()<-marginX||point.x()>1+marginX||point.y()<-marginY||point.y()>1+marginY)continue;
            const auto center=scaled(point);for(int i=0;i<sections;++i){
            const double a=2*pi*i/sections,b=2*pi*(i+1)/sections;
            triangle(center,center+QPointF(std::cos(a),std::sin(a))*point_size_/2,center+QPointF(std::cos(b),std::sin(b))*point_size_/2,series_color_);
        }}
    }else for(qsizetype i=1;i<points.size();++i){auto a=points[i-1],b=points[i];if(clippedLine(a,b))stroke(scaled(a),scaled(b),line_width_,series_color_);}
    auto* geometry=node->geometry();geometry->allocate(vertices.size(),indices.size());
    std::copy(vertices.cbegin(),vertices.cend(),geometry->vertexDataAsColoredPoint2D());
    if(!indices.isEmpty())std::copy(indices.cbegin(),indices.cend(),geometry->indexDataAsUShort());
    geometry->markVertexDataDirty();geometry->markIndexDataDirty();node->markDirty(QSGNode::DirtyGeometry);
    paint_engine_type_.store(-1,std::memory_order_relaxed);paint_samples_.store(-1,std::memory_order_relaxed);
    geometry_updates_.fetch_add(1,std::memory_order_relaxed);
    geometry_vertices_.store(vertices.size(),std::memory_order_relaxed);
    geometry_finite_.store(std::all_of(vertices.cbegin(),vertices.cend(),[](const auto& v){return std::isfinite(v.x)&&std::isfinite(v.y);}),std::memory_order_relaxed);
    const auto revision=presentation_revision_.load(std::memory_order_relaxed);
    if(consumed_revision_!=revision){consumed_revision_=revision;consumed_count_.fetch_add(1,std::memory_order_relaxed);}
    return node;
}

VectorPlotItem::VectorPlotItem(QQuickItem* parent):QQuickPaintedItem(parent) {
    setAntialiasing(true);
    // Qt >= 6.9 accelerates this target with the application's OpenGL backend.
    // Other backends/older Qt retain Qt's image fallback.
    setRenderTarget(QQuickPaintedItem::FramebufferObject);
}
void VectorPlotItem::setPresentation(const QVariantMap& value) {
    applyPresentation(value,false);
}
QObject* VectorPlotItem::sourceModel() const {return source_model_.data();}
void VectorPlotItem::setSourceModel(QObject* value) {
    auto* model=qobject_cast<QuickPlotModel*>(value);
    if(source_model_==model)return;
    disconnect(source_changed_);disconnect(source_destroyed_);source_model_=model;
    if(model){
        source_changed_=connect(model,&QuickPlotModel::changed,this,&VectorPlotItem::syncSourceModel);
        source_destroyed_=connect(model,&QObject::destroyed,this,[this]{applyPresentation({},false);emit sourceModelChanged();});
    }
    syncSourceModel();emit sourceModelChanged();
}
void VectorPlotItem::syncSourceModel() {
    if(!source_model_){applyPresentation({},false);return;}
    const bool typed=source_model_->hasTypedVector();
    if(typed)points_=source_model_->vectorPoints();
    applyPresentation(source_model_->state(),typed);
}
void VectorPlotItem::applyPresentation(const QVariantMap& value,bool typed) {
    presentation_=value;style_=value.value("style").toString();kind_=value.value("kind").toString();
    baseline_=value.value("baseline").toDouble();
    const auto bounded=[&](const char* key,double fallback,double low,double high){const double v=value.value(key,fallback).toDouble();return std::isfinite(v)?std::clamp(v,low,high):fallback;};
    line_width_=bounded("lineWidth",1.25,.5,20);point_size_=bounded("pointSize",12,1,64);bar_width_=bounded("barWidth",80,1,100);
    if(!typed){
        points_.clear();const auto points=value.value("points").toList();points_.reserve(points.size());
        for(const auto& item:points){
            const auto point=item.toMap();const double x=point.value("x").toDouble(),y=point.value("y").toDouble();
            if(std::isfinite(x)&&std::isfinite(y))points_.append({x,y});
        }
    }
    const auto ticks=[](const QVariant& source,QVector<double>& destination){
        destination.clear();const auto values=source.toList();destination.reserve(values.size());
        for(const auto& item:values){const double position=item.toMap().value("position").toDouble();if(std::isfinite(position))destination.append(position);}
    };
    const auto xTicks=value.value("xTicks"),yTicks=value.value("yTicks");
    if(x_tick_source_!=xTicks){x_tick_source_=xTicks;ticks(xTicks,x_ticks_);}
    if(y_tick_source_!=yTicks){y_tick_source_=yTicks;ticks(yTicks,y_ticks_);}
    slices_.clear();const auto slices=value.value("slices").toList();slices_.reserve(slices.size());
    for(const auto& item:slices){
        const auto slice=item.toMap();const double start=slice.value("start").toDouble(),fraction=slice.value("fraction").toDouble();
        if(std::isfinite(start)&&std::isfinite(fraction))slices_.append({start,fraction});
    }
    presentation_revision_.fetch_add(1,std::memory_order_relaxed);
    emit presentationChanged();requestPaint();
}
void VectorPlotItem::setColors(const QVariantMap& value) {
    if(colors_==value)return;
    colors_=value;grid_color_=value.value("mid").value<QColor>();series_color_=value.value("highlight").value<QColor>();
    base_color_=value.value("base").value<QColor>();text_color_=value.value("text").value<QColor>();
    emit colorsChanged();requestPaint();
}
void VectorPlotItem::setPieRadius(qreal value){
    if(!std::isfinite(value)||pie_radius_==value)return;
    pie_radius_=value;emit pieRadiusChanged();requestPaint();
}
void VectorPlotItem::setPieLabelWidth(qreal value){
    if(!std::isfinite(value)||pie_label_width_==value)return;
    pie_label_width_=value;emit pieLabelWidthChanged();requestPaint();
}
void VectorPlotItem::setPieLabels(const QVariantList& value){
    if(pie_labels_==value)return;
    pie_labels_=value;leaders_.clear();leaders_.reserve(value.size());
    for(const auto& item:value){
        const auto label=item.toMap();
        const QPointF anchor(label.value("anchorX").toDouble(),label.value("anchorY").toDouble());
        const QPointF position(label.value("x").toDouble(),label.value("y").toDouble());
        if(std::isfinite(anchor.x())&&std::isfinite(anchor.y())&&std::isfinite(position.x())&&std::isfinite(position.y()))
            leaders_.append({anchor,position,label.value("left").toBool()});
    }
    emit pieLabelsChanged();requestPaint();
}
void VectorPlotItem::requestPaint(){if(isVisible())update();}
void VectorPlotItem::itemChange(ItemChange change,const ItemChangeData& data) {
    QQuickPaintedItem::itemChange(change,data);
    if(change==ItemVisibleHasChanged&&isVisible())update();
}
void VectorPlotItem::geometryChange(const QRectF& next,const QRectF& previous) {
    QQuickPaintedItem::geometryChange(next,previous);
    if(next.size()!=previous.size())requestPaint();
}
void VectorPlotItem::paint(QPainter* painter) {
    core::perf::Scope vectorPaint(core::perf::Stage::Quick2dPaint);
    core::perf::Scope trace(core::perf::Stage::ChartPaint);
    if(const auto* engine=painter->paintEngine()){
        paint_engine_type_.store(int(engine->type()),std::memory_order_relaxed);
        if(engine->type()==QPaintEngine::OpenGL2){
            if(auto* context=QOpenGLContext::currentContext()){
                GLint samples=0;context->functions()->glGetIntegerv(GL_SAMPLES,&samples);
                paint_samples_.store(samples,std::memory_order_relaxed);
            }
        }
    }
    struct Completion {
        std::atomic<quint64>& paints;
        std::atomic<quint64>& consumed;
        quint64& previous;
        quint64 revision;
        ~Completion(){
            paints.fetch_add(1,std::memory_order_relaxed);
            if(previous!=revision){previous=revision;consumed.fetch_add(1,std::memory_order_relaxed);}
        }
    } completion{paint_count_,consumed_count_,consumed_revision_,presentation_revision_.load(std::memory_order_relaxed)};
    // Qt blocks the GUI thread while painting. Read only cached value data;
    // never invoke GUI objects, create QObjects or emit signals here.
    painter->save();painter->setClipRect(boundingRect());painter->setRenderHint(QPainter::Antialiasing);
    if(kind_=="pie"){
        const QPointF center(width()/2,height()/2);
        const QRectF circle(center.x()-pie_radius_,center.y()-pie_radius_,2*pie_radius_,2*pie_radius_);
        painter->setPen(QPen(grid_color_,1,Qt::SolidLine,Qt::FlatCap,Qt::MiterJoin));
        for(qsizetype i=0;i<slices_.size();++i){
            const double mix=.35+.65*double(i)/double(std::max(qsizetype{1},slices_.size()-1));
            const auto blend=[&](double base,double accent){return base+(accent-base)*mix;};
            painter->setBrush(QColor::fromRgbF(blend(base_color_.redF(),series_color_.redF()),blend(base_color_.greenF(),series_color_.greenF()),blend(base_color_.blueF(),series_color_.blueF()),1));
            QPainterPath slice;slice.moveTo(center);slice.arcTo(circle,90-slices_[i].x()*360,-slices_[i].y()*360);slice.closeSubpath();
            painter->drawPath(slice);
        }
        QPainterPath leaders;
        for(const auto& label:leaders_){
            const double edge=label.position.x()+(label.left?pie_label_width_:0);
            leaders.moveTo(label.anchor);leaders.lineTo(center.x()+(label.left?-1:1)*(pie_radius_+10),label.position.y());
            leaders.lineTo(edge+(label.left?4:-4),label.position.y());
        }
        painter->setPen(QPen(text_color_,.75,Qt::SolidLine,Qt::FlatCap,Qt::MiterJoin));painter->setBrush(Qt::NoBrush);painter->drawPath(leaders);
        painter->restore();return;
    }
    QPainterPath grid;
    for(const double tick:x_ticks_){const double x=tick*width();grid.moveTo(x,0);grid.lineTo(x,height());}
    for(const double tick:y_ticks_){const double y=tick*height();grid.moveTo(0,y);grid.lineTo(width(),y);}
    QPen gridPen(grid_color_,.75,Qt::SolidLine,Qt::FlatCap,Qt::MiterJoin);
    painter->setOpacity(.3);painter->setPen(gridPen);painter->setBrush(Qt::NoBrush);painter->drawPath(grid);painter->setOpacity(1);
    QPainterPath series;series.setFillRule(Qt::WindingFill);
    // Const access is essential: mutable QList/QPolygonF indexing would detach
    // the implicitly shared snapshot on the render thread.
    const auto& points=std::as_const(points_);
    const auto scaled=[this](const QPointF& point){return QPointF(point.x()*width(),point.y()*height());};
    if(style_=="bars"){
        if(std::isfinite(baseline_))for(qsizetype i=0;i<points.size();++i){
            const auto p=scaled(points[i]);
            double slot=width();
            if(i>0)slot=std::min(slot,std::abs(points[i].x()-points[i-1].x())*width());
            if(i+1<points.size())slot=std::min(slot,std::abs(points[i+1].x()-points[i].x())*width());
            const double pixel=1./std::max(1.,window()?window()->effectiveDevicePixelRatio():1.);
            const double barWidth=std::max(pixel,slot*bar_width_/100.);
            const double base=std::clamp(baseline_,0.,1.)*height(),tip=std::clamp(p.y(),0.,height());
            series.addRect(QRectF(p.x()-barWidth/2,std::min(base,tip),barWidth,std::abs(base-tip)));
        }
        painter->setPen(Qt::NoPen);painter->setBrush(series_color_);painter->drawPath(series);painter->restore();return;
    }else if(style_=="points"||points.size()==1){
        for(const auto& point:points)series.addEllipse(scaled(point),point_size_/2,point_size_/2);
        painter->setPen(Qt::NoPen);painter->setBrush(series_color_);painter->drawPath(series);painter->restore();return;
    }else{
        if(!points.isEmpty()){series.moveTo(scaled(points.front()));for(qsizetype i=1;i<points.size();++i)series.lineTo(scaled(points[i]));}
    }
    painter->setPen(QPen(series_color_,line_width_,Qt::SolidLine,Qt::FlatCap,Qt::MiterJoin));painter->drawPath(series);painter->restore();
}
