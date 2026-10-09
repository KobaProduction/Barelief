#pragma once
#include <QBrush>
#include <QPen>
#include <QFont>
#include <QFontMetricsF>
#include <QFontInfo>
#include <vector>
#include <QPolygonF>
#include <QTransform>
#include <QVariantMap>
#include <QVariantList>
#include <QPainter>

// A logical presentation display list. QML Canvas owns the actual overlay;
// the render adapter only projects anchors and computes collision-free layout.
class SurfaceOverlayCommands {
public:
    explicit SurfaceOverlayCommands(QFont font) {state.font=std::move(font);}
    QVariantList commands;
    void save(){stack.push_back(state);}
    void restore(){if(!stack.empty()){state=stack.back();stack.pop_back();}}
    void setPen(QPen pen){state.pen=std::move(pen);}
    void setPen(QColor color){setPen(QPen(color));}
    void setPen(Qt::PenStyle pen){setPen(QPen(pen));}
    void setBrush(QBrush brush){state.brush=std::move(brush);}
    void setBrush(QColor color){setBrush(QBrush(color));}
    void setBrush(Qt::BrushStyle brush){setBrush(QBrush(brush));}
    void setClipRect(QRectF rect){state.clip=state.transform.mapRect(rect);}
    void setRenderHint(QPainter::RenderHint,bool=true){}
    void setFont(QFont font){state.font=std::move(font);}
    QFont font() const{return state.font;}
    void translate(double x,double y){state.transform.translate(x,y);}
    void rotate(double degrees){state.transform.rotate(degrees);}
    void drawLine(QPointF a,QPointF b){add("line",{{"points",QVariantList{a.x(),a.y(),b.x(),b.y()}}});}
    void drawRect(QRectF rect){addRect("rect",rect);}
    void drawRoundedRect(QRectF rect,double x,double y){addRect("roundedRect",rect,{{"radiusX",x},{"radiusY",y}});}
    void fillRect(QRectF rect,QBrush brush){save();setBrush(brush);setPen(Qt::NoPen);addRect("rect",rect);restore();}
    void drawEllipse(QPointF center,double x,double y){addRect("ellipse",{center.x()-x,center.y()-y,2*x,2*y});}
    void drawPolygon(const QPolygonF& polygon){QVariantList points;for(auto p:polygon){points.append(p.x());points.append(p.y());}add("polygon",{{"points",points}});}
    void drawText(QPointF point,const QString& text){add("text",{{"x",point.x()},{"y",point.y()},{"text",text},{"baseline",true}});}
    void drawText(double x,double y,const QString& text){drawText(QPointF(x,y),text);}
    void drawText(QRectF rect,int flags,const QString& text){addRect("text",rect,{{"text",text},{"flags",flags},{"baseline",false}});}
    void end(){}
private:
    struct State {QPen pen;QBrush brush{Qt::NoBrush};QFont font;QTransform transform;QRectF clip;};
    State state;std::vector<State> stack;
    static QVariantMap brush(const QBrush& value){
        QVariantMap result{{"color",value.color().name(QColor::HexArgb)},{"none",value.style()==Qt::NoBrush}};
        if(const auto* gradient=value.gradient()){
            QVariantList stops;for(const auto& stop:gradient->stops())stops.append(QVariantMap{{"position",stop.first},{"color",stop.second.name(QColor::HexArgb)}});
            result["stops"]=stops;
            if(gradient->type()==QGradient::LinearGradient){const auto* linear=static_cast<const QLinearGradient*>(gradient);result["start"]=linear->start();result["end"]=linear->finalStop();}
        }return result;
    }
    void addRect(const char* kind,QRectF rect,QVariantMap values={}){
        values["x"]=rect.x();values["y"]=rect.y();values["width"]=rect.width();values["height"]=rect.height();add(kind,std::move(values));
    }
    void add(const char* kind,QVariantMap values){
        values["kind"]=QString::fromLatin1(kind);values["pen"]=state.pen.color().name(QColor::HexArgb);values["penWidth"]=state.pen.widthF();values["noPen"]=state.pen.style()==Qt::NoPen;
        values["brush"]=brush(state.brush);values["font"]=state.font.family();values["fontPixels"]=QFontInfo(state.font).pixelSize();values["fontBold"]=state.font.bold();
        const auto& t=state.transform;values["transform"]=QVariantList{t.m11(),t.m12(),t.m21(),t.m22(),t.dx(),t.dy()};values["clip"]=state.clip;
        commands.append(std::move(values));
    }
};
