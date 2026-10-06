#pragma once
#include <QPoint>
#include <functional>
class QWidget;
namespace desktop_input {
// Windows Precision Touchpad recognizer; wheel fallback remains available.
void attach_touchpad(QWidget *viewport,std::function<bool(QPoint)> accepts_contact);
}
