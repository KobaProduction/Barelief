#pragma once
#include <functional>
#include <QString>
class NodeParameterModel;
class QComboBox;
class QLineEdit;
namespace numeric_editor {
// Both the generic form and composite editors use the same draft/commit policy.
std::function<void()> bind_literal_input(NodeParameterModel*,const QString&,QComboBox*,QLineEdit*);
}
