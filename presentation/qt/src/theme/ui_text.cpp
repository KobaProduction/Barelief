#include "visualization/ui_text.h"
#include <QGroupBox>
#include <QApplication>
#include <QStyle>
#include <QStyleHints>
#include <QWidget>
#include <QStyleFactory>
#include <QTimer>

static void initialize_translations() {
    Q_INIT_RESOURCE(ui_translations);
    Q_INIT_RESOURCE(ui_theme_icons);
}
namespace ui {
Theme::Theme(QObject *parent) : QObject(parent), _system(QApplication::palette()) {
    setObjectName("uiThemeService");
    qApp->setStyle(QStyleFactory::create("Fusion"));
    _system_scheme=QGuiApplication::styleHints()->colorScheme();
    connect(QGuiApplication::styleHints(),&QStyleHints::colorSchemeChanged,this,[this](Qt::ColorScheme scheme) {
        _system_scheme=scheme;
        if(_code=="system" && !_applying) QTimer::singleShot(0,this,[this]{if(_code=="system") applyPalette();});
    });
}
Theme &Theme::instance() {
    auto *service=qApp->findChild<Theme *>("uiThemeService",Qt::FindDirectChildrenOnly);
    if(!service) service=new Theme(qApp);return *service;
}
QPalette Theme::palette() const {return QApplication::palette();}
bool Theme::set(const QString &code) {
    if(code!="system" && code!="dark" && code!="light") return false;
    if(_code==code) return true;
    _code=code;_applying=true;
    auto *hints=QGuiApplication::styleHints();
    if(code=="system") hints->unsetColorScheme();
    else hints->setColorScheme(code=="dark" ? Qt::ColorScheme::Dark : Qt::ColorScheme::Light);
    if(code=="system") _system_scheme=hints->colorScheme();
    _applying=false;applyPalette();return true;
}
void Theme::applyPalette() {
    const bool dark=_code=="dark" || (_code=="system" && (_system_scheme==Qt::ColorScheme::Dark ||
        (_system_scheme==Qt::ColorScheme::Unknown && _system.color(QPalette::Window).lightness()<128)));
    QPalette p=qApp->style()->standardPalette();
    const QColor window=dark ? QColor("#25282d") : QColor("#f4f5f7"),base=dark ? QColor("#191c20") : QColor(Qt::white),
        text=dark ? QColor("#e7eaf0") : QColor("#20242a"),mid=dark ? QColor("#646a74") : QColor("#a6adb8"),
        secondary=dark ? QColor("#9da5b2") : QColor("#596372");
    p.setColor(QPalette::Window,window);p.setColor(QPalette::Base,base);p.setColor(QPalette::Button,window);
    p.setColor(QPalette::AlternateBase,dark ? QColor("#30343b") : QColor("#e9ecf0"));
    for(auto role:{QPalette::WindowText,QPalette::Text,QPalette::ButtonText,QPalette::ToolTipText}) p.setColor(role,text);
    p.setColor(QPalette::PlaceholderText,secondary);p.setColor(QPalette::ToolTipBase,base);
    p.setColor(QPalette::Highlight,QColor("#397dcc"));p.setColor(QPalette::HighlightedText,Qt::white);
    p.setColor(QPalette::Mid,mid);p.setColor(QPalette::Midlight,dark ? QColor("#484e58") : QColor("#ccd1d9"));
    p.setColor(QPalette::Dark,dark ? QColor("#121519") : QColor("#727984"));
    p.setColor(QPalette::Light,dark ? QColor("#555c68") : QColor(Qt::white));p.setColor(QPalette::Shadow,dark ? Qt::black : QColor("#424954"));
    p.setColor(QPalette::BrightText,Qt::white);p.setColor(QPalette::Link,QColor("#6f42c1"));p.setColor(QPalette::LinkVisited,QColor("#59329f"));
    for(auto role:{QPalette::WindowText,QPalette::Text,QPalette::ButtonText,QPalette::PlaceholderText}) p.setColor(QPalette::Disabled,role,secondary);
    p.setColor(QPalette::Disabled,QPalette::Highlight,mid);p.setColor(QPalette::Disabled,QPalette::HighlightedText,text);
    QApplication::setPalette(p);
    // Standard controls are drawn by Fusion from the inherited palette. Only
    // application navigation and dock chrome need centralized extra styling.
    const auto accent=p.color(QPalette::Link);
    qApp->setStyleSheet(QStringLiteral(
        "QToolButton#pipelineToggle { background: %7; color: %8; border: 1px solid %1; border-radius: 4px; padding: 3px 8px; font-weight: 600; }"
        "QToolButton#pipelineToggle:hover { background: %4; color: %2; }"
        "QToolButton#pipelineToggle:checked { background: %1; color: %2; }"
        "QToolButton#pipelineToggle:pressed { background: %5; color: %2; }"
        "QMainWindow::separator { background: %3; width: 6px; height: 6px; }"
        "QMainWindow::separator:hover { background: %6; }"
        "QMainWindow[singleDock=\"true\"]::separator { width: 0px; height: 0px; background: transparent; }"
        // Qt's dock animator also queues zero-duration animations when
        // AnimatedDocks is off. Restoring tab groups can delete their tab bars
        // before those callbacks run. Keep dock geometry updates synchronous.
        "QMainWindow QTabBar, QMainWindow QDockWidget { widget-animation-duration: 0; }"
        "QDockWidget { border: 1px solid %3; } QDockWidget::title { padding: 4px; }"
        "QMenuBar { border: none; border-bottom: 1px solid %3; padding: 3px 4px; }"
        "QWidget[dockControls=\"true\"] { border-bottom: 1px solid %3; }"
        "QToolButton#themeMenuButton, QToolButton#graphPlacement { border: 1px solid %3; border-radius: 4px; padding: 3px 20px 3px 8px; }"
        "QToolButton#themeMenuButton::menu-indicator, QToolButton#graphPlacement::menu-indicator { subcontrol-origin: padding; subcontrol-position: center right; right: 6px; width: 8px; height: 8px; }"
        "QStatusBar::item { border: none; }")
        .arg(accent.name(),p.color(QPalette::HighlightedText).name(),p.color(QPalette::Mid).name(),accent.lighter(115).name(),accent.darker(120).name(),p.color(QPalette::Highlight).name(),window.name(),text.name()));
    emit changed();
}

Language::Language(QObject *parent) : QObject(parent), _translator(this) {
    setObjectName("uiLanguageService");
    initialize_translations();
    set("ru");
}
Language &Language::instance() {
    auto *service = qApp->findChild<Language *>("uiLanguageService", Qt::FindDirectChildrenOnly);
    if (!service)
        service = new Language(qApp);
    return *service;
}
bool Language::set(const QString &code) {
    if (code != "ru" && code != "en")
        return false;
    if (_code == code)
        return true;
    if (code == "ru" && !_translator.load(":/i18n/ui_ru.qm"))
        return false;
    QCoreApplication::removeTranslator(&_translator);
    if (code == "ru")
        QCoreApplication::installTranslator(&_translator);
    _code = code;
    emit changed();
    return true;
}
} // namespace ui
