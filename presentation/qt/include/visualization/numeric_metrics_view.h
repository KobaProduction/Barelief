#pragma once
#include <QWidget>
#include <QFormLayout>
#include <QLabel>
#include <QMap>
#include <QScrollArea>
#include <QVBoxLayout>
#include <visualization/ui_text.h>

// Current supplied values only. Input topology/names belong to the graph node;
// this widget owns neither samples nor processing/history state.
class NumericMetricsView final : public QWidget {
public:
    struct Input { quint16 port; QString name; };
    explicit NumericMetricsView(QWidget* parent=nullptr):QWidget(parent) {
        setObjectName("numericMetricsView");
        auto* layout=new QVBoxLayout(this);
        auto* scroll=new QScrollArea(this);scroll->setWidgetResizable(true);scroll->setFrameShape(QFrame::NoFrame);
        _body=new QWidget(scroll);_form=new QFormLayout(_body);
        _form->setRowWrapPolicy(QFormLayout::WrapLongRows);
        scroll->setWidget(_body);layout->addWidget(scroll);
        connect(&ui::Language::instance(),&ui::Language::changed,this,[this]{
            for(auto* label:_values)if(!label->property("metricAvailable").toBool())label->setText(ui::text("Renderer projection unavailable"));
            for(const auto& input:_inputs)if(input.name.isEmpty())_names.value(input.port)->setText(ui::text("Value %1").arg(unsigned(input.port)+1));
        });
    }
    void set_inputs(const QList<Input>& inputs) {
        bool same=inputs.size()==_inputs.size();
        for(qsizetype i=0;same && i<inputs.size();++i)same=inputs[i].port==_inputs[i].port && inputs[i].name==_inputs[i].name;
        if(same)return;
        while(_form->rowCount())_form->removeRow(0);
        _values.clear();_names.clear();_inputs=inputs;
        for(const auto& input:inputs) {
            auto* name=new QLabel(input.name.isEmpty()?ui::text("Value %1").arg(unsigned(input.port)+1):input.name,_body);
            name->setTextFormat(Qt::PlainText);name->setWordWrap(true);_names[input.port]=name;
            auto* value=new QLabel(_body);value->setObjectName("metricValue_"+QString::number(input.port));
            value->setTextFormat(Qt::PlainText);value->setWordWrap(true);
            value->setTextInteractionFlags(Qt::TextSelectableByMouse);
            value->setText(ui::text("Renderer projection unavailable"));
            _form->addRow(name,value);_values[input.port]=value;
        }
    }
    void set_value(quint16 port,const QString& value) {
        if(auto* label=_values.value(port)){
            // A literal value is user data, never a translation key.
            label->setProperty("metricAvailable",true);label->setProperty("metricExactText",value);label->setText(value);
        }
    }
    void set_unavailable(quint16 port) {
        if(auto* label=_values.value(port)){label->setProperty("metricAvailable",false);label->setProperty("metricExactText",QString{});label->setText(ui::text("Renderer projection unavailable"));}
    }
    void clear_values(){for(auto it=_values.cbegin();it!=_values.cend();++it)set_unavailable(it.key());}
private:
    QWidget* _body=nullptr;QFormLayout* _form=nullptr;
    QList<Input> _inputs;QMap<quint16,QLabel*> _values,_names;
};
