#pragma once
#include <visualization/quick2d/plot_model.h>
#include <visualization/ui_text.h>
#include <QAbstractListModel>
#include <QHash>
#include <cmath>
#include <algorithm>

// Stable GUI rows: value delivery changes only the affected roles, never the
// Repeater topology. Exact values remain strings throughout presentation.
class NumericMetricsModel final : public QAbstractListModel {
public:
    struct Input {quint16 port;QString name;};
    enum Role {PortRole=Qt::UserRole+1,NameRole,ExactRole,AvailableRole,BarRole};
    explicit NumericMetricsModel(QObject* parent):QAbstractListModel(parent) {
        connect(&ui::Language::instance(),&ui::Language::changed,this,[this]{
            for(int row=0;row<rows_.size();++row){
                QList<int> roles;
                if(rows_[row].input.name.isEmpty())roles.append(NameRole);
                if(!rows_[row].available)roles.append(ExactRole);
                if(!roles.isEmpty())emit dataChanged(index(row),index(row),roles);
            }
        });
    }
    int rowCount(const QModelIndex& parent={}) const override {return parent.isValid()?0:int(rows_.size());}
    QHash<int,QByteArray> roleNames() const override {
        return {{PortRole,"metricPort"},{NameRole,"metricName"},{ExactRole,"metricExact"},{AvailableRole,"metricAvailable"},{BarRole,"metricBar"}};
    }
    QVariant data(const QModelIndex& index,int role) const override {
        if(!index.isValid()||index.row()<0||index.row()>=rows_.size())return {};
        const auto& row=rows_[index.row()];
        switch(role){
        case PortRole:return row.input.port;
        case NameRole:return input_name(row.input.port);
        case ExactRole:return row.available?row.exact:ui::text("Renderer projection unavailable");
        case AvailableRole:return row.available;
        case BarRole:{bool ok=false;const double value=row.exact.toDouble(&ok);return ok&&std::isfinite(value)?std::clamp(value,-1.,1.):0.;}
        default:return {};
        }
    }
    void set_inputs(const QList<Input>& inputs) {
        bool topology=inputs.size()!=rows_.size();
        for(qsizetype i=0;!topology&&i<inputs.size();++i)topology=inputs[i].port!=rows_[i].input.port;
        if(topology){
            beginResetModel();rows_.clear();indices_.clear();rows_.reserve(inputs.size());
            for(const auto& input:inputs){indices_.insert(input.port,int(rows_.size()));rows_.append({input,{},false});}
            endResetModel();return;
        }
        for(int i=0;i<inputs.size();++i)if(rows_[i].input.name!=inputs[i].name){
            rows_[i].input.name=inputs[i].name;emit dataChanged(index(i),index(i),{NameRole});
        }
    }
    void set_value(quint16 port,const QString& value) {
        const int row=indices_.value(port,-1);if(row<0)return;
        auto& item=rows_[row];if(item.available&&item.exact==value)return;
        const bool availabilityChanged=!item.available;item.available=true;item.exact=value;
        QList<int> roles{ExactRole,BarRole};if(availabilityChanged)roles.append(AvailableRole);
        emit dataChanged(index(row),index(row),roles);
    }
    void set_unavailable(quint16 port) {
        const int row=indices_.value(port,-1);if(row<0||!rows_[row].available)return;
        rows_[row].available=false;rows_[row].exact.clear();
        emit dataChanged(index(row),index(row),{ExactRole,AvailableRole,BarRole});
    }
    void clear_values(){for(const auto& row:rows_)set_unavailable(row.input.port);}
    QString exact_value(quint16 port) const {const int row=indices_.value(port,-1);return row<0?QString{}:rows_[row].exact;}
    bool value_available(quint16 port) const {const int row=indices_.value(port,-1);return row>=0&&rows_[row].available;}
    QString input_name(quint16 port) const {
        const int row=indices_.value(port,-1);if(row<0)return {};
        const auto& name=rows_[row].input.name;return name.isEmpty()?ui::text("Value %1").arg(unsigned(port)+1):name;
    }
private:
    struct Row {Input input;QString exact;bool available;};
    QList<Row> rows_;QHash<quint16,int> indices_;
};

class NumericMetricsView final : public QuickPlotWidget {
public:
    using Input=NumericMetricsModel::Input;
    explicit NumericMetricsView(QWidget* parent=nullptr):QuickPlotWidget(parent),rows_(new NumericMetricsModel(this)){
        setObjectName("numericMetricsView");refresh_presentation();
    }
    void set_signed_bar(bool enabled){if(signed_bar_==enabled)return;signed_bar_=enabled;publish_metrics();}
    void set_inputs(const QList<Input>& inputs){rows_->set_inputs(inputs);}
    void set_value(quint16 port,const QString& value){rows_->set_value(port,value);}
    void set_unavailable(quint16 port){rows_->set_unavailable(port);}
    void clear_values(){rows_->clear_values();}
    QString exact_value(quint16 port) const {return rows_->exact_value(port);}
    bool value_available(quint16 port) const {return rows_->value_available(port);}
    QString input_name(quint16 port) const {return rows_->input_name(port);}
    int input_count() const {return rows_->rowCount();}
    NumericMetricsModel* metrics_model() const {return rows_;}
protected:
    void refresh_presentation() override {
        if(!rendering_enabled_||presentation_model()->state().value("kind")=="metrics")return;
        publish_metrics();
    }
private:
    void publish_metrics(){publish({{"kind","metrics"},{"signedBar",signed_bar_},{"metrics",QVariant::fromValue(static_cast<QObject*>(rows_))}});}
    bool signed_bar_=false;
    NumericMetricsModel* rows_;
};
