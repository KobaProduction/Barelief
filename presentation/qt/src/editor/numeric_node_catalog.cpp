#include "core_app/scheme_editor/numeric_editor_model.h"
#include "core_app/scheme_editor/audio_node_model.h"
#include "barelief/ui_text.h"
#include <desktop/numeric_project_document.h>
#include <QJsonDocument>
#include <QtNodes/internal/ConnectionIdUtils.hpp>
#include <QJsonArray>
#include <algorithm>
#include <array>
#include <limits>
#include <numeric>
#include <cmath>
#include <set>
#include <tuple>
#include <type_traits>
#include <QTreeWidget>
#include "numeric_graph_draft.h"
#include <core_app/plot_composition.h>
#include <domain/graph/node_factory.h>

namespace numeric_editor {
namespace {
struct FixedViewMode { const char* kind; const char* mode; const char* group; bool audio; };
constexpr std::array fixed_view_modes{
    FixedViewMode{"ScalarMetricsView","metrics","Scalar values",false},
    FixedViewMode{"ScalarSignedBarView","signedbar","Scalar values",false},
    FixedViewMode{"VectorLineView","line","Vectors",false},
    FixedViewMode{"VectorPointView","points","Vectors",false},
    FixedViewMode{"VectorBarView","bars","Vectors",false},
    FixedViewMode{"VectorPieView","pie","Vectors",false},
    FixedViewMode{"MatrixImageView","image2d","Matrices",false},
    FixedViewMode{"MatrixSurfaceView","surface3d","Matrices",false},
    FixedViewMode{"AudioLineView","line","Signals",true},
    FixedViewMode{"AudioPointView","points","Signals",true},
    FixedViewMode{"AudioBarView","bars","Signals",true},
    FixedViewMode{"AudioPieView","pie","Signals",true},
    FixedViewMode{"AudioImageView","image2d","Matrices",true},
    FixedViewMode{"AudioSurfaceView","surface3d","Matrices",true}
};
struct OperationGroup { const char* kind; const char* family; const char* operation; };
constexpr std::array operation_groups{
    OperationGroup{"Window","Signal processing","Windowing"},
    OperationGroup{"WindowFunction","Signal processing","Windowing"},
    OperationGroup{"MatrixWindowFunction","Signal processing","Windowing"},
    OperationGroup{"WindowGain","Signal processing","Windowing"},
    OperationGroup{"Fourier","Signal analysis","Fourier transforms"},
    OperationGroup{"RealFourier","Signal analysis","Fourier transforms"},
    OperationGroup{"RFFT","Signal analysis","Fourier transforms"},
    OperationGroup{"MatrixFFT","Signal analysis","Fourier transforms"},
    OperationGroup{"AmplitudePhase","Signal analysis","Fourier transforms"},
    OperationGroup{"ComplexPolar","Signal analysis","Fourier transforms"},
    OperationGroup{"SpectrumMerge","Signal analysis","Spectrum and scaling"},
    OperationGroup{"SpectrumGain","Signal analysis","Spectrum and scaling"},
    OperationGroup{"SpectrumAmplitude","Signal analysis","Spectrum and scaling"},
    OperationGroup{"SpectrumPeak","Signal analysis","Spectrum and scaling"},
    OperationGroup{"SpectrumProjection","Signal analysis","Spectrum and scaling"},
    OperationGroup{"FrequencyNormalizer","Signal analysis","Spectrum and scaling"},
    OperationGroup{"BarkWeighting","Signal analysis","Spectrum and scaling"},
    OperationGroup{"Mel","Signal analysis","Spectrum and scaling"},
    OperationGroup{"BroadbandRow","Signal analysis","Spectrum and scaling"},
    OperationGroup{"BroadbandFeatures","Signal analysis","Spectrum and scaling"},
    OperationGroup{"PcmResampler","Signal processing","Resampling and timing"},
    OperationGroup{"SampleRate","Signal processing","Resampling and timing"},
    OperationGroup{"FrequencyStep","Signal processing","Resampling and timing"},
    OperationGroup{"MatrixTranspose","Numeric operations","Arrays and matrices"},
    OperationGroup{"MatrixCenter","Numeric operations","Arrays and matrices"},
    OperationGroup{"MatrixRow","Numeric operations","Arrays and matrices"},
    OperationGroup{"ArrayFill","Numeric operations","Arrays and matrices"},
    OperationGroup{"ArrayMultiply","Numeric operations","Arrays and matrices"},
    OperationGroup{"ArrayConcat","Numeric operations","Arrays and matrices"},
    OperationGroup{"ArrayLength","Numeric operations","Arrays and matrices"},
    OperationGroup{"IndexExtract","Numeric operations","Arrays and matrices"},
    OperationGroup{"Converter","Numeric operations","Conversion"},
    OperationGroup{"NoteSpectrum","Music analysis","Features"},
    OperationGroup{"NoteSalience","Music analysis","Features"},
    OperationGroup{"MusicalSpectrogram","Music analysis","Features"},
    OperationGroup{"SpectralQuantize","Music analysis","Features"},
    OperationGroup{"ScalarReduce","Signal analysis","Measurements"},
    OperationGroup{"ChannelComparison","Signal analysis","Measurements"},
    OperationGroup{"Mixer","Signal processing","Mixing"},
    OperationGroup{"WLEDEncode","Transport","Protocol encoding"}
};
QStringList catalog_path(const QString& kind);
}
presentation::PlotDescriptor plot_descriptor_for_kind(const QString& kind) {
    presentation::PlotDescriptor descriptor;
    const auto fixed=std::find_if(fixed_view_modes.begin(),fixed_view_modes.end(),[&](const auto& item){return kind==item.kind;});
    if(fixed!=fixed_view_modes.end()){
        descriptor.modes={QString::fromLatin1(fixed->mode)};
        descriptor.category_path={"Graphs",fixed->audio?"Audio":"Universal",QString::fromLatin1(fixed->group)};
        descriptor.audio_scales=fixed->audio;
        return descriptor;
    }
    if(kind=="PianoView") {
        descriptor.modes=QStringList{QStringLiteral("piano")};
        descriptor.category_path=QStringList{QStringLiteral("Graphs"),QStringLiteral("Audio"),QStringLiteral("Notes")};
        descriptor.audio_scales=true;
    } else if(kind=="ScalarView") {
        descriptor.modes=QStringList{QStringLiteral("metrics"),QStringLiteral("signedbar")};
        descriptor.category_path=QStringList{QStringLiteral("Graphs"),QStringLiteral("Universal"),QStringLiteral("Scalar values")};
    } else if(kind=="SpectrogramView") {
        descriptor.modes=QStringList{QStringLiteral("image2d"),QStringLiteral("surface3d")};
        descriptor.category_path=QStringList{QStringLiteral("Graphs"),QStringLiteral("Universal"),QStringLiteral("Matrices")};
    } else if(QStringList{"FFTView","AudioView","WindowView","WindowedView",
                          "WindowFunctionView","MelView"}.contains(kind)) {
        descriptor.category_path=QStringList{QStringLiteral("Graphs"),QStringLiteral("Audio"),QStringLiteral("Signals")};
        descriptor.audio_scales=true;
    }
    return descriptor;
}

QTreeWidgetItem* catalog_parent(QTreeWidget* tree,QMap<QString,QTreeWidgetItem*>& groups,const QStringList& path) {
    QTreeWidgetItem* parent=nullptr;
    QString key;
    for(const auto& name:path){
        key+="/"+name;
        if(!groups.contains(key)){
            auto* item=parent?new QTreeWidgetItem(parent):new QTreeWidgetItem(tree);
            item->setText(0,ui::text(name.toUtf8().constData()));item->setData(0,Qt::UserRole+1,name);groups[key]=item;
        }
        parent=groups[key];
    }
    return parent;
}
void populate_catalog(QTreeWidget* tree) {
    tree->clear();QMap<QString,QTreeWidgetItem*> groups;
    const auto available=visible_catalog();
    for(const auto& kind:available) {
        const auto path=is_graph_view(kind)?presentation::plot_category_path(plot_descriptor_for_kind(kind)):catalog_path(kind);
        auto* item=new QTreeWidgetItem(catalog_parent(tree,groups,path));
        item->setText(0,AudioNodeModel(kind).caption());item->setData(0,Qt::UserRole,kind);
    }
    tree->expandAll();
}
void filter_catalog(QTreeWidget* tree,const QString& query) {
    std::function<bool(QTreeWidgetItem*,bool)> show=[&](QTreeWidgetItem* item,bool parent_match){const bool own=parent_match || item->text(0).contains(query,Qt::CaseInsensitive);bool match=own;for(int i=0;i<item->childCount();++i)match=show(item->child(i),own) || match;item->setHidden(!match);return match;};
    for(int i=0;i<tree->topLevelItemCount();++i)show(tree->topLevelItem(i),false);
}
using namespace domain;
using namespace signal_contracts;
int capture_source_kind(const ConfiguredProcessingNode& node) {
    if(const auto* p=std::get_if<EndpointProcessingParameters>(&node.parameters))for(const auto& value:p->values)
        if(value.key=="source")if(const auto* literal=std::get_if<NumericLiteral>(&value.value))return std::visit([](auto v){return int(v);},*literal);
    return 0;
}
QStringList catalog() {
    QStringList result;
    for(const auto& descriptor:node_descriptors())if(descriptor.id!="WLED" && descriptor.id!="Splitter" && descriptor.id!="SpectrumMerge" && descriptor.id!="TestSignal")result.append(QString::fromUtf8(descriptor.id.data(),int(descriptor.id.size())));
    // Converter is an explicit operation of the canonical configured-node resolver.
    if(!result.contains("Converter"))result.append("Converter");
    return result;
}
QStringList visible_catalog() {
    auto result=catalog();
    for(const auto* alias:{"StereoFFT","RFFT","WindowGain","UInt32Constant","ScalarView",
                            "FFTView","MelView","WindowView","WindowedView","WindowFunctionView",
                            "AudioView","SpectrogramView"})result.removeAll(alias);
    return result;
}
QString category(const QString& kind) {
    const auto descriptor=node_kind(kind.toStdString());
    const auto role=descriptor?node_role(*descriptor):NodeRole::Processor;
    return role==NodeRole::Source?"Sources":role==NodeRole::Sink?"Outputs":"Processors";
}
bool is_graph_view(const QString& kind) {return draft_view(kind.toStdString());}
QString direction_group(const QString& kind) {
    if(QStringList{"AudioSource","SpectrumPeak"}.contains(kind))return "Audio";
    if(QStringList{"TestSignal","SignalGenerator"}.contains(kind))return "Generators";
    if(QStringList{"Fs","UInt32Constant","Number","ArrayLength","SampleRate","FrequencyStep"}.contains(kind))return "Numeric values";
    if(QStringList{"NetworkInput","WLEDOutput","WLEDEncode"}.contains(kind))return "Transport";
    if(is_graph_view(kind))return "Graphs";
    if(QStringList{"InputSelector","Switch","Splitter","Gate","Reroute"}.contains(kind))return "Routing";
    if(kind=="Spectrogram" || kind=="NumberAccumulator")return "Accumulation";
    return "Operations";
}
namespace {
QStringList catalog_path(const QString& kind) {
    const auto parent=category(kind);
    const auto branch=direction_group(kind);
    if(parent!="Processors" || branch=="Routing" || branch=="Accumulation")return {parent,branch};
    const auto found=std::find_if(operation_groups.begin(),operation_groups.end(),
        [&](const auto& group){return kind==group.kind;});
    if(found==operation_groups.end())return {parent,"Other operations"};
    return {parent,QString::fromLatin1(found->family),QString::fromLatin1(found->operation)};
}
}
ConfiguredProcessingNode default_node(const QString& name) {
    ConfiguredProcessingNode result;
    if(!domain::make_processing_node(name.toStdString(),0,limits(),result))return {};
    return result;
}
}
