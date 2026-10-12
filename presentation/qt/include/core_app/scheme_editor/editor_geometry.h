#pragma once
#include "audio_node_model.h"
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
    double port_offset(QtNodes::NodeId id,QtNodes::PortType side,unsigned position) const {auto* n=model(id);const auto channels=n?n->grouped_channels(side):0;return position*pitch()+(channels?position/channels*24.:0.);}
    double side_span(QtNodes::NodeId id,QtNodes::PortType side) const {return count(id,side)?port_offset(id,side,count(id,side)-1):0.;}
    double radius() const {return setting("port_radius",7,5,14);}
    QRectF boundingRect(QtNodes::NodeId id) const override {const double margin=radius()+10;return QRectF(QPointF{},size(id)).adjusted(-margin,-margin,margin,margin);}
    double top(QtNodes::NodeId id) const {return captionRect(id).height()+labelRect(id).height()+24;}
    unsigned count(QtNodes::NodeId id,QtNodes::PortType side) const {return _graphModel.nodeData(id,side==QtNodes::PortType::In?QtNodes::NodeRole::InPortCount:QtNodes::NodeRole::OutPortCount).toUInt();}
    QString label(QtNodes::NodeId id,QtNodes::PortType side,unsigned port) const {return _graphModel.portData(id,side,port,QtNodes::PortRole::Caption).toString();}
    double text_width(QtNodes::NodeId id,QtNodes::PortType side) const {double width=0;QFontMetricsF metrics(label_font());for(unsigned port=0;port<count(id,side);++port)for(const auto& line:label(id,side,port).split('\n'))width=std::max(width,metrics.horizontalAdvance(line));return width+(side==QtNodes::PortType::In && model(id) && model(id)->name()=="InputSelector"?30:0);}
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
        const double width=std::max(captionRect(id).width()+32,text_width(id,QtNodes::PortType::In)+text_width(id,QtNodes::PortType::Out)+(body?body->width():0)+64);
        _graphModel.setNodeData(id,QtNodes::NodeRole::Size,QSize(int(std::ceil(width)),int(std::ceil(height))));
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
    QRectF selectorButtonRect(QtNodes::NodeId id,QtNodes::PortIndex port) const {
        auto* node=model(id);if(!node || node->name()!="InputSelector" || port>=count(id,QtNodes::PortType::In) || node->configuration_role(QtNodes::PortType::In,port))return {};
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
        painter->save();painter->setFont(geometry.label_font());const auto palette=QApplication::palette();
        for(auto side:{QtNodes::PortType::In,QtNodes::PortType::Out})for(unsigned port=0;port<geometry.count(node.nodeId(),side);++port){
            const bool connected=!node.graphModel().connections(node.nodeId(),side,port).empty();
            painter->setPen(QPen(palette.color(QPalette::Text),1));painter->setBrush(palette.color(connected?QPalette::Highlight:QPalette::Base));
            const auto pin=geometry.portPosition(node.nodeId(),side,port);
            auto* delegate=geometry.model(node.nodeId());const bool configuration=delegate && delegate->configuration_role(side,port).has_value();
            if(configuration){const auto r=geometry.radius();QPolygonF diamond;diamond<<pin+QPointF(0,-r)<<pin+QPointF(r,0)<<pin+QPointF(0,r)<<pin+QPointF(-r,0);painter->drawPolygon(diamond);}else painter->drawEllipse(pin,geometry.radius(),geometry.radius());
            const auto button=side==QtNodes::PortType::In?geometry.selectorButtonRect(node.nodeId(),port):QRectF{};
            if(!button.isEmpty()){
                QStyleOptionButton option;option.rect=button.toAlignedRect();option.palette=palette;option.fontMetrics=QFontMetrics(painter->font());option.text=geometry.label(node.nodeId(),side,port);option.state=QStyle::State_Enabled;
                auto* model=geometry.model(node.nodeId());const auto channels=model->numeric_node()->outputs.size();
                const auto input=model->port_id(side,port);const bool selected=model->selected_input()==(channels>1?input/channels:input);
                if(selected)option.state|=QStyle::State_On|QStyle::State_Sunken;else option.state|=QStyle::State_Raised;
                QApplication::style()->drawControl(QStyle::CE_PushButton,&option,painter);
                if(selected){painter->setPen(Qt::NoPen);painter->setBrush(palette.color(QPalette::Highlight));painter->drawEllipse(button.right()+6,button.center().y(),3,3);}
                continue;
            }
            painter->setPen(palette.color(QPalette::WindowText));const auto position=geometry.portTextPosition(node.nodeId(),side,port);const QFontMetricsF metrics(painter->font());
            const auto lines=geometry.label(node.nodeId(),side,port).split('\n');for(int i=0;i<lines.size();++i)painter->drawText(position+QPointF(0,metrics.ascent()+i*metrics.height()),lines[i]);
        }
        if(auto* delegate=geometry.model(node.nodeId())){
            painter->setPen(Qt::NoPen);painter->setBrush(palette.color(delegate->processing_measured()&&delegate->processing_active()?QPalette::Highlight:QPalette::Mid));painter->drawEllipse(QPointF(16,16),4,4);
        }
        painter->restore();drawProcessingIndicator(painter,node);drawValidationIcon(painter,node);
    }
};
