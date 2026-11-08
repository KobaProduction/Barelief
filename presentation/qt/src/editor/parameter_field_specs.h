#pragma once
#include <QStringList>
#include <domain/processing_contracts.h>
#include <domain/numeric/array_math.h>
namespace numeric_editor {
struct ParameterChoices {QStringList values,labels;};
domain::ArrayMathOperation array_operation(const domain::ConfiguredProcessingNode*);
const char* arithmetic_port_title(domain::ArrayMathOperation,bool input,bool left) noexcept;
ParameterChoices parameter_choices(const domain::ConfiguredProcessingNode&,const QString& key);
bool parameter_is_primary(const QString& kind,const QString& key);
QString numeric_array_text(const domain::NumericArray&);
QString parameter_title(const QString& key,const QString& kind={});
QString parameter_help(const QString& kind,const QString& key);
ParameterChoices parameter_choices(const QString& kind,const QString& key);
}
