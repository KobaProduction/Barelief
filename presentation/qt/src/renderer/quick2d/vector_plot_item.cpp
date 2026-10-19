#include <barelief/quick2d/vector_plot_item.h>
#include <barelief/quick2d/plot_model.h>
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

namespace {
struct VectorGeometryStyle {
    QString style;
    QColor grid,series;
    double width,height,baseline,line_width,point_size,bar_width,pixel;
};
class UnitLineClip {
    long double x0,y0,x1,y1;
    static int code(long double x,long double y){return (x<0?1:0)|(x>1?2:0)|(y<0?4:0)|(y>1?8:0);}
    static long double fraction(long double edge,long double a,long double b){
        const auto scale=std::max({std::abs(a),std::abs(b),std::abs(edge),1.L});
        return (edge/scale-a/scale)/(b/scale-a/scale);
    }
    bool move_endpoint(int c0,int c1){
        const int c=c0?c0:c1;long double x,y;
        if(c&12){y=c&4?0:1;x=std::lerp(x0,x1,fraction(y,y0,y1));}
        else{x=c&1?0:1;y=std::lerp(y0,y1,fraction(x,x0,x1));}
        if(!std::isfinite(x)||!std::isfinite(y))return false;
        if(c0){x0=x;y0=y;}else{x1=x;y1=y;}return true;
    }
public:
    UnitLineClip(QPointF a,QPointF b):x0(a.x()),y0(a.y()),x1(b.x()),y1(b.y()){}
    bool clip(QPointF& a,QPointF& b){
        if(!std::isfinite(x0)||!std::isfinite(y0)||!std::isfinite(x1)||!std::isfinite(y1))return false;
        for(int iteration=0;iteration<16;++iteration){
            const int c0=code(x0,y0),c1=code(x1,y1);
            if(!(c0|c1)){a={double(x0),double(y0)};b={double(x1),double(y1)};return true;}
            if(c0&c1 || !move_endpoint(c0,c1))return false;
        }
        return false;
    }
};
QSGGeometryNode* vector_geometry_node(QSGNode* old){
    auto* node=static_cast<QSGGeometryNode*>(old);if(node)return node;
    node=new QSGGeometryNode;
    auto* geometry=new QSGGeometry(QSGGeometry::defaultAttributes_ColoredPoint2D(),0);
    geometry->setDrawingMode(QSGGeometry::DrawTriangles);
    geometry->setVertexDataPattern(QSGGeometry::DynamicPattern);geometry->setIndexDataPattern(QSGGeometry::DynamicPattern);
    node->setGeometry(geometry);node->setFlag(QSGNode::OwnsGeometry);
    node->setMaterial(new QSGVertexColorMaterial);node->setFlag(QSGNode::OwnsMaterial);return node;
}
class VectorGeometryBuilder {
    const QPolygonF& points;
    const QVector<double>& x_ticks;
    const QVector<double>& y_ticks;
    const VectorGeometryStyle style;
    const bool indexed;
    QVector<QSGGeometry::ColoredPoint2D> vertices;
    QVector<quint16> indices;
    QPointF scaled(QPointF p)const{return {p.x()*style.width,p.y()*style.height};}
    void vertex(QPointF p,QColor color,double opacity=1.){
        QSGGeometry::ColoredPoint2D v;const double alpha=color.alphaF()*opacity;
        v.set(float(p.x()),float(p.y()),uchar(std::lround(color.red()*alpha)),uchar(std::lround(color.green()*alpha)),uchar(std::lround(color.blue()*alpha)),uchar(std::lround(255*alpha)));vertices.append(v);
    }
    void triangle(QPointF a,QPointF b,QPointF c,QColor color){
        const auto first=vertices.size();vertex(a,color);vertex(b,color);vertex(c,color);
        if(indexed){indices.append(quint16(first));indices.append(quint16(first+1));indices.append(quint16(first+2));}
    }
    void rectangle(QRectF r,QColor color){triangle(r.topLeft(),r.topRight(),r.bottomRight(),color);triangle(r.topLeft(),r.bottomRight(),r.bottomLeft(),color);}
    void indexed_stroke(QPointF a,QPointF b,QPointF normal,double inner,double outer,QColor color){
        const auto first=vertices.size();
        for(const auto edge:{std::pair{-outer,0.},std::pair{-inner,1.},std::pair{inner,1.},std::pair{outer,0.}}){
            vertex(a+normal*edge.first,color,edge.second);vertex(b+normal*edge.first,color,edge.second);
        }
        for(const auto index:{2,3,5,2,5,4,4,5,7,4,7,6,0,1,3,0,3,2})indices.append(quint16(first+index));
    }
    void strip(QPointF a,QPointF b,QPointF normal,double left,double right,double alpha_left,double alpha_right,QColor color){
        const auto p=a+normal*left,q=b+normal*left,r=b+normal*right,s=a+normal*right;
        vertex(p,color,alpha_left);vertex(q,color,alpha_left);vertex(r,color,alpha_right);
        vertex(p,color,alpha_left);vertex(r,color,alpha_right);vertex(s,color,alpha_right);
    }
    void stroke(QPointF a,QPointF b,double thickness,QColor color){
        const QPointF delta=b-a;const double length=std::hypot(delta.x(),delta.y());if(length<1e-9)return;
        const QPointF normal(-delta.y()/length,delta.x()/length);
        const double inner=std::max(0.,thickness*.5-.5),outer=thickness*.5+.5;
        if(indexed){indexed_stroke(a,b,normal,inner,outer,color);return;}
        strip(a,b,normal,-inner,inner,1,1,color);strip(a,b,normal,inner,outer,1,0,color);strip(a,b,normal,-outer,-inner,0,1,color);
    }
    void grid(){
        auto color=style.grid;color.setAlphaF(color.alphaF()*.3);
        for(double x:x_ticks)stroke({x*style.width,0},{x*style.width,style.height},.75,color);
        for(double y:y_ticks)stroke({0,y*style.height},{style.width,y*style.height},.75,color);
    }
    void bar(qsizetype i){
        if(!std::isfinite(points[i].x())||!std::isfinite(points[i].y())||points[i].x()<-.5||points[i].x()>1.5)return;
        const auto p=scaled({points[i].x(),std::clamp(points[i].y(),0.,1.)});double slot=style.width;
        if(i)slot=std::min(slot,std::abs(points[i].x()-points[i-1].x())*style.width);
        if(i+1<points.size())slot=std::min(slot,std::abs(points[i+1].x()-points[i].x())*style.width);
        const double w=std::max(style.pixel,slot*style.bar_width/100.),base=std::clamp(style.baseline,0.,1.)*style.height,tip=std::clamp(p.y(),0.,style.height);
        rectangle({p.x()-w/2,std::min(base,tip),w,std::abs(base-tip)},style.series);
    }
    void point(QPointF position){
        constexpr int sections=24;constexpr double pi=3.14159265358979323846;
        const double margin_x=style.point_size/std::max(1.,style.width),margin_y=style.point_size/std::max(1.,style.height);
        if(!std::isfinite(position.x())||!std::isfinite(position.y())||position.x()<-margin_x||position.x()>1+margin_x||position.y()<-margin_y||position.y()>1+margin_y)return;
        const auto center=scaled(position);
        for(int i=0;i<sections;++i){
            const double a=2*pi*i/sections,b=2*pi*(i+1)/sections;
            triangle(center,center+QPointF(std::cos(a),std::sin(a))*style.point_size/2,center+QPointF(std::cos(b),std::sin(b))*style.point_size/2,style.series);
        }
    }
    void bars_series(){
        if(!std::isfinite(style.baseline))return;
        for(qsizetype i=0;i<points.size();++i)bar(i);
    }
    void points_series(){for(auto value:points)point(value);}
    void lines_series(){
        for(qsizetype i=1;i<points.size();++i){
            auto a=points[i-1],b=points[i];
            if(UnitLineClip(a,b).clip(a,b))stroke(scaled(a),scaled(b),style.line_width,style.series);
        }
    }
    void series(){
        if(style.style=="bars"){bars_series();return;}
        if(style.style=="points"||points.size()==1){points_series();return;}
        lines_series();
    }
public:
    VectorGeometryBuilder(const QPolygonF& values,const QVector<double>& x,const QVector<double>& y,VectorGeometryStyle appearance)
        :points(values),x_ticks(x),y_ticks(y),style(std::move(appearance)),indexed(style.style!="points"&&points.size()!=1&&points.size()<=4098&&x.size()+y.size()<=1024){
        vertices.reserve(int(std::min<qsizetype>(points.size(),4098))*18+512);
        if(indexed)indices.reserve(int(points.size())*18+512);
    }
    void build(){grid();series();}
    void upload(QSGGeometryNode& node)const{
        auto* geometry=node.geometry();geometry->allocate(vertices.size(),indices.size());
        std::copy(vertices.cbegin(),vertices.cend(),geometry->vertexDataAsColoredPoint2D());
        if(!indices.isEmpty())std::copy(indices.cbegin(),indices.cend(),geometry->indexDataAsUShort());
        geometry->markVertexDataDirty();geometry->markIndexDataDirty();node.markDirty(QSGNode::DirtyGeometry);
    }
    qsizetype vertex_count()const{return vertices.size();}
    bool finite()const{return std::all_of(vertices.cbegin(),vertices.cend(),[](const auto& value){return std::isfinite(value.x)&&std::isfinite(value.y);});}
};
}
VectorGeometryItem::VectorGeometryItem(QQuickItem* parent):VectorPlotItem(parent){setClip(true);}
QSGNode* VectorGeometryItem::updatePaintNode(QSGNode* oldNode,UpdatePaintNodeData* data){
    if(window() && window()->rendererInterface()->graphicsApi()==QSGRendererInterface::Software)
        return QQuickPaintedItem::updatePaintNode(oldNode,data);
    core::perf::Scope trace(core::perf::Stage::Quick2dGeometry);
    auto* node=vector_geometry_node(oldNode);
    const double pixel=1./std::max(1.,window()?window()->effectiveDevicePixelRatio():1.);
    VectorGeometryBuilder builder(points_,x_ticks_,y_ticks_,{style_,grid_color_,series_color_,width(),height(),baseline_,line_width_,point_size_,bar_width_,pixel});
    builder.build();builder.upload(*node);
    paint_engine_type_.store(-1,std::memory_order_relaxed);paint_samples_.store(-1,std::memory_order_relaxed);
    geometry_updates_.fetch_add(1,std::memory_order_relaxed);geometry_vertices_.store(builder.vertex_count(),std::memory_order_relaxed);
    geometry_finite_.store(builder.finite(),std::memory_order_relaxed);consumePresentation();
    QMetaObject::invokeMethod(this,[this]{emit geometryTelemetryChanged();emit telemetryChanged();},Qt::QueuedConnection);
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
void VectorPlotItem::readPoints(const QVariant& value){
    points_.clear();const auto points=value.toList();points_.reserve(points.size());
    for(const auto& item:points){
        const auto point=item.toMap();const double x=point.value("x").toDouble(),y=point.value("y").toDouble();
        if(std::isfinite(x)&&std::isfinite(y))points_.append({x,y});
    }
}
void VectorPlotItem::readTicks(const QVariantMap& value){
    const auto ticks=[](const QVariant& source,QVector<double>& destination){
        destination.clear();const auto values=source.toList();destination.reserve(values.size());
        for(const auto& item:values){const double position=item.toMap().value("position").toDouble();if(std::isfinite(position))destination.append(position);}
    };
    const auto xTicks=value.value("xGrid",value.value("xTicks")),yTicks=value.value("yGrid",value.value("yTicks"));
    if(x_tick_source_!=xTicks){x_tick_source_=xTicks;ticks(xTicks,x_ticks_);}
    if(y_tick_source_!=yTicks){y_tick_source_=yTicks;ticks(yTicks,y_ticks_);}
}
void VectorPlotItem::readSlices(const QVariant& value){
    slices_.clear();const auto slices=value.toList();slices_.reserve(slices.size());
    for(const auto& item:slices){
        const auto slice=item.toMap();const double start=slice.value("start").toDouble(),fraction=slice.value("fraction").toDouble();
        if(std::isfinite(start)&&std::isfinite(fraction))slices_.append({start,fraction});
    }
}

void VectorPlotItem::applyPresentation(const QVariantMap& value,bool typed) {
    presentation_=value;style_=value.value("style").toString();kind_=value.value("kind").toString();
    baseline_=value.value("baseline").toDouble();
    const auto bounded=[&](const char* key,double fallback,double low,double high){const double v=value.value(key,fallback).toDouble();return std::isfinite(v)?std::clamp(v,low,high):fallback;};
    line_width_=bounded("lineWidth",1.25,.5,20);point_size_=bounded("pointSize",12,1,64);bar_width_=bounded("barWidth",80,1,100);
    if(!typed)readPoints(value.value("points"));
    readTicks(value);readSlices(value.value("slices"));
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
void VectorPlotItem::consumePresentation(){
    const auto revision=presentation_revision_.load(std::memory_order_relaxed);
    if(consumed_revision_!=revision){consumed_revision_=revision;consumed_count_.fetch_add(1,std::memory_order_relaxed);}
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
void VectorPlotItem::recordPaintEngine(QPainter* painter){
    const auto* engine=painter->paintEngine();if(!engine)return;
    paint_engine_type_.store(int(engine->type()),std::memory_order_relaxed);
    if(engine->type()!=QPaintEngine::OpenGL2)return;
    if(auto* context=QOpenGLContext::currentContext()){
        GLint samples=0;context->functions()->glGetIntegerv(GL_SAMPLES,&samples);
        paint_samples_.store(samples,std::memory_order_relaxed);
    }
}
QColor VectorPlotItem::pieColor(qsizetype index) const{
    const double mix=.35+.65*double(index)/double(std::max(qsizetype{1},slices_.size()-1));
    const auto blend=[&](double base,double accent){return base+(accent-base)*mix;};
    return QColor::fromRgbF(blend(base_color_.redF(),series_color_.redF()),blend(base_color_.greenF(),series_color_.greenF()),blend(base_color_.blueF(),series_color_.blueF()),1);
}
void VectorPlotItem::paintPieLeaders(QPainter* painter,const QPointF& center){
    QPainterPath path;
    for(const auto& label:std::as_const(leaders_)){
        const double edge=label.position.x()+(label.left?pie_label_width_:0);
        path.moveTo(label.anchor);path.lineTo(center.x()+(label.left?-1:1)*(pie_radius_+10),label.position.y());
        path.lineTo(edge+(label.left?4:-4),label.position.y());
    }
    painter->setPen(QPen(text_color_,.75,Qt::SolidLine,Qt::FlatCap,Qt::MiterJoin));
    painter->setBrush(Qt::NoBrush);painter->drawPath(path);
}
void VectorPlotItem::paintPie(QPainter* painter){
    const QPointF center(width()/2,height()/2);
    const QRectF circle(center.x()-pie_radius_,center.y()-pie_radius_,2*pie_radius_,2*pie_radius_);
    painter->setPen(QPen(grid_color_,1,Qt::SolidLine,Qt::FlatCap,Qt::MiterJoin));
    const auto& slices=std::as_const(slices_);
    for(qsizetype index=0;index<slices.size();++index){
        painter->setBrush(pieColor(index));
        QPainterPath slice;slice.moveTo(center);slice.arcTo(circle,90-slices[index].x()*360,-slices[index].y()*360);slice.closeSubpath();
        painter->drawPath(slice);
    }
    paintPieLeaders(painter,center);
}
void VectorPlotItem::paintGrid(QPainter* painter){
    QPainterPath grid;
    for(const double tick:std::as_const(x_ticks_)){const double x=tick*width();grid.moveTo(x,0);grid.lineTo(x,height());}
    for(const double tick:std::as_const(y_ticks_)){const double y=tick*height();grid.moveTo(0,y);grid.lineTo(width(),y);}
    const QPen pen(grid_color_,.75,Qt::SolidLine,Qt::FlatCap,Qt::MiterJoin);
    painter->setOpacity(.3);painter->setPen(pen);painter->setBrush(Qt::NoBrush);painter->drawPath(grid);painter->setOpacity(1);
}
QRectF VectorPlotItem::barRectangle(qsizetype index) const{
    const auto& points=points_;const auto& point=points[index];double slot=width();
    if(index>0)slot=std::min(slot,std::abs(point.x()-points[index-1].x())*width());
    if(index+1<points.size())slot=std::min(slot,std::abs(points[index+1].x()-point.x())*width());
    const double pixel=1./std::max(1.,window()?window()->effectiveDevicePixelRatio():1.);
    const double barWidth=std::max(pixel,slot*bar_width_/100.);
    const double base=std::clamp(baseline_,0.,1.)*height(),tip=std::clamp(point.y()*height(),0.,height());
    return {point.x()*width()-barWidth/2,std::min(base,tip),barWidth,std::abs(base-tip)};
}
void VectorPlotItem::addBarSeries(QPainterPath& series) const {
    if(std::isfinite(baseline_))for(qsizetype index=0;index<points_.size();++index)series.addRect(barRectangle(index));
}
void VectorPlotItem::addPointSeries(QPainterPath& series) const {
    const auto scaled=[this](const QPointF& point){return QPointF(point.x()*width(),point.y()*height());};
    for(const auto& point:points_)series.addEllipse(scaled(point),point_size_/2,point_size_/2);
}
void VectorPlotItem::addLineSeries(QPainterPath& series) const {
    if(points_.isEmpty())return;
    const auto scaled=[this](const QPointF& point){return QPointF(point.x()*width(),point.y()*height());};
    series.moveTo(scaled(points_.front()));
    for(qsizetype index=1;index<points_.size();++index)series.lineTo(scaled(points_[index]));
}
QPainterPath VectorPlotItem::seriesPath() const{
    QPainterPath series;series.setFillRule(Qt::WindingFill);
    if(style_=="bars")addBarSeries(series);
    else if(style_=="points"||points_.size()==1)addPointSeries(series);
    else addLineSeries(series);
    return series;
}
void VectorPlotItem::paintCartesian(QPainter* painter){
    paintGrid(painter);const auto series=seriesPath();
    if(style_=="bars" || style_=="points" || points_.size()==1){painter->setPen(Qt::NoPen);painter->setBrush(series_color_);}
    else painter->setPen(QPen(series_color_,line_width_,Qt::SolidLine,Qt::FlatCap,Qt::MiterJoin));
    painter->drawPath(series);
}

void VectorPlotItem::paint(QPainter* painter) {
    core::perf::Scope vectorPaint(core::perf::Stage::Quick2dPaint);
    core::perf::Scope trace(core::perf::Stage::ChartPaint);recordPaintEngine(painter);
    struct Completion {
        VectorPlotItem* owner;std::atomic<quint64>& paints;std::atomic<quint64>& consumed;quint64& previous;quint64 revision;
        ~Completion(){
            paints.fetch_add(1,std::memory_order_relaxed);
            if(previous!=revision){previous=revision;consumed.fetch_add(1,std::memory_order_relaxed);}
            QMetaObject::invokeMethod(owner,[item=owner]{emit item->telemetryChanged();},Qt::QueuedConnection);
        }
    } completion{this,paint_count_,consumed_count_,consumed_revision_,presentation_revision_.load(std::memory_order_relaxed)};
    // Qt blocks the GUI thread. Helpers read cached values and create no QObjects.
    painter->save();painter->setClipRect(boundingRect());painter->setRenderHint(QPainter::Antialiasing);
    if(kind_=="pie")paintPie(painter);else paintCartesian(painter);
    painter->restore();
}
