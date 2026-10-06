#include "core_app/touchpad_input.h"
#include <QApplication>
#include <QAbstractNativeEventFilter>
#include <QCursor>
#include <QLoggingCategory>
#include <QNativeGestureEvent>
#include <QPointingDevice>
#include <QPointer>
#include <QTimer>
#include <QWidget>
#include <cmath>
#include <atomic>
#ifdef Q_OS_WIN
#include <windows.h>
#include <directmanipulation.h>
#include <wrl/client.h>
#endif

namespace desktop_input {
#ifdef Q_OS_WIN
namespace {
Q_LOGGING_CATEGORY(touchpadLog,"ui.input.touchpad",QtWarningMsg)
using Microsoft::WRL::ComPtr;

// Consume the two-dimensional transform before Windows promotes it to wheel
// messages with directional rails. No inertia/rails configuration is enabled.
class Touchpad final : public QObject, public QAbstractNativeEventFilter {
    struct Handler final : IDirectManipulationViewportEventHandler {
        QPointer<Touchpad> owner;
        std::atomic<ULONG> refs{1};
        explicit Handler(Touchpad *owner) : owner(owner) {}
        HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid,void **out) override {
            if(!out) return E_POINTER;
            *out=nullptr;
            if(iid==__uuidof(IUnknown) || iid==__uuidof(IDirectManipulationViewportEventHandler)) {
                *out=static_cast<IDirectManipulationViewportEventHandler *>(this); AddRef(); return S_OK;
            }
            return E_NOINTERFACE;
        }
        ULONG STDMETHODCALLTYPE AddRef() override { return ++refs; }
        ULONG STDMETHODCALLTYPE Release() override { const auto count=--refs; if(!count) delete this; return count; }
        HRESULT STDMETHODCALLTYPE OnViewportStatusChanged(IDirectManipulationViewport *,DIRECTMANIPULATION_STATUS now,DIRECTMANIPULATION_STATUS previous) override {
            ComPtr<Handler> keep_alive(this);
            if(owner) owner->status(now,previous);
            return S_OK;
        }
        HRESULT STDMETHODCALLTYPE OnViewportUpdated(IDirectManipulationViewport *) override { return S_OK; }
        HRESULT STDMETHODCALLTYPE OnContentUpdated(IDirectManipulationViewport *,IDirectManipulationContent *content) override {
            ComPtr<Handler> keep_alive(this);
            if(owner) owner->transform(content);
            return S_OK;
        }
    };
public:
    explicit Touchpad(QWidget *target,std::function<bool(QPoint)> accepts) : QObject(target),target(target),accepts_contact(std::move(accepts)) {
        target->installEventFilter(this);
        qApp->installNativeEventFilter(this);
        timer.setInterval(8); timer.setTimerType(Qt::PreciseTimer);
        connect(&timer,&QTimer::timeout,this,[this] {
            QPointer<Touchpad> alive(this);
            auto retained=updates;
            if(!retained) { timer.stop(); return; }
            const auto result=retained->Update(nullptr);
            if(alive && !check("Update",result)) timer.stop();
        });
        if(target->isVisible()) schedule_setup();
    }
    ~Touchpad() override {
        if(qApp) qApp->removeNativeEventFilter(this);
        close();
        if(com_initialized) CoUninitialize();
    }
    bool eventFilter(QObject *watched,QEvent *event) override {
        if(watched==target) {
            if(event->type()==QEvent::Show || event->type()==QEvent::WinIdChange || event->type()==QEvent::ParentChange)
                schedule_setup();
            if(event->type()==QEvent::Hide) close();
        }
        if(event->type()==QEvent::Resize && viewport) resize();
        if(watched==host && event->type()==QEvent::WinIdChange) schedule_setup();
        return false;
    }
    bool nativeEventFilter(const QByteArray &,void *message,qintptr *) override {
        const auto *msg=static_cast<MSG *>(message);
        if(!viewport || msg->message!=DM_POINTERHITTEST || !target || !target->isVisible()) return false;
        if(msg->hwnd!=hwnd && GetAncestor(msg->hwnd,GA_ROOT)!=hwnd) return false;
        // QCursor converts Windows physical pixels to Qt logical coordinates.
        const auto position=target->mapFromGlobal(QCursor::pos());
        if(!target->rect().contains(position) || !accepts_contact(position)) return false;
        POINTER_INPUT_TYPE type{};
        const auto id=UINT32(LOWORD(msg->wParam));
        const bool begin=!timer.isActive();
        if(GetPointerType(id,&type) && type==PT_TOUCHPAD && check("SetContact",viewport->SetContact(id))) {
            if(begin) anchor=position;
            timer.start();
            qCDebug(touchpadLog)<<"touchpad contact"<<id;
        }
        return false;
    }
private:
    QPointer<QWidget> target,host;
    std::function<bool(QPoint)> accepts_contact;
    HWND hwnd=nullptr;
    ComPtr<IDirectManipulationManager> manager;
    ComPtr<IDirectManipulationUpdateManager> updates;
    ComPtr<IDirectManipulationViewport> viewport;
    Handler *handler=nullptr;
    DWORD cookie=0;
    unsigned generation=0;
    QTimer timer;
    bool com_initialized=false,setting_up=false,setup_pending=false,reset_pending=false,resetting=false,pinching=false;
    float last_scale=1;
    QPointF last_translation,anchor;
    RECT rect{};
    bool check(const char *operation,HRESULT result) {
        if(FAILED(result)) qCWarning(touchpadLog)<<operation<<"failed"<<Qt::hex<<quint32(result);
        else qCDebug(touchpadLog)<<operation<<"ok";
        return SUCCEEDED(result);
    }
    void schedule_setup() {
        if(setting_up || setup_pending) return;
        setup_pending=true;
        QTimer::singleShot(0,this,[this] { setup_pending=false; setup(); });
    }
    void close() {
        ++generation; reset_pending=false;
        timer.stop();
        if(handler) handler->owner=nullptr;
        if(viewport) {
            if(cookie) viewport->RemoveEventHandler(cookie);
            viewport->Stop(); viewport->Abandon();
        }
        if(handler) handler->Release();
        handler=nullptr; cookie=0;
        viewport.Reset(); updates.Reset();
        if(manager && hwnd) manager->Deactivate(hwnd);
        manager.Reset(); hwnd=nullptr;
        if(host) host->removeEventFilter(this);
        host=nullptr;
        last_scale=1; last_translation={}; pinching=false;
        if(target) target->setProperty("nativeTouchpadReady",false);
    }
    void setup() {
        if(!target || !target->isVisible()) return;
        setting_up=true;
        auto *current_host=target->window();
        const auto window=HWND(current_host->winId());
        setting_up=false;
        if(hwnd==window && viewport) { resize(); return; }
        close(); hwnd=window; host=current_host;
        host->installEventFilter(this);
        if(!com_initialized) {
            const HRESULT result=CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED);
            com_initialized=SUCCEEDED(result);
            if(FAILED(result) && result!=RPC_E_CHANGED_MODE) return;
        }
        HRESULT result=CoCreateInstance(__uuidof(DirectManipulationManager),nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(&manager));
        if(SUCCEEDED(result)) result=manager->GetUpdateManager(IID_PPV_ARGS(&updates));
        if(SUCCEEDED(result)) result=manager->CreateViewport(nullptr,hwnd,IID_PPV_ARGS(&viewport));
        const auto configuration=DIRECTMANIPULATION_CONFIGURATION_INTERACTION |
            DIRECTMANIPULATION_CONFIGURATION_TRANSLATION_X | DIRECTMANIPULATION_CONFIGURATION_TRANSLATION_Y |
            DIRECTMANIPULATION_CONFIGURATION_SCALING;
        if(SUCCEEDED(result)) result=viewport->ActivateConfiguration(configuration);
        if(SUCCEEDED(result)) result=viewport->SetViewportOptions(DIRECTMANIPULATION_VIEWPORT_OPTIONS_MANUALUPDATE |
                                                                 DIRECTMANIPULATION_VIEWPORT_OPTIONS_DISABLEPIXELSNAPPING);
        if(SUCCEEDED(result)) {
            handler=new Handler(this);
            result=viewport->AddEventHandler(hwnd,handler,&cookie);
        }
        if(SUCCEEDED(result)) {
            GetClientRect(hwnd,&rect);
            result=viewport->SetViewportRect(&rect);
        }
        if(SUCCEEDED(result)) result=manager->Activate(hwnd);
        if(SUCCEEDED(result)) result=viewport->Enable();
        if(FAILED(result)) {
            qCWarning(touchpadLog)<<"Native touchpad unavailable; retaining wheel input"<<Qt::hex<<quint32(result);
            close(); return;
        }
        target->setProperty("nativeTouchpadReady",true);
        qCDebug(touchpadLog)<<"Unrailed touchpad viewport ready";
    }
    void resize() {
        RECT size{}; GetClientRect(hwnd,&size);
        if(size.right==rect.right && size.bottom==rect.bottom) return;
        resetting=true; viewport->Stop(); timer.stop(); rect=size;
        viewport->SetViewportRect(&rect); reset();
    }
    void reset() {
        if(!viewport) return;
        resetting=true;
        check("Reset transform",viewport->ZoomToRect(0,0,float(rect.right),float(rect.bottom),FALSE));
        updates->Update(nullptr);
        last_scale=1; last_translation={}; pinching=false; resetting=false;
    }
    void send(Qt::NativeGestureType type,double value,QPointF delta) {
        if(!target) return;
        const auto position=type==Qt::ZoomNativeGesture ? anchor : QPointF();
        QNativeGestureEvent event(type,QPointingDevice::primaryPointingDevice(),2,position,position,
                                  target->mapToGlobal(position.toPoint()),value,delta);
        QApplication::sendEvent(target,&event);
    }
    void status(DIRECTMANIPULATION_STATUS now,DIRECTMANIPULATION_STATUS previous) {
        qCDebug(touchpadLog)<<"status"<<int(previous)<<"->"<<int(now);
        if(resetting) return;
        if(now==DIRECTMANIPULATION_RUNNING) timer.start();
        if(now==DIRECTMANIPULATION_READY || now==DIRECTMANIPULATION_INERTIA) {
            timer.stop();
            if(!reset_pending && (std::abs(last_scale-1)>1e-6 || !last_translation.isNull())) {
                reset_pending=true;
                const auto current_generation=generation;
                QTimer::singleShot(0,this,[this,current_generation] {
                    if(current_generation!=generation) return;
                    reset_pending=false; reset();
                });
            }
        }
    }
    void transform(IDirectManipulationContent *content) {
        float matrix[6]{};
        if(!check("GetContentTransform",content->GetContentTransform(matrix,6))) return;
        if(!resetting && target && host && std::isfinite(matrix[0]) && matrix[0]>0 &&
           std::isfinite(matrix[4]) && std::isfinite(matrix[5])) {
            const double ratio=matrix[0]/last_scale;
            const QPointF translation(matrix[4]/host->devicePixelRatioF(),matrix[5]/host->devicePixelRatioF());
            const auto delta=translation-last_translation;
            last_scale=matrix[0]; last_translation=translation;
            qCDebug(touchpadLog)<<"transform"<<matrix[0]<<translation<<"delta"<<delta;
            if(std::abs(matrix[0]-1)>1e-5) pinching=true;
            // Pinch changes translation too: do not also apply it as a pan.
            if(pinching) {
                if(std::abs(ratio-1)>1e-6) send(Qt::ZoomNativeGesture,ratio-1,{});
            } else if(!delta.isNull()) send(Qt::PanNativeGesture,0,delta);
        }
    }
};
}
#endif
void attach_touchpad(QWidget *viewport,std::function<bool(QPoint)> accepts_contact) {
#ifdef Q_OS_WIN
    if(QGuiApplication::platformName()=="windows") new Touchpad(viewport,std::move(accepts_contact));
#else
    Q_UNUSED(viewport);
    Q_UNUSED(accepts_contact);
#endif
}
}
