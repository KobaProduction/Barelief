#pragma once
#include <desktop/graph_document.h>

namespace desktop {
struct ProjectDisplaySettingsResult;
// Local provenance only. T3 associates this value with an accepted request;
// these adapters never track sessions, revisions or pending requests.
class ProjectDisplaySettings {
public:
    const QJsonObject& document() const {return document_;}
private:
    QJsonObject document_;
    friend struct ProjectDisplaySettingsResult;
    friend ProjectDisplaySettingsResult capture_project_display_settings(const QJsonObject&);
};
struct ProjectDisplaySettingsResult {
    ProjectDisplaySettings settings;
    QString error;
    explicit operator bool() const {return error.isEmpty();}
};
ProjectDisplaySettingsResult capture_project_display_settings(const QJsonObject&);
ProcessingProjectionResult merge_project_display_settings(
    const application::ProcessingDTO& accepted, const ProjectDisplaySettings& local);
}
