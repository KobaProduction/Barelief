#include "visualization/ui_text.h"
#include "core_app/settings_widget.h"
#include "core_app/ui/field_row.h"
#include "core_app/ui/slider_value_control.h"
#include <QDialog>
#include <QApplication>
#include <QPushButton>
#include <QLabel>
#include <QGridLayout>
#include <QKeyEvent>
#include <QTimer>
#include <QSlider>
#include <QHBoxLayout>
#include <utility>

namespace {
class CaptureKey final : public QDialog {
  public:
    QJsonObject binding;
    explicit CaptureKey(const QString &action, QWidget *parent) : QDialog(parent) {
        setObjectName("captureCameraKey");
        ui::bind(this, "windowTitle", "Assign camera key");
        setMinimumWidth(360);
        auto *layout = new QVBoxLayout(this);
        auto *label =
            new QLabel(ui::text("Press a key for %1\nEscape cancels. Any keyboard layout works.").arg(action), this);
        label->setWordWrap(true);
        layout->addWidget(label);
        auto *cancel = ui::make<QPushButton>("Cancel", this);
        layout->addWidget(cancel);
        connect(cancel, &QPushButton::clicked, this, &QDialog::reject);
        qApp->installEventFilter(this);
    }
    ~CaptureKey() override {
        qApp->removeEventFilter(this);
    }

  protected:
    bool eventFilter(QObject *, QEvent *event) override {
        if (event->type() == QEvent::ShortcutOverride) {
            event->accept();
            return true;
        }
        if (event->type() != QEvent::KeyPress)
            return false;
        auto *key = static_cast<QKeyEvent *>(event);
        if (key->isAutoRepeat())
            return true;
        if (key->key() == Qt::Key_Escape)
            reject();
        else if (key->key() != Qt::Key_unknown && key->key()!=Qt::Key_Control) {
            binding = CameraControls::binding(CameraControls::scan(*key), key->key(),int(key->modifiers()));
            accept();
        }
        return true;
    }
};
QGridLayout* camera_binding_grid(QGroupBox* group) {
    auto *grid=new QGridLayout;
    grid->setHorizontalSpacing(16);
    grid->setVerticalSpacing(6);
    grid->addWidget(ui::make<QLabel>("Action",group),0,0);
    grid->addWidget(ui::make<QLabel>("Primary",group),0,1);
    grid->addWidget(ui::make<QLabel>("Alternative",group),0,2);
    return grid;
}
} // namespace

void SettingsWidget::_setup_camera_controls() {
    auto *group = ui::make<QGroupBox>("Camera controls", this);
    group->setObjectName("cameraControls");
    auto *layout = new QVBoxLayout(group);
    _setup_camera_speed(layout,group);
    _setup_camera_bindings(layout,group);
    _main_layout->insertWidget(2, group);
}

void SettingsWidget::_setup_camera_speed(QVBoxLayout *layout,QGroupBox *group) {
    auto *speedEditor = new ui::SliderValueControl(group);
    auto *speedRow = new ui::FieldRow("Movement speed", speedEditor, group);
    auto *speed = speedEditor->slider();
    speed->setObjectName("commonMovementSpeed");
    speed->setRange(50, 1000);
    speed->setSingleStep(5);
    speed->setPageStep(50);
    speedEditor->setFormatter([](int n) { return QString::number(n / 100.0, 'f', 2); });
    speedEditor->setDefaultValue(300);
    auto *speedValue = speedEditor->valueLabel();
    auto *speedReset = speedEditor->resetButton();
    speedReset->setObjectName("resetCommonMovementSpeed");
    ui::bind(speedRow, "toolTip", "Common movement speed for the camera and editor.");
    speed->setValue(qRound(CameraControls::movementSpeed(_context.file_configs.ui.camera_controls) * 100));
    speedValue->setText(QString::number(speed->value() / 100.0, 'f', 2));
    _bind_camera_speed(speed,speedValue);
    layout->addWidget(speedRow);
}

void SettingsWidget::_bind_camera_speed(QSlider *speed,QLabel *valueLabel) {
    connect(speed, &QSlider::valueChanged, this, [this, valueLabel](int value) {
        valueLabel->setText(QString::number(value / 100.0, 'f', 2));
        _context.file_configs.ui.camera_controls["movement_speed"] = value / 100.0;
        emit camera_controls_changed();
        emit presentation_preferences_changed();
        _on_config_change();
    });
}

void SettingsWidget::_refresh_camera_bindings(QGroupBox *group) {
    const auto buttons=group->findChildren<QPushButton *>();
    for (auto *button : buttons) {
        const auto action = button->property("cameraAction").toString();
        if (action.isEmpty())continue;
        const auto slot = button->property("cameraSlot").toInt();
        const auto keys = _context.file_configs.ui.camera_controls[action].toArray();
        const auto key = slot < keys.size() ? keys[slot].toObject() : QJsonObject{};
        button->setText(key.isEmpty() ? ui::text("Unassigned")
            : CameraControls::label(key["scan"].toInt(), key["key"].toInt(),key["modifiers"].toInt()));
    }
}

void SettingsWidget::_setup_camera_bindings(QVBoxLayout *layout,QGroupBox *group) {
    auto *help = ui::make<QLabel>("Click a key to reassign it. Works in any keyboard layout.\nThe second column adds "
                                  "an alternative key for the same action.",
                                  group);
    help->setWordWrap(true);
    layout->addWidget(help);
    auto *grid=camera_binding_grid(group);
    const std::pair<const char *, const char *> actions[] = {
        {"forward", "Forward"}, {"backward", "Backward"}, {"left", "Strafe left"}, {"right", "Strafe right"},
        {"up", "Move up"},      {"down", "Move down"},    {"fast", "Move faster"}, {"reset", "Reset camera"}};
    _context.file_configs.ui.camera_controls = CameraControls::normalized(_context.file_configs.ui.camera_controls);
    auto *status = new QLabel(group);
    status->setWordWrap(true);
    status->setObjectName("cameraBindingStatus");
    int row = 1;
    for (auto [action, title] : actions) {
        _add_camera_binding_row(grid,group,row,action,title,status);
        ++row;
    }
    grid->setColumnStretch(0, 1);
    layout->addLayout(grid);
    layout->addWidget(status);
    _setup_camera_footer(layout,group,status);
    auto refresh=[this,group]{_refresh_camera_bindings(group);};
    connect(&ui::Language::instance(), &ui::Language::changed, group, refresh);
    refresh();
}

void SettingsWidget::_add_camera_binding_row(QGridLayout *grid,QGroupBox *group,int row,
                                             const char *action,const char *title,QLabel *status) {
    auto *action_label=new QLabel(group);
    ui::bind(action_label,"text",QString::fromLatin1(title));
    grid->addWidget(action_label,row,0);
    auto *primary=_add_camera_binding_button(grid,group,row,action,title,0,status);
    action_label->setBuddy(primary);
    _add_camera_binding_button(grid,group,row,action,title,1,status);
}

QPushButton *SettingsWidget::_add_camera_binding_button(QGridLayout *grid,QGroupBox *group,int row,
                                                         const char *action,const char *title,int slot,QLabel *status) {
    auto *button=new QPushButton(group);
    button->setObjectName(QString("bind_%1_%2").arg(action).arg(slot));
    ui::bind(button,"accessibleName",QString::fromLatin1(title));
    button->setProperty("cameraAction",action);
    button->setProperty("cameraSlot",slot);
    if(slot==1) {
        auto *alternate=new QHBoxLayout;
        alternate->addWidget(button);
        auto *clear=ui::make<QPushButton>("Clear",group);
        clear->setObjectName(QString("clear_%1").arg(action));
        alternate->addWidget(clear);
        grid->addLayout(alternate,row,slot+1);
        connect(clear,&QPushButton::clicked,this,[this,action,group,status]{_clear_camera_alternate(action,group,status);});
    } else grid->addWidget(button,row,slot+1);
    connect(button,&QPushButton::clicked,this,[this,action,title,slot,group,status]{
        _assign_camera_binding(action,title,slot,group,status);
    });
    return button;
}

void SettingsWidget::_clear_camera_alternate(const char *action,QGroupBox *group,QLabel *status) {
    auto keys=_context.file_configs.ui.camera_controls[action].toArray();
    if(keys.size()<2)return;
    keys.removeAt(1);
    _context.file_configs.ui.camera_controls[action]=keys;
    _refresh_camera_bindings(group);
    status->clear();
    emit camera_controls_changed();
    _on_config_change();
}

bool SettingsWidget::_camera_binding_conflicts(const char *action,int slot,const QJsonObject& binding) const {
    const auto& controls=_context.file_configs.ui.camera_controls;
    for(auto it=controls.constBegin();it!=controls.constEnd();++it) {
        const auto keys=it.value().toArray();
        for(int index=0;index<keys.size();++index) {
            if(it.key()==action && index==slot)continue;
            if(CameraControls::same(keys[index].toObject(),binding))return true;
        }
    }
    return false;
}

void SettingsWidget::_assign_camera_binding(const char *action,const char *title,int slot,QGroupBox *group,QLabel *status) {
    CaptureKey capture(ui::text(title),this);
    if(capture.exec()!=QDialog::Accepted)return;
    if(_camera_binding_conflicts(action,slot,capture.binding)) {
        status->setText(ui::text("This key is already assigned. Choose another key."));
        return;
    }
    auto& controls=_context.file_configs.ui.camera_controls;
    auto keys=controls[action].toArray();
    if(slot<keys.size())keys[slot]=capture.binding;
    else keys.append(capture.binding);
    controls[action]=keys;
    status->setText(ui::text("Binding applied. Autosave or Ctrl+S saves it."));
    _refresh_camera_bindings(group);
    emit camera_controls_changed();
    _on_config_change();
}

void SettingsWidget::_setup_camera_footer(QVBoxLayout *layout,QGroupBox *group,QLabel *status) {
    auto *buttons = new QHBoxLayout;
    auto *defaults = ui::make<QPushButton>("Restore default keys", group);
    defaults->setObjectName("restoreCameraKeys");
    auto *reset = ui::make<QPushButton>("Reset camera position", group);
    reset->setObjectName("resetCameraFromSettings");
    buttons->addWidget(defaults);
    buttons->addWidget(reset);
    layout->addLayout(buttons);
    connect(defaults, &QPushButton::clicked, this, [this, group, status] {
        _context.file_configs.ui.camera_controls =
            CameraControls::restoreDefaultBindings(_context.file_configs.ui.camera_controls);
        _refresh_camera_bindings(group);
        status->clear();
        emit camera_controls_changed();
        _on_config_change();
    });
    connect(reset, &QPushButton::clicked, this, &SettingsWidget::reset_camera_requested);
}
