#include "core_app/scheme_editor/node_parameter_form.h"
#include "core_app/scheme_editor/node_parameter_model.h"
#include "core_app/scheme_editor/typed_parameter_helper.h"
#include "core_app/ui/field_row.h"
#include "core_app/ui/collapsible_section.h"
#include "core_app/wheel_input.h"
#include "visualization/ui_text.h"
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QComboBox>
#include <QCheckBox>
#include <QLineEdit>
#include <QSpinBox>
#include <QPushButton>
#include <QToolButton>
#include <QLabel>
#include <QMenu>
#include <QTimer>
#include <QSignalBlocker>
#include <QVariant>
#include <type_traits>

NodeParameterForm::NodeParameterForm(NodeParameterModel *model, bool inline_form, QWidget *parent)
    : QWidget(parent), _model(model), _inline_form(inline_form) {
    desktop_input::protect_parameter_wheels();
    setObjectName("numericNodeParameters");
    setProperty("scrollSafeControls", true);
    setProperty("inlineForm", inline_form);
    setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    connect(model, &NodeParameterModel::structureChanged, this, [this] {
        if (_rebuild_pending)
            return;
        _rebuild_pending = true;
        QTimer::singleShot(0, this, [this] {
            _rebuild_pending = false;
            rebuild();
        });
    });
    connect(&ui::Language::instance(), &ui::Language::changed, this, [this] {
        if (_model)
            emit _model->valuesChanged();
    });
    rebuild();
}
void NodeParameterForm::rebuild() {
    for (const auto &c : _bindings)
        disconnect(c);
    _bindings.clear();
    const auto children = findChildren<QWidget *>(QString{}, Qt::FindDirectChildrenOnly);
    for (auto *child : children)
        delete child;
    delete layout();
    if (!_model)
        return;
    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(6);
    auto* bypass=new QCheckBox(this);
    bypass->setObjectName("numericBypass");
    ui::bind(bypass,"text","Bypass");
    ui::bind(bypass,"toolTip","Skip processing. Compatible signals pass through; other outputs use typed silence.");
    bypass->setChecked(_model->bypassed());
    root->addWidget(bypass);
    _bindings.push_back(connect(_model,&NodeParameterModel::valuesChanged,bypass,[this,bypass]{
        if(!_model)return;
        QSignalBlocker block(bypass);bypass->setChecked(_model->bypassed());
    }));
    connect(bypass,&QCheckBox::toggled,bypass,[this](bool checked){if(_model)_model->setBypassed(checked);});
    QMenu *visible = nullptr;
    if (!_inline_form) {
        auto *button = new QToolButton(this);
        ui::bind(button, "text", "Visible on block");
        ui::bind(button, "toolTip", "Visible on block");
        button->setPopupMode(QToolButton::InstantPopup);
        visible = new QMenu(button);
        button->setMenu(visible);
        root->addWidget(button);
    }
    using M = NodeParameterModel;
    QMap<int, QVBoxLayout *> sections;
    auto binding = [this](const QMetaObject::Connection &c) { _bindings.push_back(c); };
    for (const auto &descriptor : _model->fields()) {
        // Copy the descriptor: edits can synchronously replace the model's field list.
        const auto f = descriptor;
        if (_inline_form && f.hide_inline)
            continue;
        QWidget *control = nullptr;
        std::function<void()> refresh;
        if (f.editor == M::Editor::Literal) {
            auto *box = new QWidget(this);
            auto *horizontal = new QHBoxLayout(box);
            horizontal->setContentsMargins(0, 0, 0, 0);
            auto *type = new QComboBox(box);
            type->addItems({"int64", "uint64", "float32", "float64"});
            type->setObjectName(f.key + "_type");
            auto *edit = new QLineEdit(box);
            edit->setObjectName(f.key);
            box->setFocusProxy(edit);
            horizontal->addWidget(type);
            horizontal->addWidget(edit);
            box->setObjectName(f.key + "_field");
            control = box;
            refresh = [this, f, type, edit] {
                if (!_model)
                    return;
                const auto *field = _model->field(f.key);
                if (!field || !field->read)
                    return;
                const auto state = _model->editState(f.key);
                QSignalBlocker blocker(type);
                if (state.dirty) {
                    type->setCurrentIndex(state.literal_type);
                    if (edit->text() != state.input)
                        edit->setText(state.input);
                    edit->setModified(true);
                    return;
                }
                if (edit->hasFocus() && edit->isModified())
                    return;
                const auto v = std::get<signal_contracts::NumericLiteral>(field->read());
                type->setCurrentIndex(int(v.index()));
                edit->setText(numeric_editor::literal_text(v));
                edit->setModified(false);
            };
            connect(edit, &QLineEdit::textEdited, box, [this, f, type](const QString &input) {
                if (_model)
                    _model->setDraftInput(f.key, input, type->currentIndex());
            });
            connect(edit, &QLineEdit::editingFinished, box, [this, f, type, edit] {
                if (_model && edit->isModified()) {
                    if (_model->editLiteral(f.key, edit->text(), type->currentIndex()))
                        edit->setModified(false);
                }
            });
            connect(type, &QComboBox::currentIndexChanged, box, [this, f, edit](int index) {
                if (_model && _model->editLiteral(f.key, edit->text(), index))
                    edit->setModified(false);
            });
        } else if (f.editor == M::Editor::Choice) {
            auto *combo = new QComboBox(this);
            control = combo;
            refresh = [this, f, combo] {
                if (!_model)
                    return;
                const auto *field = _model->field(f.key);
                if (!field || !field->read)
                    return;
                const auto value = field->read();
                QSignalBlocker blocker(combo);
                combo->clear();
                int selected = -1;
                for (std::size_t i = 0; i < field->options.size(); ++i) {
                    const auto &o = field->options[i];
                    auto label = o.translate ? ui::text(o.label.toUtf8().constData()) : o.label;
                    if (o.label == "Input %1")
                        label = label.arg(unsigned(std::get<int>(o.value)) + 1);
                    if (o.label == "Unavailable device: %1")
                        label = label.arg(std::get<QString>(o.value));
                    const auto data = std::visit(
                        [](const auto &value) -> QVariant {
                            using T = std::decay_t<decltype(value)>;
                            if constexpr (std::is_same_v<T, signal_contracts::NumericLiteral>)
                                return {};
                            else
                                return QVariant::fromValue(value);
                        },
                        o.value);
                    combo->addItem(label, data);
                    if (o.value == value)
                        selected = int(i);
                }
                combo->setCurrentIndex(selected);
            };
            connect(combo, &QComboBox::currentIndexChanged, combo, [this, f](int i) {
                if (!_model || i < 0)
                    return;
                const auto *field = _model->field(f.key);
                if (field && i < int(field->options.size())) {
                    const auto value = field->options[std::size_t(i)].value;
                    _model->edit(f.key, value);
                }
            });
        } else if (f.editor == M::Editor::Boolean) {
            auto *check = new QCheckBox(this);
            control = check;
            refresh = [this, f, check] {
                if (!_model)
                    return;
                const auto *field = _model->field(f.key);
                if (field && field->read) {
                    QSignalBlocker b(check);
                    check->setChecked(std::get<bool>(field->read()));
                }
            };
            connect(check, &QCheckBox::toggled, check, [this, f](bool v) {
                if (_model)
                    _model->edit(f.key, v);
            });
        } else if (f.editor == M::Editor::Integer) {
            auto *spin = new QSpinBox(this);
            control = spin;
            refresh = [this, f, spin] {
                if (!_model)
                    return;
                const auto *field = _model->field(f.key);
                if (field && field->read) {
                    QSignalBlocker b(spin);
                    spin->setRange(field->minimum, field->maximum);
                    spin->setValue(std::get<int>(field->read()));
                    if (f.key.startsWith("adapter"))
                        spin->setSpecialValueText(ui::text("Unspecified"));
                }
            };
            connect(spin, &QSpinBox::valueChanged, spin, [this, f](int v) {
                if (_model)
                    _model->edit(f.key, v);
            });
        } else if (f.editor == M::Editor::Text) {
            auto *edit = new QLineEdit(this);
            control = edit;
            refresh = [this, f, edit] {
                if (!_model)
                    return;
                const auto *field = _model->field(f.key);
                if (!field || !field->read)
                    return;
                const auto state = _model->editState(f.key);
                if (state.dirty) {
                    if (edit->text() != state.input)
                        edit->setText(state.input);
                    edit->setModified(true);
                    return;
                }
                edit->setText(std::get<QString>(field->read()));
                edit->setModified(false);
            };
            connect(edit, &QLineEdit::textEdited, edit, [this, f](const QString &input) {
                if (_model)
                    _model->setDraftInput(f.key, input);
            });
            connect(edit, &QLineEdit::editingFinished, edit, [this, f, edit] {
                if (_model && edit->isModified() && _model->edit(f.key, edit->text()))
                    edit->setModified(false);
            });
        } else if (f.editor == M::Editor::Caption) {
            auto *label = new QLabel(this);
            label->setWordWrap(true);
            label->setTextFormat(Qt::PlainText);
            control = label;
            refresh = [this, f, label] {
                if (!_model)
                    return;
                const auto *field = _model->field(f.key);
                if (field && field->read) {
                    const auto value = std::get<QString>(field->read());
                    label->setText(field->translate_value ? ui::text(value.toUtf8().constData()) : value);
                }
            };
        } else {
            auto *button = ui::make<QPushButton>(f.label.toUtf8().constData(), this);
            control = button;
            connect(button, &QPushButton::clicked, button, [this, f] {
                if (_model)
                    _model->edit(f.key, false);
            });
        }
        if (control->objectName().isEmpty())
            control->setObjectName(f.key);
        control->setMinimumWidth(0);
        control->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
        auto *row = new ui::FieldRow(f.label, control, this);
        row->setObjectName(f.key + "_row");
        row->setHelp(f.help.isEmpty() ? QString{} : ui::text(f.help.toUtf8().constData()));
        QAction *show = nullptr;
        if (visible && f.pinnable && !f.primary) {
            show = visible->addAction(ui::text(f.label.toUtf8().constData()));
            show->setCheckable(true);
            show->setObjectName("showOnBlock_" + f.key);
            ui::bind(show, "text", f.label);
            connect(show, &QAction::toggled, row, [this, f](bool v) {
                if (_model && !_model->setOnBlock(f.key, v))
                    emit _model->presentationChanged();
            });
        }
        auto rowState = [this, f, row, control, show] {
            if (!_model)
                return;
            const auto *field = _model->field(f.key);
            const bool available = field && (!field->available || field->available());
            control->setEnabled(available);
            const auto state = _model->editState(f.key);
            auto message = ui::text(state.error.toUtf8().constData());
            for (const auto &arg : state.error_arguments)
                message = message.arg(arg);
            row->setError(message);
            const bool pinned = _model->onBlock(f.key);
            if (show) {
                QSignalBlocker b(show);
                show->setChecked(pinned);
            }
            if (_inline_form)
                row->setVisible(pinned && !f.hide_inline);
        };
        if (_inline_form)
            root->addWidget(row);
        else {
            const int index = int(f.section);
            if (!sections.contains(index)) {
                auto *section = new ui::CollapsibleSection(M::sectionTitle(f.section), this);
                section->setObjectName(QString("parameterSection%1").arg(index));
                section->setExpanded(_model->expanded(f.section));
                root->addWidget(section);
                sections[index] = section->contentLayout();
                connect(section, &ui::CollapsibleSection::expandedChanged, section, [this, f](bool v) {
                    if (_model)
                        _model->setExpanded(f.section, v);
                });
            }
            sections[index]->addWidget(row);
        }
        if (refresh) {
            binding(connect(_model, &M::valuesChanged, row, refresh));
            binding(connect(_model, &M::editStateChanged, row, [refresh, key = f.key](const QString &changed) {
                if (changed == key)
                    refresh();
            }));
            refresh();
        }
        binding(connect(_model, &M::valuesChanged, row, rowState));
        binding(connect(_model, &M::presentationChanged, row, rowState));
        binding(connect(_model, &M::editStateChanged, row, [rowState, key = f.key](const QString &changed) {
            if (changed == key)
                rowState();
        }));
        rowState();
    }
    updateGeometry();
}
