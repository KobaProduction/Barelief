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
#include "oscillator_bank_widget.h"
#include "literal_edit_binding.h"
#include <QHeaderView>
#include <QDoubleSpinBox>
#include <QSignalBlocker>
#include <QVariant>
#include <type_traits>

NodeParameterForm::NodeParameterForm(NodeParameterModel *model, bool inline_form, QWidget *parent)
    : QWidget(parent), _model(model), _inline_form(inline_form) {
    desktop_input::protect_parameter_wheels();
    setObjectName("numericNodeParameters");
    setProperty("scrollSafeControls", true);
    setProperty("inlineForm", inline_form);
    setSizePolicy(inline_form ? QSizePolicy::Preferred : QSizePolicy::Ignored, QSizePolicy::Preferred);
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
    if(!_inline_form)bypass->setSizePolicy(QSizePolicy::Ignored,QSizePolicy::Fixed);
    bypass->setObjectName("numericBypass");
    ui::bind(bypass,"text","Bypass");
    ui::bind(bypass,"toolTip","Skip processing. Compatible signals pass through; other outputs use typed silence.");
    bypass->setChecked(_model->bypassed());
    root->addWidget(bypass,0,Qt::AlignRight);
    _bindings.push_back(connect(_model,&NodeParameterModel::valuesChanged,bypass,[this,bypass]{
        if(!_model)return;
        QSignalBlocker block(bypass);bypass->setChecked(_model->bypassed());
    }));
    connect(bypass,&QCheckBox::toggled,bypass,[this](bool checked){if(_model)_model->setBypassed(checked);});
    ui::CollapsibleSection *placement = nullptr;
    if (!_inline_form) {
        placement = new ui::CollapsibleSection("Fields on block", this);
        placement->setObjectName("fieldsOnBlockSection");
        placement->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
        if (auto *toggle = placement->findChild<QToolButton *>())
            toggle->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
        placement->setExpanded(_placement_expanded);
        connect(placement, &ui::CollapsibleSection::expandedChanged, this,
                [this](bool expanded) { _placement_expanded = expanded; });

    }
    using M = NodeParameterModel;
    QMap<int, QVBoxLayout *> sections;
    auto binding = [this](const QMetaObject::Connection &c) { _bindings.push_back(c); };
    if (!_inline_form && _model->field("oscillatorCount"))
        root->addWidget(new OscillatorBankWidget(_model, this));
    for (const auto &descriptor : _model->fields()) {
        // Copy the descriptor: edits can synchronously replace the model's field list.
        const auto f = descriptor;
        if(!_inline_form && f.key.startsWith("oscillator") && f.key!="oscillatorCount")continue;
        if (_inline_form && f.hide_inline)
            continue;
        QWidget *control = nullptr;
        std::function<void()> refresh;
        if (f.editor == M::Editor::Literal) {
            auto *box = new QWidget(this);
            auto *horizontal = new QHBoxLayout(box);
            horizontal->setContentsMargins(0, 0, 0, 0);
            if (!_inline_form)
                horizontal->setDirection(QBoxLayout::TopToBottom);
            auto *type = new QComboBox(box);
            type->addItems({"int64", "uint64", "float32", "float64"});
            type->setObjectName(f.key + "_type");
            if(f.literal_type){type->setCurrentIndex(*f.literal_type);type->hide();}
            auto *edit = new QLineEdit(box);
            edit->setObjectName(f.key);
            box->setFocusProxy(edit);
            horizontal->addWidget(type);
            horizontal->addWidget(edit);
            box->setObjectName(f.key + "_field");
            control = box;
            refresh = numeric_editor::bind_literal_input(_model, f.key, type, edit);
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
                    if (o.label == "Input %1" || o.label == "Stream %1" || o.label == "Output %1")
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
        // Keep native editors' minimum size hints in the embedded body's
        // geometry. Ignored allowed selectors and literal type boxes to shrink
        // below their usable width when another inline field was added.
        auto controlPolicy = control->sizePolicy();
        controlPolicy.setHorizontalPolicy(_inline_form ? QSizePolicy::Preferred : QSizePolicy::Ignored);
        control->setSizePolicy(controlPolicy);
        auto *row = new ui::FieldRow(f.label, control, this);
        if(f.key.startsWith("mixerWeight") || f.key.startsWith("mixerShare")){
            const bool weight=f.key.startsWith("mixerWeight");const auto id=f.key.mid(weight?11:10).toUInt();
            auto caption=[row,weight,id]{row->label()->setText(ui::text(weight?"Input %1 weight":"Input %1 share").arg(id+1));};caption();binding(connect(&ui::Language::instance(),&ui::Language::changed,row,caption));
        }
        if (!_inline_form) {
            // Inspector width belongs to its splitter/viewport. Stack the label
            // and editor so a narrow inspector does not inherit the combined
            // horizontal minimum intended for embedded node controls.
            auto *line = qobject_cast<QBoxLayout *>(row->layout()->itemAt(0)->layout());
            line->setDirection(QBoxLayout::TopToBottom);
            line->setAlignment(Qt::AlignTop);
            row->layout()->setAlignment(Qt::AlignTop);
            row->label()->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
            row->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
        }
        row->setObjectName(f.key + "_row");
        row->setHelp(f.help.isEmpty() ? QString{} : ui::text(f.help.toUtf8().constData()));
        QAction *show = nullptr;
        if (placement && f.pinnable && !f.primary) {
            show = new QAction(placement);
            show->setCheckable(true);
            show->setObjectName("showOnBlock_" + f.key);
            ui::bind(show, "text", f.label);
            auto *check = new QCheckBox(placement);
            check->setSizePolicy(QSizePolicy::Ignored,QSizePolicy::Fixed);
            check->setObjectName("fieldOnBlock_" + f.key);
            ui::bind(check, "text", f.label);
            placement->contentLayout()->addWidget(check);
            connect(check, &QCheckBox::toggled, show, &QAction::setChecked);
            connect(show, &QAction::toggled, check, &QCheckBox::setChecked);
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
                // Action signals are blocked during model synchronization.
                if (auto *check = findChild<QCheckBox *>("fieldOnBlock_" + f.key)) {
                    QSignalBlocker checkboxBlocker(check);
                    check->setChecked(pinned);
                }
            }
            if (_inline_form) {
                const bool visible=pinned && !f.hide_inline;
                if(!row->property("parameterVisibilityInitialized").toBool() || !row->isHidden()!=visible){row->setProperty("parameterVisibilityInitialized",true);row->setVisible(visible);row->updateGeometry();if(layout())layout()->invalidate();updateGeometry();}
            }
        };
        if (_inline_form)
            root->addWidget(row);
        else {
            const int index = int(f.section);
            if (!sections.contains(index)) {
                auto *section = new QWidget(this);
                section->setObjectName(QString("parameterSection%1").arg(index));
                section->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
                auto *content = new QVBoxLayout(section);
                content->setContentsMargins(0, 0, 0, 0);
                content->setAlignment(Qt::AlignTop);
                auto *title = ui::make<QLabel>(M::sectionTitle(f.section), section);
                title->setWordWrap(true);
                title->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
                content->addWidget(title);
                root->addWidget(section);
                sections[index] = content;
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
    if(!_inline_form)root->addWidget(placement);
    // A tall inspector has spare space below the fields. Keep section, label,
    // help and validation geometry driven by content rather than distributing
    // that space between controls.
    if (!_inline_form)
        root->addStretch(1);
    updateGeometry();
}
