#include "visualization/ui_text.h"
#include <QGroupBox>
#include <QApplication>
#include <QStyle>

static void initialize_translations() { Q_INIT_RESOURCE(ui_translations); }
namespace ui {
Theme::Theme(QObject *parent) : QObject(parent), _system(QApplication::palette()) { setObjectName("uiThemeService"); }
Theme &Theme::instance() {
    auto *service=qApp->findChild<Theme *>("uiThemeService",Qt::FindDirectChildrenOnly);
    if (!service) service=new Theme(qApp);
    return *service;
}
bool Theme::set(const QString &code) {
    if (code!="system" && code!="dark" && code!="light") return false;
    if (_code==code) return true;
    QPalette palette=_system;
    if (code=="dark" || code=="light") {
        const bool dark=code=="dark";
        palette.setColor(QPalette::Window,dark ? QColor("#25282d") : QColor("#f4f5f7"));
        palette.setColor(QPalette::Base,dark ? QColor("#191c20") : Qt::white);
        palette.setColor(QPalette::AlternateBase,dark ? QColor("#30343b") : QColor("#e9ecf0"));
        palette.setColor(QPalette::Button,palette.color(QPalette::Window));
        for (auto role : {QPalette::WindowText,QPalette::Text,QPalette::ButtonText,QPalette::ToolTipText})
            palette.setColor(role,dark ? QColor("#e7eaf0") : QColor("#20242a"));
        palette.setColor(QPalette::ToolTipBase,palette.color(QPalette::Base));
        palette.setColor(QPalette::Highlight,QColor("#397dcc"));
        palette.setColor(QPalette::HighlightedText,Qt::white);
        for (auto role : {QPalette::WindowText,QPalette::Text,QPalette::ButtonText})
            palette.setColor(QPalette::Disabled,role,dark ? QColor("#858b96") : QColor("#9096a0"));
    }
    QApplication::setPalette(palette);
    _code=code; emit changed(); return true;
}

Language::Language(QObject *parent) : QObject(parent), _translator(this) {
    setObjectName("uiLanguageService");
    initialize_translations();
    set("ru");
}
Language &Language::instance() {
    auto *service = qApp->findChild<Language *>("uiLanguageService",Qt::FindDirectChildrenOnly);
    if (!service) service=new Language(qApp);
    return *service;
}
bool Language::set(const QString &code) {
    if (code!="ru" && code!="en") return false;
    if (_code==code) return true;
    if (code=="ru" && !_translator.load(":/i18n/ui_ru.qm")) return false;
    QCoreApplication::removeTranslator(&_translator);
    if (code=="ru") QCoreApplication::installTranslator(&_translator);
    _code=code;
    emit changed();
    return true;
}
}
