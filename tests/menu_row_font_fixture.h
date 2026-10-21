#pragma once
#include <QDir>
#include <QFontDatabase>
#include <QFontMetrics>
#include <stdexcept>
#include <QWidget>

// Windows offscreen does not discover system fonts, unlike the desktop QPA.
// Use the same explicit native font baseline as owned widget-layout captures.
inline QFont menu_row_fixture_font() {
    const QString windowsDirectory = qEnvironmentVariable("WINDIR", "C:/Windows");
    const int id = QFontDatabase::addApplicationFont(QDir(windowsDirectory).filePath("Fonts/segoeui.ttf"));
    const auto families = QFontDatabase::applicationFontFamilies(id);
    if (id < 0 || families.isEmpty()) throw std::runtime_error("Menu layout fixture cannot load native Windows font");
    const QFont font(families.front(), 9);
    if (!QFontMetrics(font).inFontUcs4(0x0416)) throw std::runtime_error("Menu layout font lacks Cyrillic glyphs");
    return font;
}

inline void set_menu_row_fixture_font(QWidget *menuRow, const QFont &font) {
    // CoreApplication may already have polished children before the offscreen
    // font is registered. Set the owned row explicitly rather than relying on
    // a later top-level inherited-font change to replace cached style fonts.
    for (const auto children=menuRow->findChildren<QWidget *>();auto *child : children) child->setFont(font);
    menuRow->setFont(font);
}
