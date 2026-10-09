#pragma once
#include <domain/graph_definition.h>
#include <application/processing_control.h>
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
// Explicit typed mapper; raw JSON stays in document, view selectors stay local.
GraphDocument read_processing_graph_document(const QJsonObject&);
struct ProcessingProjectionResult {
    QJsonObject document;
    QString error;
    explicit operator bool() const {return error.isEmpty();}
};
ProcessingProjectionResult project_processing_graph(const QJsonObject&);
struct ProcessingDTOProjectionResult {
    application::ProcessingDTO processing;
    QString error;
    explicit operator bool() const {return error.isEmpty();}
};
// Strict typed control boundary. Only descriptor-registered processing/adapter
// fields cross it; raw extensions remain exclusively in the local document.
ProcessingDTOProjectionResult project_processing_dto(const QJsonObject&);
ProcessingProjectionResult processing_dto_document(const application::ProcessingDTO&);
// Control-plane projection only. Never use this to save a user document.
QJsonObject processing_graph_document(const QJsonObject&);
QJsonObject restore_graph_layout(const QJsonObject &processing,const QJsonObject &presentation);
}
