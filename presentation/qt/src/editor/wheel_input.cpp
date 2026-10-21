#include "core_app/wheel_input.h"
#include <QApplication>
#include <QAbstractNativeEventFilter>
#include <QPointer>
#include <QPointingDevice>
#include <QWheelEvent>
#include <QLoggingCategory>
#include <QAbstractSpinBox>
#include <QComboBox>
#include <QScrollArea>
#include <QAbstractItemView>
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
        if(msg->message==WM_INPUT) record_raw_wheel(msg);
        if(msg->message==WM_MOUSEWHEEL || msg->message==WM_MOUSEHWHEEL) record_native_wheel(msg);
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
            if(physical_raw_match(*it)) return Device{false,false};
            const bool touchpad=it->touchpad || raw_available || it->horizontal || it->control;
            return Device{touchpad,touchpad && it->control};
        }
        return {};
    }
private:
    struct Record { quint32 time; int delta; bool horizontal,touchpad,control; };
    struct RawRecord { quint32 time; int delta; bool horizontal; };
    bool physical_raw_match(const Record& record) const {
        for(auto raw=raw_records.rbegin();raw!=raw_records.rend();++raw)
            if(qint32(record.time-raw->time)>=-2 && qint32(record.time-raw->time)<=2 &&
               raw->delta==record.delta && raw->horizontal==record.horizontal && !record.touchpad)
                return true;
        return false;
    }
#ifdef Q_OS_WIN
    void record_raw_wheel(const MSG *msg) {
        RAWINPUT raw{}; UINT size=sizeof(raw);
        if(GetRawInputData(HRAWINPUT(msg->lParam),RID_INPUT,&raw,&size,sizeof(RAWINPUTHEADER))==UINT(-1) ||
           raw.header.dwType!=RIM_TYPEMOUSE || !raw.header.hDevice) return;
        const auto flags=raw.data.mouse.usButtonFlags;
        if(!(flags&(RI_MOUSE_WHEEL|RI_MOUSE_HWHEEL))) return;
        raw_records.push_back({msg->time,short(raw.data.mouse.usButtonData),bool(flags&RI_MOUSE_HWHEEL)});
        qCDebug(wheelLog)<<"raw wheel"<<msg->time<<short(raw.data.mouse.usButtonData)<<bool(flags&RI_MOUSE_HWHEEL);
        if(raw_records.size()>64) raw_records.pop_front();
    }
    void record_native_wheel(const MSG *msg) {
        INPUT_MESSAGE_SOURCE source{};
        GetCurrentInputMessageSource(&source);
        records.push_back({msg->time,short(HIWORD(msg->wParam)),msg->message==WM_MOUSEHWHEEL,
                          source.deviceType==IMDT_TOUCHPAD, bool(LOWORD(msg->wParam)&MK_CONTROL)});
        qCDebug(wheelLog)<<"native wheel"<<msg->time<<short(HIWORD(msg->wParam))
                        <<(msg->message==WM_MOUSEHWHEEL)<<bool(LOWORD(msg->wParam)&MK_CONTROL)
                        <<int(source.deviceType)<<raw_available;
        if(records.size()>64) records.pop_front();
    }
#endif
    std::deque<Record> records;
    std::deque<RawRecord> raw_records;
    bool raw_available=false,owns_raw=false;
};
QPointer<WheelDevices>& wheel_devices() {
    static auto* instance=new QPointer<WheelDevices>;
    return *instance;
}
class ParameterWheels final : public QObject {
public:
    using QObject::QObject;
    bool eventFilter(QObject* object,QEvent* event) override {
        if(event->type()!=QEvent::Wheel)return false;
        auto* control=qobject_cast<QWidget*>(object);
        if(!control)return false;
        auto *selector=parameter_selector(control);
        if(!selector)return false;
        if(auto* combo=qobject_cast<QComboBox*>(selector);combo && combo->view()->isVisible())return false;
        return protect_selector(selector,event);
    }
private:
    static bool protect_selector(QWidget* selector,QEvent* event) {
        QScrollArea* scroll=nullptr;
        const bool protectedForm=protected_parent(selector,scroll);
        if(!protectedForm)return false;
        if(scroll) forward_wheel(scroll,*static_cast<QWheelEvent*>(event));
        event->accept();return true;
    }
    static QWidget* parameter_selector(QWidget *control) {
        auto *selector=control;
        while(selector && !qobject_cast<QComboBox*>(selector) && !qobject_cast<QAbstractSpinBox*>(selector))
            selector=selector->parentWidget();
        return selector;
    }
    static bool protected_parent(QWidget *selector,QScrollArea *&scroll) {
        bool protected_form=false;
        for(auto *parent=selector;parent;parent=parent->parentWidget()) {
            protected_form|=parent->property("scrollSafeControls").toBool();
            if(auto *area=qobject_cast<QScrollArea*>(parent)){scroll=area;break;}
        }
        return protected_form;
    }
    static void forward_wheel(QScrollArea *scroll,const QWheelEvent& wheel) {
        const auto pos=scroll->viewport()->mapFromGlobal(wheel.globalPosition().toPoint());
        QWheelEvent forwarded(pos,wheel.globalPosition(),wheel.pixelDelta(),wheel.angleDelta(),
            wheel.buttons(),wheel.modifiers(),wheel.phase(),wheel.inverted(),wheel.source(),wheel.pointingDevice());
        QApplication::sendEvent(scroll->viewport(),&forwarded);
    }
};
}
void protect_parameter_wheels() {
    static QPointer<ParameterWheels> policy;
    if(!policy){policy=new ParameterWheels(qApp);qApp->installEventFilter(policy);}
}
void track_wheel_devices() { if(!wheel_devices()) wheel_devices()=new WheelDevices(qApp); }
bool is_touchpad(const QWheelEvent &event) {
    if(event.pointingDevice() && event.pointingDevice()->type()==QInputDevice::DeviceType::TouchPad) return true;
    if(wheel_devices()) if(auto native=wheel_devices()->device(event)) return native->touchpad;
    return !event.pixelDelta().isNull() || event.phase()!=Qt::NoScrollPhase ||
           event.source()==Qt::MouseEventSynthesizedBySystem || event.angleDelta().x()!=0;
}
bool is_pinch(const QWheelEvent &event) {
    if(wheel_devices()) if(auto native=wheel_devices()->device(event)) return native->pinch;
    return is_touchpad(event) && event.modifiers().testFlag(Qt::ControlModifier);
}
}
