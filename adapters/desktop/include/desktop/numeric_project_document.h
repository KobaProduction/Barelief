#pragma once
#include <domain/processing_contracts.h>
#include <project_storage/project_source.h>
#include <QByteArray>
#include <QByteArrayView>
#include <QString>
#include <QJsonObject>
#include <optional>

namespace desktop {
QString numeric_project_path(const QString& explicit_path = {});
struct NumericDocumentResult {
    QString error, path;
    explicit operator bool() const { return error.isEmpty(); }
};
// Known typed snapshots for editor clipboard/undo. Never a whole-project writer.
NumericDocumentResult encode_numeric_node(const domain::ConfiguredProcessingNode&, QByteArray&);
NumericDocumentResult decode_numeric_node(QByteArrayView, domain::ConfiguredProcessingNode&);
NumericDocumentResult encode_numeric_graph(const domain::NumericProcessingGraph&, QByteArray&);
NumericDocumentResult decode_numeric_graph(QByteArrayView, domain::NumericProcessingGraph&);
// New editor/preset snapshot boundary: canonical3 or typed QtNodes visual3.
// On failure out is unchanged. Whole-project scene_graph remains canonical3.
NumericDocumentResult decode_numeric_editor_graph(QByteArrayView, domain::NumericProcessingGraph&);
// Version1 preset display/workspace snapshot, excluding global preferences and
// processing. Unknown source fields are not projected; failure keeps out intact.
NumericDocumentResult decode_numeric_preset_presentation(QByteArrayView, QJsonObject&);
// Registered ApplicationAdapter routing metadata; partial drafts are allowed.
// No ProcessingParameters/control fields. Failure keeps out unchanged.
NumericDocumentResult decode_numeric_node_adapter(QByteArrayView, QJsonObject&);

// Local source ownership is separate from typed draft/applied state. Copying a
// document is explicit parsing via clone_numeric_project; graph copies own data.
struct NumericProjectDocument {
    project_storage::OwnedProjectSource source;
    QString active_path;
    QByteArray disk_baseline; // Exact loaded/committed bytes, never a Qt JSON tree.
};
NumericDocumentResult read_numeric_project(QByteArrayView, NumericProjectDocument&);
NumericDocumentResult clone_numeric_project(const NumericProjectDocument&, NumericProjectDocument&);
NumericDocumentResult read_numeric_draft(const NumericProjectDocument&, domain::NumericProcessingGraph&);
// A fresh project may have no applied graph. Absence clears out on success;
// malformed input leaves out unchanged. Loading never performs Apply.
NumericDocumentResult read_numeric_applied(const NumericProjectDocument&, std::optional<domain::NumericProcessingGraph>&);
// Only the supported numeric document/graph edition is admitted. Missing scene
// is allowed while composing a fresh document, but has no loadable draft.
NumericDocumentResult read_project_graph_edition(const NumericProjectDocument&, std::uint32_t&);
NumericDocumentResult read_numeric_application_settings(const NumericProjectDocument&, QJsonObject&);
NumericDocumentResult stage_numeric_draft(const NumericProjectDocument&, const domain::NumericProcessingGraph&, NumericProjectDocument&);
struct NumericDraftPresentation {
    QJsonObject positions; // Decimal node-ID keys -> {x,y}; known UI projection only.
    QJsonObject node_presentations; // Decimal node-ID keys -> registered presentation deltas: inline_parameters, display_name, scalar_inputs (full port-ID map).
    QJsonObject node_adapters; // Decimal node-ID keys -> FULL registered adapter snapshot; {} clears known fields.
    QJsonObject editor_view; // version1, scale, center_x, center_y.
    QJsonObject application_settings; // Explicit settings deltas; ui.pipeline_presets reconciles stable IDs against raw source.
    std::optional<domain::NumericProcessingGraph> applied_graph; // Explicit successfully applied snapshot; absent keeps existing applied source.
};
// Construct a fresh canonical document from explicit graph/presentation values.
// No filesystem access, load fallback or Apply; caller selects missing-file startup.
NumericDocumentResult create_numeric_project(const domain::NumericProcessingGraph&, const NumericDraftPresentation&,
                                             NumericProjectDocument&);
// Fresh first-run project with the supplied canonical graph as draft and a
// builtin-test-signal preset. Caller gates missing local config; no I/O/Apply.
NumericDocumentResult create_numeric_startup_project(const domain::NumericProcessingGraph&, NumericProjectDocument&);
NumericDocumentResult read_numeric_presentation(const NumericProjectDocument&, NumericDraftPresentation&);
// Supply only explicitly edited registered presentation fields, after staging
// the typed graph. Unknown descendants stay source-owned. No whole Qt graph dump.
NumericDocumentResult stage_numeric_presentation(const NumericProjectDocument&, const NumericDraftPresentation&,
                                                NumericProjectDocument&);
NumericDocumentResult load_numeric_project(const QString& path, NumericProjectDocument&, domain::NumericProcessingGraph&);
// Success alone publishes disk baseline/path. Existing changed/future/ambiguous
// destination refuses before QSaveFile replacement. Old editions are unsupported.
NumericDocumentResult save_numeric_project(NumericProjectDocument&, const QString& path = {});
} // namespace desktop
