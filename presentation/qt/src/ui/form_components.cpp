#include "core_app/ui/field_row.h"
#include "core_app/ui/wrapping_button.h"
#include "core_app/ui/validation_message.h"
#include "core_app/ui/collapsible_section.h"
#include "core_app/ui/slider_value_control.h"
#include "barelief/ui_text.h"
#include <QHBoxLayout>
#include <QVBoxLayout>
#include <QToolButton>
#include <QSlider>
#include <QPushButton>
#include <QStylePainter>
#include <QStyleOptionButton>
#include <QStyle>

namespace ui {
WrappingButton::WrappingButton(const QString& text,QWidget* parent) : QPushButton(text,parent) {
    auto policy=QSizePolicy(QSizePolicy::Preferred,QSizePolicy::Preferred);
    policy.setHeightForWidth(true);
    setSizePolicy(policy);
}
QSize WrappingButton::buttonSize(const QSize& contents) const {
    QStyleOptionButton option;initStyleOption(&option);
    return style()->sizeFromContents(QStyle::CT_PushButton,&option,contents,this);
}
QSize WrappingButton::sizeHint() const {
    return buttonSize(fontMetrics().size(Qt::TextShowMnemonic,text()));
}
QSize WrappingButton::minimumSizeHint() const {
    int width=0;
    const auto words=text().split(' ',Qt::SkipEmptyParts);
    for(const auto& word:words)width=qMax(width,fontMetrics().size(Qt::TextShowMnemonic,word).width());
    return buttonSize(QSize(width,fontMetrics().height()));
}
int WrappingButton::heightForWidth(int width) const {
    QStyleOptionButton option;initStyleOption(&option);option.rect=QRect(0,0,width,1000);
    const auto contents=style()->subElementRect(QStyle::SE_PushButtonContents,&option,this);
    const auto bounds=fontMetrics().boundingRect(QRect(0,0,qMax(1,contents.width()),1000),Qt::TextWordWrap|Qt::TextShowMnemonic,text());
    return buttonSize(bounds.size()).height();
}
void WrappingButton::paintEvent(QPaintEvent*) {
    QStylePainter painter(this);QStyleOptionButton option;initStyleOption(&option);
    const auto title=option.text;option.text.clear();
    painter.drawControl(QStyle::CE_PushButton,option);
    auto contents=style()->subElementRect(QStyle::SE_PushButtonContents,&option,this);
    if(option.state.testFlag(QStyle::State_Sunken))contents.translate(style()->pixelMetric(QStyle::PM_ButtonShiftHorizontal,&option,this),style()->pixelMetric(QStyle::PM_ButtonShiftVertical,&option,this));
    int flags=Qt::AlignCenter|Qt::TextWordWrap|Qt::TextShowMnemonic;
    if(!style()->styleHint(QStyle::SH_UnderlineShortcut,&option,this))flags|=Qt::TextHideMnemonic;
    style()->drawItemText(&painter,contents,flags,option.palette,option.state.testFlag(QStyle::State_Enabled),title,QPalette::ButtonText);
}
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
    auto *focusTarget = setupLabel(key, editor);
    _line->addWidget(_label);
    _line->addWidget(editor, 1);
    layout->addLayout(_line);
    _help->setWordWrap(true);
    _help->setTextFormat(Qt::PlainText);
    _help->hide();
    layout->addWidget(_help);
    layout->addWidget(_error);
    setupAccessibility(focusTarget);
}
QWidget *FieldRow::setupLabel(const QString &key, QWidget *editor) {
    ui::bind(_label, "text", key);
    auto *focusTarget = editor->focusProxy() ? editor->focusProxy() : editor;
    _label->setBuddy(focusTarget);
    _label->setWordWrap(true);
    return focusTarget;
}
void FieldRow::setupAccessibility(QWidget *focusTarget) {
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
