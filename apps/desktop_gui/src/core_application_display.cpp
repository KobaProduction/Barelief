#include <core_app/core_application.h>
#include <core_app/main_window.h>
#include <core_app/settings_widget.h>
#include <core_app/graph_panel.h>
#include <visualization/surface_settings.h>
#include <visualization/ui_text.h>
#include <QScreen>
#include <QWindow>
#include <QEvent>
#include <QTimer>
#include <QStatusBar>
#include <QScopedValueRollback>
#include <algorithm>
#ifdef Q_OS_WIN
#include <qt_windows.h>
#include <QtGui/qscreen_platform.h>
#endif

void CoreApplication::_bind_display_settings() {
    auto* settings=_main_window->findChild<SettingsWidget*>();if(!settings)return;
    _bind_display_rate_provider(settings);
    settings->set_display_current_rate_provider([this](QScreen* screen){return _current_display_rate(screen);});
    if(_display_session)settings->set_display_mode_requester([this](QScreen* screen,double rate){return _request_display_mode(screen,rate);});
    connect(settings,&SettingsWidget::cadence_changed,this,&CoreApplication::_synchronize_display_modes);
}
void CoreApplication::_bind_display_rate_provider(SettingsWidget* settings) {
    settings->set_display_rate_provider([this](QScreen* screen){
        SettingsWidget::DisplayRates result;
        if(!screen || !_display_api.query || !_display_api.enumerate)return result;
        const auto device=_display_device(screen).toStdWString();
        const auto current=_display_api.query(device);if(!current)return result;
        const auto native=desktop::refresh_rates_for_modes(*current,_display_api.enumerate(device));
        result.current=native.current;for(double rate:native.supported)result.supported.append(rate);
        return result;
    });
}
double CoreApplication::_current_display_rate(QScreen* screen) const {
    if(screen && _display_api.query)if(const auto mode=_display_api.query(_display_device(screen).toStdWString()))return mode->refresh_rate;
    return screen?screen->refreshRate():60.;
}
bool CoreApplication::_request_display_mode(QScreen* screen,double rate) {
    if(!screen)return false;
    const auto device=_display_device(screen);_managed_displays.insert(device);
    _synchronizing_displays=true;
    const bool applied=rate==0?_display_session->select_maximum(device.toStdWString()):_display_session->select(device.toStdWString(),rate);
    _synchronizing_displays=false;
    if(!applied){_main_window->statusBar()->showMessage(ui::text("Could not change the display refresh rate."),10000);_reconcile_display_mode(device);}
    return applied;
}
void CoreApplication::_bind_display_events() {
    _main_window->set_display_refresh_rate_provider([this](QScreen* screen){return _current_display_rate(screen);});
    installEventFilter(this);installNativeEventFilter(this);
    connect(this,&QGuiApplication::screenAdded,this,[this]{_display_catalog_changed();});
    connect(this,&QGuiApplication::screenRemoved,this,[this]{_display_catalog_changed();});
    QTimer::singleShot(0,this,[this]{if(_main_window)_watch_display_window(_main_window);_synchronize_display_modes();});
}
QString CoreApplication::_display_device(QScreen *screen) const {
    if(!screen)return {};
#ifdef Q_OS_WIN
    if(!_injected_display_api)
        if(auto* native=screen->nativeInterface<QNativeInterface::QWindowsScreen>())
            return QString::fromStdWString(desktop::display_device_for_monitor(reinterpret_cast<std::uintptr_t>(native->handle())));
#endif
    const auto name=screen->name();
    return name.isEmpty() && _injected_display_api?QStringLiteral("injected-display"):name;
}
bool CoreApplication::eventFilter(QObject *receiver,QEvent *event) {
    if(_main_window && (event->type()==QEvent::Show || event->type()==QEvent::Hide || event->type()==QEvent::ParentChange))
        if(auto* widget=qobject_cast<QWidget*>(receiver);widget &&
           (widget==_main_window || qobject_cast<GraphPanel*>(widget) || widget->findChild<GraphPanel*>())) {
            QPointer<QWidget> guarded=widget;
            QTimer::singleShot(0,this,[this,guarded]{if(guarded)_watch_display_window(guarded->window());_defer_display_sync();});
        }
    return QApplication::eventFilter(receiver,event);
}
bool CoreApplication::nativeEventFilter(const QByteArray&,void* message,qintptr*) {
#ifdef Q_OS_WIN
    if(message && static_cast<MSG*>(message)->message==WM_DISPLAYCHANGE)_display_catalog_changed();
#else
    (void)message;
#endif
    return false;
}
void CoreApplication::_display_catalog_changed() {
    if(!_main_window)return;
    if(_display_session)_display_session->invalidate_catalog();
    if(auto* settings=_main_window->findChild<SettingsWidget*>())settings->invalidate_display_catalog();
    _defer_display_sync();
}
void CoreApplication::_watch_display_window(QWidget *widget) {
    if(!widget || (widget!=_main_window && !widget->findChild<GraphPanel*>()))return;
    auto* window=widget->windowHandle();if(!window || _display_windows.contains(window))return;
    _display_windows.insert(window,widget);
    connect(window,&QWindow::screenChanged,this,[this](QScreen* screen){
        if(screen)connect(screen,&QScreen::geometryChanged,this,&CoreApplication::_display_catalog_changed,Qt::UniqueConnection);
        _display_catalog_changed();
    });
    if(window->screen())connect(window->screen(),&QScreen::geometryChanged,this,&CoreApplication::_display_catalog_changed,Qt::UniqueConnection);
    connect(window,&QObject::destroyed,this,[this,window]{_display_windows.remove(window);_synchronize_display_modes();});
}
void CoreApplication::_defer_display_sync() {
    if(_display_sync_queued)return;
    _display_sync_queued=true;
    QTimer::singleShot(0,this,[this]{_display_sync_queued=false;_synchronize_display_modes();});
}
void CoreApplication::_reconcile_display_mode(const QString& device,int attempts) {
    if(attempts==3 && _display_reconciliations.contains(device))return;
    _display_reconciliations.insert(device);
    QTimer::singleShot(100,this,[this,device,attempts]{_finish_display_reconciliation(device,attempts);});
}
void CoreApplication::_finish_display_reconciliation(const QString& device,int attempts) {
    if(!_main_window || !_display_session){_display_reconciliations.remove(device);return;}
    const bool releasing=!_required_displays.contains(device);
    const bool reconciled=releasing?_display_session->release(device.toStdWString()):_display_session->reconcile(device.toStdWString());
    if(releasing && reconciled)_managed_displays.remove(device);
    if(!reconciled && attempts>1){_reconcile_display_mode(device,attempts-1);return;}
    _display_reconciliations.remove(device);_refresh_display_cadence();
}
QSet<QString> CoreApplication::_required_display_devices(const QJsonObject& cadence) const {
    QSet<QString> required;
    const auto policy=cadence["gui_fps_mode"].toString();
    if(!_display_session || (policy!="maximum" && policy!="display"))return required;
    for(auto it=_display_windows.cbegin();it!=_display_windows.cend();++it){
        auto* window=it.key();auto* widget=it.value().data();
        if(!widget || !window->isVisible() || !window->screen())continue;
        const auto panels=widget->findChildren<GraphPanel*>();
        if(widget==_main_window || std::any_of(panels.begin(),panels.end(),[](auto* panel){return panel->isVisible();}))required.insert(_display_device(window->screen()));
    }
    return required;
}
void CoreApplication::_release_display_modes(const QSet<QString>& required) {
    for(const auto& device:_managed_displays-required)
        if(_display_session->release(device.toStdWString()))_managed_displays.remove(device);
        else _reconcile_display_mode(device);
}
void CoreApplication::_apply_display_modes(const QSet<QString>& required,const QJsonObject& cadence) {
    if(!_display_session)return;
    const auto policy=cadence["gui_fps_mode"].toString();
    for(const auto& device:required){
        _managed_displays.insert(device);
        const bool applied=policy=="maximum"?_display_session->select_maximum(device.toStdWString()):_display_session->select_or_maximum(device.toStdWString(),cadence["gui_target_fps"].toInt());
        if(!applied){_main_window->statusBar()->showMessage(ui::text("Could not change the display refresh rate."),10000);_reconcile_display_mode(device);}
    }
}
void CoreApplication::_refresh_display_cadence() {
    if(auto* settings=_main_window->findChild<SettingsWidget*>())settings->refresh_display_cadence();
    _main_window->refresh_display_cadence();
}
void CoreApplication::_synchronize_display_modes() {
    if(!_main_window || _synchronizing_displays)return;
    const QScopedValueRollback synchronizing(_synchronizing_displays,true);
    const auto cadence=SurfaceSettings::normalizedCadence(_ctx.file_configs.ui.graph_rendering);
    _required_displays=_required_display_devices(cadence);
    _release_display_modes(_required_displays);_apply_display_modes(_required_displays,cadence);
    _refresh_display_cadence();
}
