#include <core_app/example_presentation.h>
#include <core_app/plot_composition.h>
#include <core_app/scheme_editor/numeric_editor_model.h>
#include <core_app/scheme_editor/audio_node_model.h>
#include <barelief/ui_text.h>
#include <QJsonArray>
#include <QJsonDocument>
#include <algorithm>
namespace {
QString view_key(const QString& device,const QString& graph,domain::NodeId id) {
    return QString::fromLatin1(QJsonDocument(QJsonArray{device,graph,id}).toJson(QJsonDocument::Compact).toHex());
}
QJsonArray sections_for(const NumericExampleGraph& example,domain::NodeId view) {
    QJsonArray result;
    for(const auto& section:example.sections)if(section.view==view)
        result.append(QJsonObject{{"label",QString::fromUtf8(section.label)},
            {"begin",qint64(section.begin)},{"count",qint64(section.count)}});
    return result;
}
}

QJsonObject example_view_preferences(const NumericExampleGraph& example,const QString& device,const QString& graph) {
    QJsonObject preferences;
    for(const auto id:example.audio_matrix_views) {
        const auto node=std::find_if(example.graph.nodes.begin(),example.graph.nodes.end(),[=](const auto& value){return value.id==id;});
        if(node==example.graph.nodes.end())continue;
        const auto key=view_key(device,graph,id);
        QJsonObject view{{"node",id},{"device",device},{"graph",graph},
            {"kind",QString::fromStdString(node->kind)},{"instance",0},{"open",true},
            {"plot_sections",sections_for(example,id)}};
        const auto descriptor=presentation::plot_descriptor_preferences(
            presentation::PlotDescriptor{{"image2d","surface3d"},{"Graphs","Audio","Signals"},true});
        for(auto field=descriptor.begin();field!=descriptor.end();++field)view[field.key()]=field.value();
        preferences[key]=view;
    }
    return preferences;
}
QJsonObject example_presentation(const NumericExampleGraph& example,const QString& device,const QString& graph) {
    return {{"version",1},{"graph_views",example_view_preferences(example,device,graph)},
        {"graph_rendering",QJsonObject{}},{"workspace",QJsonObject{}},
        {"show_raw_samples",true},{"show_amplitudes",true},{"show_test_amplitudes",true},
        {"show_spectrogram",true},{"show_data_flow",true}};
}
void apply_example_labels(NumericEditorGraphModel* model,const NumericExampleGraph& example) {
    if(!model)return;
    for(const auto& stream:example.streams) {
        auto* node=model->delegateModel<AudioNodeModel>(stream.selector);
        if(!node)continue;
        QJsonArray labels;
        for(const auto* label:stream.labels)labels.append(ui::text(label));
        auto presentation=node->presentation();
        presentation["stream_names"]=labels;
        node->set_presentation(presentation);
    }
}
