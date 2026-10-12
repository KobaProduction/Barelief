#include "core_app/scheme_editor/numeric_editor_model.h"
#include "core_app/scheme_editor/audio_node_model.h"
#include "visualization/ui_text.h"
#include <desktop/numeric_project_document.h>
#include <QJsonDocument>
#include <QtNodes/internal/ConnectionIdUtils.hpp>
#include <QJsonArray>
#include <algorithm>
#include <limits>
#include <numeric>
#include <cmath>
#include <set>
#include <tuple>
#include <type_traits>
#include <QTreeWidget>
#include "numeric_graph_draft.h"

namespace numeric_editor {
void populate_catalog(QTreeWidget* tree) {
    tree->clear();QMap<QString,QTreeWidgetItem*> roles,groups;
    for(const auto& kind:catalog()) {
        const auto role=category(kind),direction=direction_group(kind),key=role+"/"+direction;
        if(!roles.contains(role)){auto* item=new QTreeWidgetItem(tree);item->setText(0,ui::text(role.toUtf8().constData()));item->setData(0,Qt::UserRole+1,role);roles[role]=item;}
        if(!groups.contains(key)){auto* item=new QTreeWidgetItem(roles[role]);item->setText(0,ui::text(direction.toUtf8().constData()));item->setData(0,Qt::UserRole+1,direction);groups[key]=item;}
        auto* item=new QTreeWidgetItem(groups[key]);item->setText(0,AudioNodeModel(kind).caption());item->setData(0,Qt::UserRole,kind);
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
QString category(const QString& kind) {
    const auto descriptor=node_kind(kind.toStdString());
    const auto role=descriptor?node_role(*descriptor):NodeRole::Processor;
    return role==NodeRole::Source?"Sources":role==NodeRole::Sink?"Outputs":"Processors";
}
bool is_graph_view(const QString& kind) {return draft_view(kind.toStdString());}
QString direction_group(const QString& kind) {
    if(kind=="AudioSource")return "Audio";
    if(kind=="TestSignal" || kind=="SignalGenerator")return "Generators";
    if(kind=="NetworkInput" || kind=="WLEDOutput" || kind=="WLEDEncode")return "Transport";
    if(is_graph_view(kind))return "Graphs";
    if(kind=="InputSelector" || kind=="Switch" || kind=="Splitter" || kind=="Gate")return "Routing";
    if(kind=="Spectrogram")return "Accumulation";
    return "DSP";
}
namespace {
constexpr std::uint32_t channels[]={0,1};
NumericContract contract(Semantic semantic) {
    NumericContract c;c.channel_order=std::span(channels).first(1);c.dtype=DType::Float32;
    c.semantic=semantic;c.clock.domain="local";c.clock.sample_rate=48000;c.clock.tick_seconds={1,48000};
    if(semantic==Semantic::Scalar)return c;
    c.shape.rank=Rank::Vector;c.shape.extents[0]={256,256};c.shape.axes[0]=Axis::Sample;
    if(semantic==Semantic::Window || semantic==Semantic::Windowed)c.shape.extents[0]={4096,4096};
    if(semantic==Semantic::SpectrumMagnitude){c.shape.axes[0]=Axis::Bin;c.shape.extents[0]={2048,2048};c.coordinates[0]={CoordinateMode::Regular,NumericUnit::Hertz,{0,1},{48000,4096}};}
    if(semantic==Semantic::MelEnergy){c.shape.axes[0]=Axis::Band;c.shape.extents[0]={50,50};
        c.coordinates[0]={CoordinateMode::ExplicitFloat64LE,NumericUnit::Hertz,{0,1},{1,1}};}
    if(semantic==Semantic::History){c.history_element=Semantic::MelEnergy;c.shape.rank=Rank::Matrix;c.shape.axes={Axis::Time,Axis::Band};c.shape.extents={ExtentBounds{0,400},ExtentBounds{50,50}};
        c.coordinates[0]={CoordinateMode::ExplicitFloat64LE,NumericUnit::Seconds,{0,1},{1,1}};
        c.coordinates[1]=contract(Semantic::MelEnergy).coordinates[0];}
    return c;
}
ProcessingPort port(PortDirection direction,PortId id,const DataContract& c) {
    ProcessingPort p;p.id=id;p.direction=direction;
    own_processing_contract(c,limits().frame,limits().max_owned_metadata_bytes,p.contract);return p;
}
}
ConfiguredProcessingNode default_node(const QString& name) {
    ConfiguredProcessingNode n;n.id=0;n.kind=name.toStdString();
    const auto k=node_kind(n.kind);
    if(!k && name!="Converter")return n;
    const auto role=k?node_role(*k):NodeRole::Processor;
    Semantic in=Semantic::PCM,out=Semantic::PCM;
    if(name=="MelView" || name=="ScalarReduce" || name=="Spectrogram" || name=="WLED" || name=="WLEDEncode")in=out=Semantic::MelEnergy;
    if(name=="AudioSource" || name=="TestSignal" || name=="AudioView" || name=="Window")in=out=Semantic::PCM;
    if(name=="Window"){out=Semantic::Window;}
    if(name=="WindowFunction"){in=Semantic::Window;out=Semantic::Windowed;}
    if(name=="StereoFFT"){in=Semantic::Windowed;out=Semantic::SpectrumMagnitude;}
    if(name=="SpectrumMerge" || name=="SpectrumGain" || name=="BarkWeighting" || name=="FFTView")in=out=Semantic::SpectrumMagnitude;
    if(name=="Mel"){in=Semantic::SpectrumMagnitude;out=Semantic::MelEnergy;}
    if(name=="WindowView")in=out=Semantic::Window;
    if(name=="WindowedView")in=out=Semantic::Windowed;
    if(name=="SpectrogramView")in=out=Semantic::History;
    if(name=="ScalarView")in=out=Semantic::Scalar;
    if(role==NodeRole::Processor)n.parameters=TransformProcessingParameters{};
    if(role!=NodeRole::Source)n.inputs.push_back(port(PortDirection::Input,0,contract(in)));
    if(role!=NodeRole::Sink)n.outputs.push_back(port(PortDirection::Output,0,contract(out)));
    if(name=="SignalGenerator"){n.parameters=SignalGeneratorProcessingParameters{};auto c=contract(Semantic::PCM);c.shape.extents[0]={1,256};n.outputs[0]=port(PortDirection::Output,0,c);}
    if(name=="SampleClock"){n.inputs.clear();n.outputs.clear();n.parameters=SampleClockProcessingParameters{};}
    if(name=="Mixer"){n.parameters=MixerProcessingParameters{MixerMode::WeightedMean,{{0,1},{1,1}},100};auto c=contract(Semantic::PCM);c.shape.extents[0]={1,256};n.inputs[0]=port(PortDirection::Input,0,c);n.inputs.push_back(port(PortDirection::Input,1,c));n.outputs[0]=port(PortDirection::Output,0,c);}
    if(name=="ChannelComparison"){
        auto pcm=contract(Semantic::PCM);pcm.shape.extents[0]={1,256};n.inputs={port(PortDirection::Input,0,pcm),port(PortDirection::Input,1,pcm)};n.outputs={port(PortDirection::Output,0,contract(Semantic::Scalar))};
        n.parameters=TransformProcessingParameters{{{"window_samples",NumericLiteral{std::uint64_t{512}}},{"silence_threshold",NumericLiteral{1e-12}}}};
    }
    if(name=="IndexExtract"){
        auto output=contract(Semantic::Scalar);output.channel_order=std::span(channels).first(1);n.outputs[0]=port(PortDirection::Output,0,output);
        for(const auto* key:{"channel_index","index","column"})std::get<TransformProcessingParameters>(n.parameters).values.push_back({key,NumericLiteral{std::uint64_t{0}}});
    }
    if(name=="Window")std::get<TransformProcessingParameters>(n.parameters).values.push_back({"size",NumericLiteral{std::uint64_t{4096}}});
    if(name=="PcmResampler"){
        std::get<TransformProcessingParameters>(n.parameters).values.push_back({"output_rate",NumericLiteral{std::uint64_t{48000}}});
        auto output=contract(Semantic::PCM);output.shape.extents[0]={1,257};n.outputs[0]=port(PortDirection::Output,0,output);
    }
    if(name=="Mel")std::get<TransformProcessingParameters>(n.parameters).values.push_back({"bands",NumericLiteral{std::uint64_t{50}}});
    if(name=="SpectrumMerge" || name=="FFTView" || name=="Mel"){auto mono=contract(Semantic::SpectrumMagnitude);mono.channel_order=std::span(channels).first(1);if(name=="SpectrumMerge")n.outputs[0]=port(PortDirection::Output,0,mono);else n.inputs[0]=port(PortDirection::Input,0,mono);}
    if(name=="InputSelector"){n.inputs.push_back(port(PortDirection::Input,1,contract(in)));std::get<TransformProcessingParameters>(n.parameters).values.push_back({"selected_input",NumericLiteral{std::int64_t{0}}});}
    if(name=="Converter")n.parameters=ConverterProcessingParameters{};
    if(name=="Splitter" || name=="Switch" || name=="Gate"){
        RouteProcessingParameters p;p.outputs=name=="Gate"?1:2;n.parameters=p;
        if(p.outputs==2)n.outputs.push_back(port(PortDirection::Output,1,contract(out)));
    }
    if(name=="ScalarReduce"){
        n.parameters=ReduceProcessingParameters{};n.outputs[0]=port(PortDirection::Output,0,contract(Semantic::Scalar));
    }
    if(name=="Spectrogram"){
        n.parameters=HistoryProcessingParameters{400};n.outputs[0]=port(PortDirection::Output,0,contract(Semantic::History));
        auto count=contract(Semantic::Scalar);count.dtype=DType::UInt64;count.quantity=Quantity::FilledFrameCount;count.unit=NumericUnit::Frames;
        n.outputs.push_back(port(PortDirection::Output,1,count));
    }
    if(name=="WLEDEncode")n.outputs[0]=port(PortDirection::Output,0,StructuredContract{"wled-audio-sync",1,"audio-sync-44-le",{44,44}});
    if(name=="WLEDOutput")n.inputs[0]=port(PortDirection::Input,0,StructuredContract{"wled-audio-sync",1,"audio-sync-44-le",{44,44}});
    return n;
}
}
