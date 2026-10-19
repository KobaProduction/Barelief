#pragma once
#include <QStringList>
#include <domain/processing_contracts.h>
namespace numeric_editor {
struct ParameterChoices {QStringList values,labels;};
bool parameter_is_primary(const QString& kind,const QString& key);
QString numeric_array_text(const domain::NumericArray&);
QString parameter_title(const QString& key);
ParameterChoices parameter_choices(const QString& kind,const QString& key);
}
