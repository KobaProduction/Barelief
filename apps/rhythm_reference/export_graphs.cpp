// SPDX-License-Identifier: AGPL-3.0-or-later
#include "export_graphs.h"
#include <desktop/numeric_project_document.h>
#include <QDir>
#include <QDebug>
#include <algorithm>
namespace {
bool present(const NumericExampleGraph& example,domain::NodeId id) {
    return std::any_of(example.graph.nodes.begin(),example.graph.nodes.end(),[=](const auto& node){return node.id==id;});
}
void scalar_labels(const NumericExampleGraph& example,desktop::NumericDraftPresentation& presentation) {
    for(const auto& names:example.scalar_names) {
        if(!present(example,names.view))continue;
        QJsonObject inputs;
        for(std::size_t port=0;port<names.labels.size();++port)
            inputs[QString::number(port)]=QJsonObject{{"enabled",true},{"display_name",QString::fromUtf8(names.labels[port])}};
        const auto key=QString::number(names.view);auto node=presentation.node_presentations.value(key).toObject();
        node["scalar_inputs"]=inputs;presentation.node_presentations[key]=node;
    }
}
desktop::NumericDraftPresentation presentation(const NumericExampleGraph& example) {
    desktop::NumericDraftPresentation result;
    for(const auto& position:example.positions)result.positions[QString::number(position.node)]=QJsonObject{{"x",position.x},{"y",position.y}};
    for(const auto& label:example.node_names)if(present(example,label.node))result.node_presentations[QString::number(label.node)]=QJsonObject{{"display_name",label.label}};
    scalar_labels(example,result);
    result.application_settings={{"ui",QJsonObject{{"language","en"},{"pipeline_auto_apply",false}}}};
    return result;
}
bool reload_export(const QString& path,const NumericExampleGraph& example) {
    desktop::NumericProjectDocument document;domain::NumericProcessingGraph graph;
    const auto result=desktop::load_numeric_project(path,document,graph);
    if(!result){qCritical()<<example.id<<result.path<<result.error;return false;}
    return graph.graph_id==example.graph.graph_id && graph.nodes.size()==example.graph.nodes.size() && graph.edges.size()==example.graph.edges.size();
}
bool export_graph(const QDir& directory,const NumericExampleGraph& example) {
    desktop::NumericProjectDocument document;
    auto result=desktop::create_numeric_project(example.graph,presentation(example),document);
    if(result)result=desktop::save_numeric_project(document,directory.filePath(QString::fromLatin1(example.id)+".json"));
    if(!result){qCritical()<<example.id<<result.path<<result.error;return false;}
    const auto path=directory.filePath(QString::fromLatin1(example.id)+".json");
    if(!reload_export(path,example))return false;
    qInfo().noquote()<<path;return true;
}
}
bool export_reference_graphs(const QString& path,const std::vector<NumericExampleGraph>& examples) {
    QDir directory(path);if(!directory.exists() && !directory.mkpath("."))return false;
    for(const auto& example:examples)if(!export_graph(directory,example))return false;
    return true;
}
