#pragma once
class QWheelEvent;
namespace desktop_input {
// Install once; forms opt in with the scrollSafeControls property.
void protect_parameter_wheels();
void track_wheel_devices();
bool is_touchpad(const QWheelEvent &event);
bool is_pinch(const QWheelEvent &event);
}
