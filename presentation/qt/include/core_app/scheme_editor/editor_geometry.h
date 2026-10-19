#pragma once
#include "audio_node_model.h"
#include "node_parameter_model.h"
#include "barelief/ui_text.h"
#include <QtNodes/internal/DefaultHorizontalNodeGeometry.hpp>
#include <QtNodes/internal/DefaultNodePainter.hpp>
#include <QtNodes/internal/NodeGraphicsObject.hpp>
#include <QtNodes/internal/BasicGraphicsScene.hpp>
#include <QtNodes/DataFlowGraphModel>
#include <QApplication>
#include <QFontMetricsF>
#include <QLayout>
#include <QStyleOptionButton>
#include <QStyle>
#include <QPolygonF>
#include "embedded_widget_layout.h"
#include <cmath>

// Presentation ordering only: every geometry API still addresses stable port indices.
class EditorNodeGeometry final : public QtNodes::DefaultHorizontalNodeGeometry {
public:
    EditorNodeGeometry(QtNodes::AbstractGraphModel& model,std::function<QJsonObject()> settings)
        :DefaultHorizontalNodeGeometry(model),settings_(std::move(settings)){}
    double setting(const char* key,double fallback,double low,double high) const {return std::clamp(settings_().value(key).toDouble(fallback),low,high);}
    QFont label_font() const {auto f=QApplication::font();f.setPointSizeF(setting("port_font",9,7,16));return f;}
    double pitch() const {return std::max(setting("port_spacing",52,40,160),QFontMetricsF(label_font()).height()*3+8);}
    double port_offset(QtNodes::NodeId id,QtNodes::PortType side,unsigned position) const {
        auto* n=model(id);const auto channels=n?n->grouped_channels(side):0;
        const bool stream=n && n->name()=="InputSelector" && side==QtNodes::PortType::In;
        const auto spacing=stream?QFontMetricsF(label_font()).height()+16:pitch();
        return position*spacing+(channels?position/channels*12.:0.);
    }
    double side_span(QtNodes::NodeId id,QtNodes::PortType side) const {return count(id,side)?port_offset(id,side,count(id,side)-1):0.;}
    double radius() const {return setting("port_radius",7,5,14);}
    QRectF boundingRect(QtNodes::NodeId id) const override {const double margin=radius()+10;return QRectF(QPointF{},size(id)).adjusted(-margin,-margin,margin,margin);}
    double top(QtNodes::NodeId id) const {
        int lines=1;for(const auto side:{QtNodes::PortType::In,QtNodes::PortType::Out})for(unsigned port=0;port<count(id,side);++port)lines=std::max(lines,int(label(id,side,port).count('\n')+1));
        return std::max(captionRect(id).height()+labelRect(id).height()+24,
            2*captionRect(id).height()+14+QFontMetricsF(label_font()).height()*lines/2);
    }
    unsigned count(QtNodes::NodeId id,QtNodes::PortType side) const {return _graphModel.nodeData(id,side==QtNodes::PortType::In?QtNodes::NodeRole::InPortCount:QtNodes::NodeRole::OutPortCount).toUInt();}
    QString label(QtNodes::NodeId id,QtNodes::PortType side,unsigned port) const {return _graphModel.portData(id,side,port,QtNodes::PortRole::Caption).toString();}
    double text_width(QtNodes::NodeId id,QtNodes::PortType side) const {
        double width=0;
        const QFontMetricsF metrics(label_font());
        for(unsigned port=0;port<count(id,side);++port){
            const auto lines=label(id,side,port).split('\n');
            for(const auto& line:lines)width=std::max(width,metrics.horizontalAdvance(line));
        }
        return width+(side==QtNodes::PortType::In && model(id) && model(id)->name()=="InputSelector"?30:0);
    }
    void recomputeSize(QtNodes::NodeId id) const override {
        auto* body=_graphModel.nodeData(id,QtNodes::NodeRole::Widget).value<QWidget*>();
        if(body){
            const int preferred=int(setting("node_width",200,140,400));
            body->setProperty("nodePreferredWidth",preferred);
            body->setFixedWidth(embeddedContentWidth(body,preferred));
            if(auto* layout=body->layout()){
                layout->activate();
                const int height=embeddedContentHeight(body);
                body->setFixedHeight(std::max(0,height));layout->activate();
            }
        }
        const double height=std::max(top(id)+(body?body->height()+16.0:0.0),2*top(id)+std::max(side_span(id,QtNodes::PortType::In),side_span(id,QtNodes::PortType::Out)));
        const auto* delegate=model(id);const bool bypass=delegate && delegate->parameter_model()->supportsBypass();
        const double width=std::max(captionRect(id).width()+(bypass?200:80),text_width(id,QtNodes::PortType::In)+text_width(id,QtNodes::PortType::Out)+(body?body->width():0)+64);
        _graphModel.setNodeData(id,QtNodes::NodeRole::Size,QSize(int(std::ceil(width)),int(std::ceil(height))));
    }
    QPointF captionPosition(QtNodes::NodeId id) const override {
        const auto caption=captionRect(id);
        return {(size(id).width()-caption.width())/2.,8+caption.height()};
    }
    QPointF widgetPosition(QtNodes::NodeId id) const override {
        return {text_width(id,QtNodes::PortType::In)+32,top(id)+8};
    }
    QPointF portPosition(QtNodes::NodeId id,QtNodes::PortType side,QtNodes::PortIndex port) const override {
        auto* graph=dynamic_cast<QtNodes::DataFlowGraphModel*>(&_graphModel);
        auto* model=graph?graph->delegateModel<AudioNodeModel>(id):nullptr;
        const auto order=model?model->visual_port_order(side):std::vector<QtNodes::PortIndex>{};
        const auto it=std::find(order.begin(),order.end(),port);const auto position=it==order.end()?port:unsigned(it-order.begin());
        return {side==QtNodes::PortType::In?0.0:double(size(id).width()),size(id).height()/2.0+port_offset(id,side,position)-side_span(id,side)/2.};
    }
    QPointF portTextPosition(QtNodes::NodeId id,QtNodes::PortType side,QtNodes::PortIndex port) const override {
        const auto p=portPosition(id,side,port);QFontMetricsF metrics(label_font());
        return {side==QtNodes::PortType::In?16.0:double(size(id).width())-16-text_width(id,side),p.y()-metrics.height()*label(id,side,port).split('\n').size()/2.0};
    }
    AudioNodeModel* model(QtNodes::NodeId id) const {
        auto* graph=dynamic_cast<QtNodes::DataFlowGraphModel*>(&_graphModel);return graph?graph->delegateModel<AudioNodeModel>(id):nullptr;
    }
    QRectF bypassRect(QtNodes::NodeId id) const {
        const auto* delegate=model(id);if(!delegate || !delegate->parameter_model()->supportsBypass())return {};
        return {double(size(id).width())-88,4,80,std::max(22.,captionRect(id).height())};
    }
    QRectF selectorButtonRect(QtNodes::NodeId id,QtNodes::PortIndex port) const {
        auto* node=model(id);if(!node || node->name()!="InputSelector" || port>=count(id,QtNodes::PortType::In) || node->configuration_role(QtNodes::PortType::In,port))return {};
        const auto channels=node->grouped_channels(QtNodes::PortType::In);
        if(channels && node->port_id(QtNodes::PortType::In,port)%channels!=0)return {};
        const QFontMetricsF metrics(label_font());const auto pin=portPosition(id,QtNodes::PortType::In,port);
        return {20,pin.y()-(metrics.height()+10)/2,metrics.horizontalAdvance(label(id,QtNodes::PortType::In,port))+18,metrics.height()+10};
    }
    QtNodes::PortIndex selectorButtonHit(QtNodes::NodeId id,QPointF point) const {
        for(unsigned port=0;port<count(id,QtNodes::PortType::In);++port)if(selectorButtonRect(id,port).contains(point))return port;
        return QtNodes::InvalidPortIndex;
    }
    QtNodes::PortIndex checkPortHit(QtNodes::NodeId id,QtNodes::PortType side,QPointF point) const override {
        for(unsigned port=0;port<count(id,side);++port)if(QLineF(point,portPosition(id,side,port)).length()<=std::max(16.0,radius()+8))return port;
        return QtNodes::InvalidPortIndex;
    }
    int getPortSpacing() override{return int(pitch());}
private:
    std::function<QJsonObject()> settings_;
};
class EditorNodePainter final : public QtNodes::DefaultNodePainter {
public:
    void paint(QPainter* painter,QtNodes::NodeGraphicsObject& node) const override {
        drawNodeRect(painter,node);drawNodeCaption(painter,node);drawNodeLabel(painter,node);
        const auto& geometry=static_cast<EditorNodeGeometry&>(node.nodeScene()->nodeGeometry());
        painter->save();painter->setFont(geometry.label_font());
        for(auto side:{QtNodes::PortType::In,QtNodes::PortType::Out})
            for(unsigned port=0;port<geometry.count(node.nodeId(),side);++port)paint_port(*painter,node,geometry,side,port);
        paint_node_controls(*painter,node,geometry);
        painter->restore();drawProcessingIndicator(painter,node);drawValidationIcon(painter,node);
    }
private:
    static void paint_pin(QPainter& painter,const QPointF& pin,double radius,bool configuration) {
        if(!configuration){painter.drawEllipse(pin,radius,radius);return;}
        QPolygonF diamond;
        diamond<<pin+QPointF(0,-radius)<<pin+QPointF(radius,0)<<pin+QPointF(0,radius)<<pin+QPointF(-radius,0);
        painter.drawPolygon(diamond);
    }
    static bool selector_selected(const AudioNodeModel& model,QtNodes::PortType side,unsigned port) {
        const auto channels=domain::data_outputs(*model.numeric_node()).size();
        const auto input=model.port_id(side,port);
        return model.selected_input()==(channels>1?input/channels:input);
    }
    static void paint_selector_marker(QPainter& painter,const QRectF& button,bool selected) {
        const auto palette=QApplication::palette();
        painter.setPen(QPen(palette.color(QPalette::Mid),1));painter.setBrush(palette.color(selected?QPalette::Highlight:QPalette::Base));
        painter.drawEllipse(QPointF(button.right()+9,button.center().y()),4,4);
    }
    static void paint_selector(QPainter& painter,QtNodes::NodeId id,const EditorNodeGeometry& geometry,unsigned port,const QRectF& button) {
        QStyleOptionButton option;option.rect=button.toAlignedRect();option.palette=QApplication::palette();
        option.fontMetrics=QFontMetrics(painter.font());option.text=geometry.label(id,QtNodes::PortType::In,port);option.state=QStyle::State_Enabled;
        const bool selected=selector_selected(*geometry.model(id),QtNodes::PortType::In,port);
        if(selected)option.state|=QStyle::State_On|QStyle::State_Sunken;else option.state|=QStyle::State_Raised;
        QApplication::style()->drawControl(QStyle::CE_PushButton,&option,&painter);
        paint_selector_marker(painter,button,selected);
    }
    static void paint_port_label(QPainter& painter,QtNodes::NodeId id,const EditorNodeGeometry& geometry,QtNodes::PortType side,unsigned port) {
        painter.setPen(QApplication::palette().color(QPalette::WindowText));
        const auto position=geometry.portTextPosition(id,side,port);const QFontMetricsF metrics(painter.font());
        const auto lines=geometry.label(id,side,port).split('\n');
        for(int i=0;i<lines.size();++i){
            const double x=side==QtNodes::PortType::Out?geometry.size(id).width()-16-metrics.horizontalAdvance(lines[i]):position.x();
            painter.drawText(QPointF(x,position.y()+metrics.ascent()+i*metrics.height()),lines[i]);
        }
    }
    static void paint_port(QPainter& painter,QtNodes::NodeGraphicsObject& node,const EditorNodeGeometry& geometry,QtNodes::PortType side,unsigned port) {
        const bool connected=!node.graphModel().connections(node.nodeId(),side,port).empty();
        const auto palette=QApplication::palette();
        painter.setPen(QPen(palette.color(QPalette::Text),1));painter.setBrush(palette.color(connected?QPalette::Highlight:QPalette::Base));
        auto* delegate=geometry.model(node.nodeId());
        paint_pin(painter,geometry.portPosition(node.nodeId(),side,port),geometry.radius(),delegate && delegate->configuration_role(side,port).has_value());
        const auto button=side==QtNodes::PortType::In?geometry.selectorButtonRect(node.nodeId(),port):QRectF{};
        if(!button.isEmpty()){paint_selector(painter,node.nodeId(),geometry,port,button);return;}
        paint_port_label(painter,node.nodeId(),geometry,side,port);
    }
    static void paint_bypass(QPainter& painter,const AudioNodeModel& delegate,const QRectF& bypass) {
        if(bypass.isEmpty())return;
        QStyleOptionButton option;option.rect=bypass.toAlignedRect();option.palette=QApplication::palette();
        option.fontMetrics=QFontMetrics(painter.font());option.text=ui::text("Bypass");
        option.state=QStyle::State_Enabled|(delegate.parameter_model()->bypassed()?QStyle::State_On|QStyle::State_Sunken:QStyle::State_Off|QStyle::State_Raised);
        QApplication::style()->drawControl(QStyle::CE_PushButton,&option,&painter);
    }
    static void paint_node_controls(QPainter& painter,QtNodes::NodeGraphicsObject& node,const EditorNodeGeometry& geometry) {
        auto* delegate=geometry.model(node.nodeId());if(!delegate)return;
        paint_bypass(painter,*delegate,geometry.bypassRect(node.nodeId()));
        painter.setPen(Qt::NoPen);
        painter.setBrush(QApplication::palette().color(delegate->processing_measured()&&delegate->processing_active()?QPalette::Highlight:QPalette::Mid));
        painter.drawEllipse(QPointF(16,16),4,4);
    }
};
