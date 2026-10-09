#include <desktop/project_display_settings.h>
#include <config/config_migration.h>
#include <QJsonArray>
#include <QSet>
#include <cmath>
#include <limits>

namespace desktop {
ProjectDisplaySettingsResult capture_project_display_settings(const QJsonObject& local) {
    ProjectDisplaySettingsResult result;
    // Draft processing may be invalid. Only provenance must be unambiguous.
    result.error=configuration::check_version(local,"schema_version",1);
    if(result.error.isEmpty())result.error=configuration::check_version(local,"version",2);
    if(result.error.isEmpty())result.error=configuration::check_version(local,"processing_version",1);
    if(!result.error.isEmpty())return result;
    if(!local.value("nodes").isArray() || !local.value("connections").isArray() ||
       (local.contains("graph_id") && (!local["graph_id"].isString() || local["graph_id"].toString().isEmpty()))) {
        result.error="Invalid display provenance";return result;
    }
    QSet<int> identities;
    for(const auto& value:local["nodes"].toArray()) {
        const auto node=value.toObject();const auto id=node["id"];
        const auto number=id.toDouble(-1);
        if(!id.isDouble() || !std::isfinite(number) || number<0 || number>=std::numeric_limits<int>::max() ||
           std::floor(number)!=number || identities.contains(int(number)) ||
           !node["internal-data"].toObject()["model-name"].isString()) {
            result.error="Invalid or ambiguous display node identity";return result;
        }
        identities.insert(int(number));
    }
    result.settings.document_=local;return result;
}
ProcessingProjectionResult merge_project_display_settings(
    const application::ProcessingDTO& accepted,const ProjectDisplaySettings& local) {
    auto result=processing_dto_document(accepted);
    if(!result)return result;
    // restore_graph_layout filters the remote side and matches graph/node/kind.
    result.document=restore_graph_layout(result.document,local.document());
    return result;
}
}
