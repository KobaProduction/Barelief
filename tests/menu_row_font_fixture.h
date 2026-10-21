#pragma once
#include <QDir>
#include <QFontDatabase>
#include <QFontMetrics>
#include <stdexcept>
#include <QWidget>

// Pin a native font with Cyrillic coverage on each desktop test platform.
// This makes layout metrics independent of the QPA's fallback font selection.
inline QFont menu_row_fixture_font() {
#ifdef Q_OS_WIN
    const QString fontFile = QDir(qEnvironmentVariable("WINDIR", "C:/Windows")).filePath("Fonts/segoeui.ttf");
#else
    const QString fontFile = QStringLiteral("/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf");
#endif
    const int id = QFontDatabase::addApplicationFont(fontFile);
    const auto families = QFontDatabase::applicationFontFamilies(id);
    if (id < 0 || families.isEmpty()) throw std::runtime_error("Menu layout fixture cannot load native platform font");
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
