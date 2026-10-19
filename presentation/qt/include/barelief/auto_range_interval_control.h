#pragma once
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QLabel>
#include <QSignalBlocker>
#include <barelief/amplitude_range.h>
#include <barelief/ui_text.h>

namespace ui {
// Shared standard widgets inherit Theme; restoration emits no user edit.
class AutoRangeIntervalControl : public QWidget {
public:
    explicit AutoRangeIntervalControl(QWidget* parent=nullptr):QWidget(parent) {
        auto* layout=new QFormLayout(this);layout->setContentsMargins(0,0,0,0);
        layout->setRowWrapPolicy(QFormLayout::WrapAllRows);
        box=new QDoubleSpinBox(this);box->setObjectName("autoRangeIntervalSeconds");
        box->setRange(1,60);box->setDecimals(1);box->setValue(10);box->setKeyboardTracking(false);
        layout->addRow(ui::make<QLabel>("Auto range interval (s)",this),box);
    }
    void set_seconds(double seconds) {const QSignalBlocker blocker(box);stored_=AmplitudeRange::interval(seconds);box->setValue(stored_);displayed_=box->value();}
    double seconds() const {return box->value()==displayed_?stored_:box->value();}
    QDoubleSpinBox* box;
private:
    double stored_=10,displayed_=10;
};
}
