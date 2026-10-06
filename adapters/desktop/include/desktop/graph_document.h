#pragma once
#include <domain/graph_definition.h>
#include <QJsonObject>
#include <QString>
namespace desktop {
struct GraphDocument {
    domain::GraphDefinition graph;
    QJsonObject document; // Preserve layout, extensions and stable settings verbatim.
    QString error;
};
// Reads canonical version-2 documents. Legacy migration precedes this boundary.
GraphDocument read_graph_document(const QJsonObject&);
}
