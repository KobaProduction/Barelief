#pragma once

#include <QCoreApplication>
#include <QComboBox>
#include <QColorDialog>
#include <QDialogButtonBox>
#include <QPushButton>
#include <QLayout>
#include <QTabWidget>
#include <QGroupBox>
#include <type_traits>
#include <QTranslator>
#include <QPalette>
#include <QSignalBlocker>

// Presentation-only localization. Stable model/configuration keys are never translated.
namespace ui {
class Theme final : public QObject {
    Q_OBJECT
  public:
    static Theme &instance();
    QString code() const {
        return _code;
    }
    bool set(const QString &code);
    QPalette palette() const;
  signals:
    void changed();

  private:
    explicit Theme(QObject *parent);
    QString _code;
    QPalette _system;
    Qt::ColorScheme _system_scheme = Qt::ColorScheme::Unknown;
    bool _applying = false;
    void applyPalette();
};
class Language final : public QObject {
    Q_OBJECT
  public:
    static Language &instance();
    QString code() const {
        return _code;
    }
    bool set(const QString &code);
  signals:
    void changed();

  private:
    explicit Language(QObject *parent);
    QTranslator _translator;
    QString _code;
};
inline QString text(const char *source) {
    if (qstrcmp(source, "Pipeline") == 0)
        source = "Data Flow";
    if (qstrcmp(source, "System output") == 0)
        source = "System audio (output)";
    return QCoreApplication::translate("Ui", source);
}
inline QColor select_color(const QColor &initial, QWidget *parent, const QString &title, bool *use_default = nullptr,
                           const QColor &default_color = QColor{}) {
    // Qt's chooser follows the application palette, including explicit Light/Dark.
    if (!use_default)
        return QColorDialog::getColor(initial, parent, title, QColorDialog::DontUseNativeDialog);
    *use_default = false;
    QColorDialog dialog(initial, parent);
    dialog.setWindowTitle(title);
    dialog.setOption(QColorDialog::DontUseNativeDialog);
    QPushButton *reset = nullptr;
    if (auto *buttons = dialog.findChild<QDialogButtonBox *>())
        reset = buttons->addButton(text("Reset to default"), QDialogButtonBox::ResetRole);
    else {
        reset = new QPushButton(text("Reset to default"), &dialog);
        dialog.layout()->addWidget(reset);
    }
    reset->setObjectName("resetColorToDefault");
    bool reset_requested = false;
    QObject::connect(&dialog, &QColorDialog::currentColorChanged, &dialog, [&] { reset_requested = false; });
    QObject::connect(reset, &QPushButton::clicked, &dialog, [&] {
        dialog.setCurrentColor(default_color.isValid() ? default_color : initial);
        reset_requested = true;
    });
    if (dialog.exec() != QDialog::Accepted)
        return {};
    *use_default = reset_requested;
    return dialog.currentColor();
}
inline void bind(QObject *object, const char *property, const QString &source) {
    const QByteArray key(property), utf8 = source.toUtf8();
    auto &language = Language::instance();
    auto update = [object, key, utf8] { object->setProperty(key.constData(), text(utf8.constData())); };
    update();
    QObject::connect(&language, &Language::changed, object, update);
}
template <class Widget> Widget *make(const char *source, QWidget *parent) {
    auto *widget = new Widget(QString(), parent);
    bind(widget, std::is_same_v<Widget, QGroupBox> ? "title" : "text", QString::fromUtf8(source));
    return widget;
}
inline void items(QComboBox *combo, const QStringList &sources) {
    for (const auto &source : sources)
        combo->addItem(text(source.toUtf8().constData()));
    QObject::connect(&Language::instance(), &Language::changed, combo, [combo, sources] {
        const QSignalBlocker blocker(combo);
        for (int i = 0; i < sources.size(); ++i)
            combo->setItemText(i, text(sources[i].toUtf8().constData()));
    });
}
inline void tab(QTabWidget *tabs, QWidget *page, const char *source) {
    tabs->addTab(page, text(source));
    const QByteArray utf8(source);
    QObject::connect(&Language::instance(), &Language::changed, tabs, [tabs, page, utf8] {
        const int index = tabs->indexOf(page);
        if (index >= 0)
            tabs->setTabText(index, text(utf8.constData()));
    });
}
} // namespace ui
