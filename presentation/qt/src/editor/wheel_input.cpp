#include "core_app/wheel_input.h"
#include <QApplication>
#include <QAbstractNativeEventFilter>
#include <QPointer>
#include <QPointingDevice>
#include <QWheelEvent>
#include <QLoggingCategory>
#include <deque>
#include <optional>
#include <vector>
#ifdef Q_OS_WIN
#include <windows.h>
#endif

namespace desktop_input {
namespace {
Q_LOGGING_CATEGORY(wheelLog,"ui.input.wheel",QtWarningMsg)
// Qt's Windows wheel event may identify every device as the primary mouse.
// Preserve native device identity before Qt queues/converts the input message.
class WheelDevices final : public QObject, public QAbstractNativeEventFilter {
public:
    explicit WheelDevices(QObject *parent) : QObject(parent) {
        qApp->installNativeEventFilter(this);
#ifdef Q_OS_WIN
        // Physical mouse wheel packets have Raw Input counterparts. Precision
        // touchpad gestures are promoted to legacy wheel messages by Windows.
        // Do not replace another component's existing Raw Input registration.
        UINT count=0;
        if(GetRegisteredRawInputDevices(nullptr,&count,sizeof(RAWINPUTDEVICE))!=UINT(-1)) {
            std::vector<RAWINPUTDEVICE> registered(count);
            if(count && GetRegisteredRawInputDevices(registered.data(),&count,sizeof(RAWINPUTDEVICE))==UINT(-1)) return;
            for(const auto &entry:registered)
                if(entry.usUsagePage==1 && entry.usUsage==2) { raw_available=true; return; }
            RAWINPUTDEVICE mouse{1,2,0,nullptr}; // foreground only; retain legacy messages
            raw_available=owns_raw=RegisterRawInputDevices(&mouse,1,sizeof(mouse));
        }
#endif
    }
    ~WheelDevices() override {
        if(qApp) qApp->removeNativeEventFilter(this);
#ifdef Q_OS_WIN
        if(owns_raw) {
            RAWINPUTDEVICE mouse{1,2,RIDEV_REMOVE,nullptr};
            RegisterRawInputDevices(&mouse,1,sizeof(mouse));
        }
#endif
    }
    bool nativeEventFilter(const QByteArray &,void *message,qintptr *) override {
#ifdef Q_OS_WIN
        const auto *msg=static_cast<MSG *>(message);
        if(msg->message==WM_INPUT) {
            RAWINPUT raw{}; UINT size=sizeof(raw);
            if(GetRawInputData(HRAWINPUT(msg->lParam),RID_INPUT,&raw,&size,sizeof(RAWINPUTHEADER))!=UINT(-1) &&
               raw.header.dwType==RIM_TYPEMOUSE && raw.header.hDevice) {
                const auto flags=raw.data.mouse.usButtonFlags;
                if(flags&(RI_MOUSE_WHEEL|RI_MOUSE_HWHEEL)) {
                    raw_records.push_back({msg->time,short(raw.data.mouse.usButtonData),bool(flags&RI_MOUSE_HWHEEL)});
                    qCDebug(wheelLog)<<"raw wheel"<<msg->time<<short(raw.data.mouse.usButtonData)<<bool(flags&RI_MOUSE_HWHEEL);
                    if(raw_records.size()>64) raw_records.pop_front();
                }
            }
        }
        if(msg->message==WM_MOUSEWHEEL || msg->message==WM_MOUSEHWHEEL) {
            INPUT_MESSAGE_SOURCE source{};
            GetCurrentInputMessageSource(&source);
            records.push_back({msg->time,short(HIWORD(msg->wParam)),msg->message==WM_MOUSEHWHEEL,
                              source.deviceType==IMDT_TOUCHPAD, bool(LOWORD(msg->wParam)&MK_CONTROL)});
            qCDebug(wheelLog)<<"native wheel"<<msg->time<<short(HIWORD(msg->wParam))
                            <<(msg->message==WM_MOUSEHWHEEL)<<bool(LOWORD(msg->wParam)&MK_CONTROL)
                            <<int(source.deviceType)<<raw_available;
            if(records.size()>64) records.pop_front();
        }
#else
        Q_UNUSED(message);
#endif
        return false; // Keep native dispatch, controls and Qt's event routing.
    }
    struct Device { bool touchpad,pinch; };
    std::optional<Device> device(const QWheelEvent &event) const {
        if(!event.timestamp()) return {};
        for(auto it=records.rbegin();it!=records.rend();++it) {
            const auto delta=it->horizontal ? event.angleDelta().x() : event.angleDelta().y();
            if(it->time!=quint32(event.timestamp()) || (delta!=it->delta && delta!=-it->delta)) continue;
            // IMDT_MOUSE alone is not evidence of a physical wheel: Windows
            // uses it for touchpad scrolling too. Match actual Raw Input.
            for(auto raw=raw_records.rbegin();raw!=raw_records.rend();++raw)
                if(qint32(it->time-raw->time)>=-2 && qint32(it->time-raw->time)<=2 &&
                   raw->delta==it->delta && raw->horizontal==it->horizontal && !it->touchpad)
                    return Device{false,false};
            const bool touchpad=it->touchpad || raw_available || it->horizontal || it->control;
            return Device{touchpad,touchpad && it->control};
        }
        return {};
    }
private:
    struct Record { quint32 time; int delta; bool horizontal,touchpad,control; };
    struct RawRecord { quint32 time; int delta; bool horizontal; };
    std::deque<Record> records;
    std::deque<RawRecord> raw_records;
    bool raw_available=false,owns_raw=false;
};
QPointer<WheelDevices> devices;
}
void track_wheel_devices() { if(!devices) devices=new WheelDevices(qApp); }
bool is_touchpad(const QWheelEvent &event) {
    if(event.pointingDevice() && event.pointingDevice()->type()==QInputDevice::DeviceType::TouchPad) return true;
    if(devices) if(auto native=devices->device(event)) return native->touchpad;
    return !event.pixelDelta().isNull() || event.phase()!=Qt::NoScrollPhase ||
           event.source()==Qt::MouseEventSynthesizedBySystem || event.angleDelta().x()!=0;
}
bool is_pinch(const QWheelEvent &event) {
    if(devices) if(auto native=devices->device(event)) return native->pinch;
    return is_touchpad(event) && event.modifiers().testFlag(Qt::ControlModifier);
}
}
