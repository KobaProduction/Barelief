#pragma once
#include <visualization/quick2d/plot_model.h>
#include <visualization/ui_text.h>
#include <QMap>
class NumericMetricsView final : public QuickPlotWidget {
public:
    struct Input {quint16 port;QString name;};
    explicit NumericMetricsView(QWidget* parent=nullptr):QuickPlotWidget(parent){setObjectName("numericMetricsView");refresh_presentation();}
    void set_inputs(const QList<Input>& inputs){
        bool same=inputs.size()==inputs_.size();for(qsizetype i=0;same&&i<inputs.size();++i)same=inputs[i].port==inputs_[i].port&&inputs[i].name==inputs_[i].name;
        if(same)return;inputs_=inputs;values_.clear();refresh_presentation();
    }
    void set_value(quint16 port,const QString& value){if(has_input(port)&&(!values_.contains(port)||values_.value(port)!=value)){values_[port]=value;refresh_presentation();}}
    void set_unavailable(quint16 port){values_.remove(port);refresh_presentation();}
    void clear_values(){values_.clear();refresh_presentation();}
    QString exact_value(quint16 port) const {return values_.value(port);}
    bool value_available(quint16 port) const {return values_.contains(port);}
    QString input_name(quint16 port) const {for(const auto& input:inputs_)if(input.port==port)return input.name.isEmpty()?ui::text("Value %1").arg(unsigned(port)+1):input.name;return {};}
    int input_count() const {return int(inputs_.size());}
protected:
    void refresh_presentation() override {
        if(!rendering_enabled_)return;
        QVariantList rows;for(const auto& input:inputs_)rows.append(QVariantMap{{"port",input.port},{"name",input_name(input.port)},
            {"exact",values_.contains(input.port)?values_.value(input.port):ui::text("Renderer projection unavailable")},{"available",values_.contains(input.port)}});
        publish({{"kind","metrics"},{"metrics",rows}});
    }
private:
    bool has_input(quint16 port) const {for(const auto& input:inputs_)if(input.port==port)return true;return false;}
    QList<Input> inputs_;QMap<quint16,QString> values_;
};
