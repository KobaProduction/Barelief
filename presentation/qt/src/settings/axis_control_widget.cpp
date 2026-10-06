#include "visualization/axis_control_widget.h"
#include "visualization/ui_text.h"
#include <QDoubleSpinBox>
#include <QCheckBox>
#include <QLabel>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QSignalBlocker>
#include <cmath>

namespace {
class CompactAxisSpin final : public QDoubleSpinBox {
  public:
    explicit CompactAxisSpin(QWidget *parent):QDoubleSpinBox(parent) {
        setDecimals(15);setRange(-1e6,1e6);setSingleStep(.1);setMinimumWidth(0);
        setSizePolicy(QSizePolicy::Expanding,QSizePolicy::Fixed);setKeyboardTracking(false);
    }
    QSize sizeHint() const override {return {76,QDoubleSpinBox::sizeHint().height()};}
    QSize minimumSizeHint() const override {return {48,QDoubleSpinBox::minimumSizeHint().height()};}
  protected:
    QString textFromValue(double value) const override {return locale().toString(value,'g',8);}
};
}

AxisControlWidget::AxisControlWidget(const char *title,QWidget *parent):QWidget(parent) {
    setMinimumWidth(0);
    QSizePolicy policy(QSizePolicy::Expanding,QSizePolicy::Minimum);
    policy.setHeightForWidth(true);setSizePolicy(policy);
    auto *layout=new QVBoxLayout(this);layout->setContentsMargins(0,4,0,4);layout->setSpacing(4);
    auto *heading=ui::make<QLabel>(title,this);heading->setWordWrap(true);
    heading->setSizePolicy(QSizePolicy::Preferred,QSizePolicy::Minimum);layout->addWidget(heading);
    _hint=new QLabel(this);_hint->setWordWrap(true);
    _hint->setSizePolicy(QSizePolicy::Preferred,QSizePolicy::Minimum);_hint->hide();layout->addWidget(_hint);
    _automatic=ui::make<QCheckBox>("Auto",this);_reverse=ui::make<QCheckBox>("Reverse",this);
    ui::bind(_automatic,"toolTip","Automatic");ui::bind(_reverse,"toolTip","Reverse axis");
    _automatic->setObjectName("axisAutomatic");_reverse->setObjectName("axisReverse");
    layout->addWidget(_automatic);layout->addWidget(_reverse);
    _minimum=new CompactAxisSpin(this);_maximum=new CompactAxisSpin(this);
    _minimum->setObjectName("axisMinimum");_maximum->setObjectName("axisMaximum");
    auto *row=new QHBoxLayout;row->setSpacing(6);
    for(const auto &entry:{qMakePair("Min",_minimum),qMakePair("Max",_maximum)}) {
        auto *column=new QVBoxLayout;auto *label=ui::make<QLabel>(entry.first,this);label->setWordWrap(true);
        column->addWidget(label);column->addWidget(entry.second);row->addLayout(column,1);
    }
    layout->addLayout(row);set_state(0,1,true,false);
    connect(_minimum,&QDoubleSpinBox::valueChanged,this,[this]{apply_range(true);});
    connect(_maximum,&QDoubleSpinBox::valueChanged,this,[this]{apply_range(false);});
    connect(_automatic,&QCheckBox::toggled,this,[this](bool automatic){
        _minimum->setEnabled(!automatic);_maximum->setEnabled(!automatic);
        emit range_changed(_minimum->value(),_maximum->value(),automatic);
    });
    connect(_reverse,&QCheckBox::toggled,this,&AxisControlWidget::reverse_changed);
}
void AxisControlWidget::set_bounds(double minimum,double maximum) {
    if(!std::isfinite(minimum) || !std::isfinite(maximum) || minimum>=maximum) return;
    const QSignalBlocker b0(_minimum),b1(_maximum);
    _minimum->setRange(minimum,maximum);_maximum->setRange(minimum,maximum);
}
void AxisControlWidget::set_state(double minimum,double maximum,bool automatic,bool reverse) {
    const QSignalBlocker b0(_minimum),b1(_maximum),b2(_automatic),b3(_reverse);
    _minimum->setValue(minimum);_maximum->setValue(maximum);_automatic->setChecked(automatic);_reverse->setChecked(reverse);
    _minimum->setEnabled(!automatic);_maximum->setEnabled(!automatic);
}
void AxisControlWidget::set_hint(const QString &text) {_hint->setText(text);_hint->setVisible(!text.isEmpty());}
void AxisControlWidget::apply_range(bool minimumChanged) {
    double low=_minimum->value(),high=_maximum->value();
    if(high-low<1e-6) {
        if(minimumChanged && low+1e-6<=_maximum->maximum()) high=low+1e-6;
        else low=high-1e-6;
        const QSignalBlocker b0(_minimum),b1(_maximum);_minimum->setValue(low);_maximum->setValue(high);
        low=_minimum->value();high=_maximum->value();
    }
    if(high>low) emit range_changed(low,high,_automatic->isChecked());
}
