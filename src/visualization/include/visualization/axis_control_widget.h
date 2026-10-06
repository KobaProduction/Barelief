#pragma once
#include <QWidget>
class QCheckBox;
class QDoubleSpinBox;
class QLabel;

// Shared numerical axis editor; presentation/persistence remains with the caller.
class AxisControlWidget final : public QWidget {
    Q_OBJECT
  public:
    explicit AxisControlWidget(const char *title,QWidget *parent=nullptr);
    void set_bounds(double minimum,double maximum);
    void set_state(double minimum,double maximum,bool automatic,bool reverse);
    void set_hint(const QString &text);
    QDoubleSpinBox *minimum_box() const {return _minimum;}
    QDoubleSpinBox *maximum_box() const {return _maximum;}
    QCheckBox *auto_box() const {return _automatic;}
    QCheckBox *reverse_box() const {return _reverse;}
  signals:
    void range_changed(double minimum,double maximum,bool automatic);
    void reverse_changed(bool reverse);
  private:
    void apply_range(bool minimumChanged);
    QDoubleSpinBox *_minimum,*_maximum;
    QCheckBox *_automatic,*_reverse;
    QLabel *_hint;
};
