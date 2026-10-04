#include "core_app/settings_widget.h"
#include <QDialog>
#include <QApplication>
#include <QPushButton>
#include <QLabel>
#include <QGridLayout>
#include <QKeyEvent>
#include <QTimer>

namespace {
class CaptureKey final : public QDialog {
public:
    QJsonObject binding;
    explicit CaptureKey(const QString &action, QWidget *parent) : QDialog(parent) {
        setObjectName("captureCameraKey");
        setWindowTitle("Assign camera key");
        setMinimumWidth(360);
        auto *layout = new QVBoxLayout(this);
        auto *label = new QLabel(QString("Press a key for %1\nEscape cancels. Any keyboard layout works.").arg(action),this);
        label->setWordWrap(true);
        layout->addWidget(label);
        auto *cancel = new QPushButton("Cancel",this);
        layout->addWidget(cancel);
        connect(cancel,&QPushButton::clicked,this,&QDialog::reject);
        qApp->installEventFilter(this);
    }
    ~CaptureKey() override { qApp->removeEventFilter(this); }
protected:
    bool eventFilter(QObject *, QEvent *event) override {
        if (event->type() == QEvent::ShortcutOverride) { event->accept(); return true; }
        if (event->type() != QEvent::KeyPress) return false;
        auto *key = static_cast<QKeyEvent *>(event);
        if (key->isAutoRepeat()) return true;
        if (key->key() == Qt::Key_Escape) reject();
        else if (key->key() != Qt::Key_unknown) {
            binding = CameraControls::binding(CameraControls::scan(*key),key->key());
            accept();
        }
        return true;
    }
};
}

void SettingsWidget::_setup_camera_controls() {
    auto *group = new QGroupBox("Camera controls",this);
    group->setObjectName("cameraControls");
    group->setStyleSheet("QGroupBox#cameraControls { border: 1px solid palette(mid); border-radius: 8px; margin-top: 12px; padding: 14px 10px 10px; } QGroupBox#cameraControls::title { subcontrol-origin: margin; left: 12px; padding: 0 6px; } QGroupBox#cameraControls QPushButton { padding: 6px 12px; min-width: 76px; }");
    auto *layout = new QVBoxLayout(group);
    auto *help = new QLabel("Click a key to reassign it. Works in any keyboard layout.\nThe second column adds an alternative key for the same action.",group);
    help->setWordWrap(true);
    layout->addWidget(help);
    auto *grid = new QGridLayout;
    grid->setHorizontalSpacing(16);
    grid->setVerticalSpacing(6);
    grid->addWidget(new QLabel("Action",group),0,0);
    grid->addWidget(new QLabel("Primary",group),0,1);
    grid->addWidget(new QLabel("Alternative",group),0,2);
    const std::pair<const char *, const char *> actions[] = {{"forward","Forward"},{"backward","Backward"},
        {"left","Strafe left"},{"right","Strafe right"},{"up","Move up"},{"down","Move down"},
        {"fast","Move faster"},{"reset","Reset camera"}};
    _context.file_configs.ui.camera_controls = CameraControls::normalized(_context.file_configs.ui.camera_controls);
    auto refresh = [this,group] {
        for (auto *button : group->findChildren<QPushButton *>()) {
            const auto action = button->property("cameraAction").toString();
            if (action.isEmpty()) continue;
            const auto slot = button->property("cameraSlot").toInt();
            const auto keys = _context.file_configs.ui.camera_controls[action].toArray();
            const auto key = slot < keys.size() ? keys[slot].toObject() : QJsonObject{};
            button->setText(key.isEmpty() ? "Unassigned" : CameraControls::label(key["scan"].toInt(),key["key"].toInt()));
        }
    };
    auto *status = new QLabel(group);
    status->setWordWrap(true);
    status->setObjectName("cameraBindingStatus");
    int row = 1;
    for (auto [action,title] : actions) {
        grid->addWidget(new QLabel(title,group),row,0);
        for (int slot = 0; slot < 2; ++slot) {
            auto *button = new QPushButton(group);
            button->setObjectName(QString("bind_%1_%2").arg(action).arg(slot));
            button->setProperty("cameraAction",action);
            button->setProperty("cameraSlot",slot);
            if (slot == 1) {
                auto *alternate = new QHBoxLayout;
                alternate->addWidget(button);
                auto *clear = new QPushButton("Clear",group);
                clear->setObjectName(QString("clear_%1").arg(action));
                alternate->addWidget(clear);
                grid->addLayout(alternate,row,slot+1);
                connect(clear,&QPushButton::clicked,this,[this,action,refresh,status] {
                    auto keys = _context.file_configs.ui.camera_controls[action].toArray();
                    if (keys.size() < 2) return;
                    keys.removeAt(1);
                    _context.file_configs.ui.camera_controls[action] = keys;
                    refresh(); status->clear();
                    emit camera_controls_changed();
                    _on_config_change();
                });
            } else grid->addWidget(button,row,slot+1);
            connect(button,&QPushButton::clicked,this,[this,action,title,slot,refresh,status] {
                CaptureKey capture(title,this);
                if (capture.exec() != QDialog::Accepted) return;
                auto &controls = _context.file_configs.ui.camera_controls;
                for (auto it = controls.constBegin(); it != controls.constEnd(); ++it) {
                    const auto keys = it.value().toArray();
                    for (int i = 0; i < keys.size(); ++i) {
                        if (it.key() == action && i == slot) continue;
                        if (CameraControls::same(keys[i].toObject(),capture.binding)) {
                            status->setText("This key is already assigned. Choose another key.");
                            return;
                        }
                    }
                }
                auto keys = controls[action].toArray();
                if (slot < keys.size()) keys[slot] = capture.binding;
                else keys.append(capture.binding);
                controls[action] = keys;
                status->setText("Binding applied. Autosave or Ctrl+S saves it.");
                refresh();
                emit camera_controls_changed();
                _on_config_change();
            });
        }
        ++row;
    }
    grid->setColumnStretch(0,1);
    layout->addLayout(grid);
    layout->addWidget(status);
    auto *buttons = new QHBoxLayout;
    auto *defaults = new QPushButton("Restore default keys",group);
    defaults->setObjectName("restoreCameraKeys");
    auto *reset = new QPushButton("Reset camera position",group);
    reset->setObjectName("resetCameraFromSettings");
    buttons->addWidget(defaults);
    buttons->addWidget(reset);
    layout->addLayout(buttons);
    connect(defaults,&QPushButton::clicked,this,[this,refresh,status] {
        _context.file_configs.ui.camera_controls = CameraControls::defaults();
        refresh(); status->clear();
        emit camera_controls_changed();
        _on_config_change();
    });
    connect(reset,&QPushButton::clicked,this,&SettingsWidget::reset_camera_requested);
    _main_layout->insertWidget(2,group);
    refresh();
}
