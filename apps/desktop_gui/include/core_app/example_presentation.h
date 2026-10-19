#pragma once
#include <core_app/numeric_default_graph.h>
#include <QJsonObject>
#include <QString>
class NumericEditorGraphModel;
QJsonObject example_view_preferences(const NumericExampleGraph&,const QString& device,const QString& graph);
QJsonObject example_presentation(const NumericExampleGraph&,const QString& device,const QString& graph);
void apply_example_labels(NumericEditorGraphModel*,const NumericExampleGraph&);
