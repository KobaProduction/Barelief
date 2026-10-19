#include "barelief/axis_control_widget.h"
#include "barelief/ui_text.h"
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
    setup_heading(layout,title);
    setup_options(layout);
    setup_range_boxes(layout);
    set_state(0,1,true,false);
    bind_controls();
}
void AxisControlWidget::setup_heading(QVBoxLayout* layout,const char* title) {
    auto *heading=ui::make<QLabel>(title,this);heading->setWordWrap(true);
    heading->setSizePolicy(QSizePolicy::Preferred,QSizePolicy::Minimum);layout->addWidget(heading);
    _hint=new QLabel(this);_hint->setWordWrap(true);
    _hint->setSizePolicy(QSizePolicy::Preferred,QSizePolicy::Minimum);_hint->hide();layout->addWidget(_hint);
}
void AxisControlWidget::setup_options(QVBoxLayout *layout) {
    _automatic=ui::make<QCheckBox>("Auto",this);_reverse=ui::make<QCheckBox>("Reverse",this);
    ui::bind(_automatic,"toolTip","Automatic");ui::bind(_reverse,"toolTip","Reverse axis");
    _automatic->setObjectName("axisAutomatic");_reverse->setObjectName("axisReverse");
    layout->addWidget(_automatic);layout->addWidget(_reverse);
}
void AxisControlWidget::setup_range_boxes(QVBoxLayout *layout) {
    _minimum=new CompactAxisSpin(this);_maximum=new CompactAxisSpin(this);
    _minimum->setObjectName("axisMinimum");_maximum->setObjectName("axisMaximum");
    auto *row=new QHBoxLayout;row->setSpacing(6);
    for(const auto &entry:{qMakePair("Min",_minimum),qMakePair("Max",_maximum)}) {
        auto *column=new QVBoxLayout;auto *label=ui::make<QLabel>(entry.first,this);label->setWordWrap(true);
        label->setBuddy(entry.second);
        ui::bind(entry.second,"accessibleName",entry.first);
        column->addWidget(label);column->addWidget(entry.second);row->addLayout(column,1);
    }
    layout->addLayout(row);
}
void AxisControlWidget::bind_controls() {
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
    const double step=std::max(1e-15,(maximum-minimum)/10);
    _minimum->setSingleStep(step);_maximum->setSingleStep(step);
}
void AxisControlWidget::set_hint(const QString &text) {_hint->setText(text);_hint->setVisible(!text.isEmpty());}
void AxisControlWidget::apply_range(bool minimumChanged) {
    double low=_minimum->value(),high=_maximum->value();
    if(high<=low) {
        const double step=std::max(1e-15,std::max(_minimum->singleStep(),_maximum->singleStep()));
        if(minimumChanged && low+step<=_maximum->maximum()) high=low+step;
        else low=high-step;
        const QSignalBlocker b0(_minimum),b1(_maximum);_minimum->setValue(low);_maximum->setValue(high);
        low=_minimum->value();high=_maximum->value();
    }
    if(high>low) emit range_changed(low,high,_automatic->isChecked());
}
