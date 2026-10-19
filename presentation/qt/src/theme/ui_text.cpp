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
namespace {
void set_active_palette_roles(QPalette& p,bool dark,const QColor& window,const QColor& base,const QColor& text,
                              const QColor& mid,const QColor& secondary) {
    p.setColor(QPalette::Window,window);p.setColor(QPalette::Base,base);p.setColor(QPalette::Button,window);
    p.setColor(QPalette::AlternateBase,dark ? QColor(0x30,0x34,0x3b) : QColor(0xe9,0xec,0xf0));
    for(auto role:{QPalette::WindowText,QPalette::Text,QPalette::ButtonText,QPalette::ToolTipText}) p.setColor(role,text);
    p.setColor(QPalette::PlaceholderText,secondary);p.setColor(QPalette::ToolTipBase,base);
    p.setColor(QPalette::Highlight,QColor(0x39,0x7d,0xcc));p.setColor(QPalette::HighlightedText,Qt::white);
    p.setColor(QPalette::Mid,mid);p.setColor(QPalette::Midlight,dark ? QColor(0x48,0x4e,0x58) : QColor(0xcc,0xd1,0xd9));
    p.setColor(QPalette::Dark,dark ? QColor(0x12,0x15,0x19) : QColor(0x72,0x79,0x84));
    p.setColor(QPalette::Light,dark ? QColor(0x55,0x5c,0x68) : QColor(Qt::white));p.setColor(QPalette::Shadow,dark ? Qt::black : QColor(0x42,0x49,0x54));
    p.setColor(QPalette::BrightText,Qt::white);p.setColor(QPalette::Link,QColor(0x6f,0x42,0xc1));p.setColor(QPalette::LinkVisited,QColor(0x59,0x32,0x9f));
}
void set_disabled_palette_roles(QPalette& p,const QColor& secondary,const QColor& mid,const QColor& text) {
    for(auto role:{QPalette::WindowText,QPalette::Text,QPalette::ButtonText,QPalette::PlaceholderText}) p.setColor(QPalette::Disabled,role,secondary);
    p.setColor(QPalette::Disabled,QPalette::Highlight,mid);p.setColor(QPalette::Disabled,QPalette::HighlightedText,text);
}
QPalette theme_palette(QPalette p,bool dark) {
    const QColor window=dark ? QColor(0x25,0x28,0x2d) : QColor(0xf4,0xf5,0xf7),base=dark ? QColor(0x19,0x1c,0x20) : QColor(Qt::white),
        text=dark ? QColor(0xe7,0xea,0xf0) : QColor(0x20,0x24,0x2a),mid=dark ? QColor(0x64,0x6a,0x74) : QColor(0xa6,0xad,0xb8),
        secondary=dark ? QColor(0x9d,0xa5,0xb2) : QColor(0x59,0x63,0x72);
    set_active_palette_roles(p,dark,window,base,text,mid,secondary);
    set_disabled_palette_roles(p,secondary,mid,text);
    return p;
}
QString theme_style_sheet(const QPalette& p) {
    const auto window=p.color(QPalette::Window);
    const auto text=p.color(QPalette::Text);
    const auto mid=p.color(QPalette::Mid);
    const auto secondary=p.color(QPalette::PlaceholderText);
    // Standard controls are drawn by Fusion from the inherited palette. Only
    // application navigation and dock chrome need centralized extra styling.
    return QStringLiteral(
        "QAbstractButton[compactControl=\"true\"] { background: %1; color: %2; border: 1px solid %3; border-radius: 3px; padding: 1px 6px; }"
        "QAbstractButton[compactControl=\"true\"]:hover { border-color: %5; background: %4; }"
        "QAbstractButton[compactControl=\"true\"]:checked, QAbstractButton[compactControl=\"true\"]:pressed { background: %4; }"
        "QAbstractButton[compactControl=\"true\"]:disabled { color: %6; }"
        "QMainWindow::separator { background: %3; width: 6px; height: 6px; }"
        "QMainWindow::separator:hover { background: %5; }"
        // Qt's dock animator also queues zero-duration animations when
        // AnimatedDocks is off. Restoring tab groups can delete their tab bars
        // before those callbacks run. Keep dock geometry updates synchronous.
        "QMainWindow QTabBar { widget-animation-duration: 0; }"
        "QDockWidget { border: 1px solid %3; } QDockWidget::title { padding: 4px; }"
        "QDockWidget[externalWindow=\"true\"] { border: 2px solid %3; }"
        "QMenuBar { border: none; border-bottom: 1px solid %3; padding: 3px 4px; }"
        "QWidget[dockControls=\"true\"] { border-bottom: 1px solid %3; }"
        "QToolButton[menuControl=\"true\"] { padding-right: 18px; }"
        "QToolButton[menuControl=\"true\"]::menu-indicator { subcontrol-origin: padding; subcontrol-position: center right; right: 5px; width: 7px; height: 7px; }"
        "QStatusBar::item { border: none; }")
        .arg(window.name(),text.name(),mid.name(),p.color(QPalette::AlternateBase).name(),p.color(QPalette::Highlight).name(),secondary.name());
}
}
void Theme::applyPalette() {
    const bool dark=_code=="dark" || (_code=="system" && (_system_scheme==Qt::ColorScheme::Dark ||
        (_system_scheme==Qt::ColorScheme::Unknown && _system.color(QPalette::Window).lightness()<128)));
    const auto palette=theme_palette(qApp->style()->standardPalette(),dark);
    QApplication::setPalette(palette);
    qApp->setStyleSheet(theme_style_sheet(palette));
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
