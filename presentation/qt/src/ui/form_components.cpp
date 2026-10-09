#include "core_app/ui/field_row.h"
#include "core_app/ui/validation_message.h"
#include "core_app/ui/collapsible_section.h"
#include "core_app/ui/slider_value_control.h"
#include "visualization/ui_text.h"
#include <QHBoxLayout>
#include <QVBoxLayout>
#include <QToolButton>
#include <QSlider>
#include <QPushButton>

namespace ui {
ValidationMessage::ValidationMessage(QWidget *parent) : QLabel(parent) {
    setObjectName("validationMessage");
    setWordWrap(true);
    setTextFormat(Qt::PlainText);
    hide();
}
void ValidationMessage::setMessage(const QString &message) {
    setText(message);
    setAccessibleName(message);
    setVisible(!message.isEmpty());
}
FieldRow::FieldRow(const char *key, QWidget *editor, QWidget *parent)
    : FieldRow(QString::fromUtf8(key), editor, parent) {}
FieldRow::FieldRow(const QString &key, QWidget *editor, QWidget *parent)
    : QWidget(parent), _label(new QLabel(this)), _editor(editor), _help(new QLabel(this)),
      _error(new ValidationMessage(this)), _line(new QHBoxLayout) {
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(4);
    ui::bind(_label, "text", key);
    auto *focusTarget = editor->focusProxy() ? editor->focusProxy() : editor;
    _label->setBuddy(focusTarget);
    _label->setWordWrap(true);
    _line->addWidget(_label);
    _line->addWidget(editor, 1);
    layout->addLayout(_line);
    _help->setWordWrap(true);
    _help->setTextFormat(Qt::PlainText);
    _help->hide();
    layout->addWidget(_help);
    layout->addWidget(_error);
    auto updateAccessibility = [this, focusTarget] {
        _editor->setAccessibleName(_label->text());
        focusTarget->setAccessibleName(_label->text());
    };
    connect(&ui::Language::instance(), &ui::Language::changed, this, updateAccessibility);
    updateAccessibility();
}
void FieldRow::setHelp(const QString &help) {
    _help->setText(help);
    _help->setVisible(!help.isEmpty());
    _editor->setToolTip(help);
    _editor->setAccessibleDescription(help);
}
void FieldRow::setError(const QString &error) {
    _error->setMessage(error);
    _editor->setProperty("validationError", !error.isEmpty());
    _editor->setAccessibleDescription(error.isEmpty() ? _help->text() : error);
}
void FieldRow::setResetControl(QWidget *control) { _line->addWidget(control); }
CollapsibleSection::CollapsibleSection(const char *key, QWidget *parent)
    : CollapsibleSection(QString::fromUtf8(key), parent) {}
CollapsibleSection::CollapsibleSection(const QString &key, QWidget *parent)
    : QWidget(parent), _toggle(new QToolButton(this)), _content(new QWidget(this)),
      _layout(new QVBoxLayout(_content)) {
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(4);
    _toggle->setCheckable(true);
    _toggle->setChecked(true);
    _toggle->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    _toggle->setArrowType(Qt::DownArrow);
    ui::bind(_toggle, "text", key);
    ui::bind(_toggle, "accessibleName", key);
    _layout->setContentsMargins(12, 0, 0, 0);
    layout->addWidget(_toggle);
    layout->addWidget(_content);
    connect(_toggle, &QToolButton::toggled, this, [this](bool expanded) {
        _toggle->setArrowType(expanded ? Qt::DownArrow : Qt::RightArrow);
        _content->setVisible(expanded);
        emit expandedChanged(expanded);
    });
}
void CollapsibleSection::setExpanded(bool expanded) { _toggle->setChecked(expanded); }
bool CollapsibleSection::isExpanded() const { return _toggle->isChecked(); }
SliderValueControl::SliderValueControl(QWidget *parent)
    : QWidget(parent), _slider(new QSlider(Qt::Horizontal, this)), _value(new QLabel(this)),
      _reset(ui::make<QPushButton>("Reset", this)), _formatter([](int n) { return QString::number(n); }) {
    auto *layout = new QHBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    _value->setMinimumWidth(48);
    layout->addWidget(_slider, 1);
    layout->addWidget(_value);
    layout->addWidget(_reset);
    ui::bind(_reset, "toolTip", "Reset to default");
    _reset->hide();
    setFocusProxy(_slider);
    connect(_slider, &QSlider::valueChanged, this, [this](int n) { _value->setText(_formatter(n)); });
    connect(_reset, &QPushButton::clicked, this, [this] { _slider->setValue(_default); });
    _value->setText(_formatter(_slider->value()));
}
void SliderValueControl::setFormatter(std::function<QString(int)> formatter) {
    _formatter = std::move(formatter);
    _value->setText(_formatter(_slider->value()));
}
void SliderValueControl::setDefaultValue(int value) { _default = value; _reset->show(); }
}
