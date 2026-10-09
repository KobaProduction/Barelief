#pragma once
#include <QMetaType>

namespace ui {
enum class SettingsChangeScope { ApplicationPreferences, ProjectPresentation, SessionWorkspace, NodeAdapters };
struct SettingsChangeRequest {
    SettingsChangeScope scope = SettingsChangeScope::ApplicationPreferences;
    bool force_save = false;
};
}
Q_DECLARE_METATYPE(ui::SettingsChangeRequest)
