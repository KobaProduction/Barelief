#pragma once
#include <QWidget>
#include <QString>
#include <functional>
class QSlider;
class QLabel;
class QPushButton;
namespace ui {
// Reusable editor; bounds, defaults and display units are supplied by the form.
class SliderValueControl final : public QWidget {
  public:
    explicit SliderValueControl(QWidget *parent = nullptr);
    [[nodiscard]] QSlider *slider() const { return _slider; }
    [[nodiscard]] QLabel *valueLabel() const { return _value; }
    [[nodiscard]] QPushButton *resetButton() const { return _reset; }
    void setFormatter(std::function<QString(int)> formatter);
    void setDefaultValue(int value);
  private:
    QSlider *_slider;
    QLabel *_value;
    QPushButton *_reset;
    std::function<QString(int)> _formatter;
    int _default = 0;
};
}
