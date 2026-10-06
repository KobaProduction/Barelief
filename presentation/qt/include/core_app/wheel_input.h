#pragma once
class QWheelEvent;
namespace desktop_input {
void track_wheel_devices();
bool is_touchpad(const QWheelEvent &event);
bool is_pinch(const QWheelEvent &event);
}
