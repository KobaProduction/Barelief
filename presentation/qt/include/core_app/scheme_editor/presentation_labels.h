#pragma once
#include <barelief/ui_text.h>
#include <QJsonObject>

namespace numeric_editor {
inline QString presentation_label(const QJsonObject& presentation) {
    const auto custom=presentation.value("display_name").toString().trimmed();
    if(!custom.isEmpty())return custom;
    const auto key=presentation.value("display_name_key").toString().trimmed();
    return key.isEmpty()?QString{}:ui::text(key.toUtf8().constData());
}
}
