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
#include <QScopedValueRollback>

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
ProcessingAdmissionLimits limits() {return {128,256,8,128,4096,16*1024*1024,{16,800000,16*1024*1024,65536,4096,65536}};}
int capture_source_kind(const ConfiguredProcessingNode& node) {
    if(const auto* p=std::get_if<EndpointProcessingParameters>(&node.parameters))for(const auto& value:p->values)
        if(value.key=="source")if(const auto* literal=std::get_if<NumericLiteral>(&value.value))return std::visit([](auto v){return int(v);},*literal);
    return 0;
}
QString dtype_caption(DType type) {
    switch(type) {
    case DType::Int8:return "int8";case DType::UInt8:return "uint8";
    case DType::Int16:return "int16";case DType::UInt16:return "uint16";
    case DType::Int32:return "int32";case DType::UInt32:return "uint32";
    case DType::Int64:return "int64";case DType::UInt64:return "uint64";
    case DType::Float32:return "float32";case DType::Float64:return "float64";
    }return {};
}
QStringList catalog() {
    QStringList result;
    for(const auto& descriptor:node_descriptors())if(descriptor.id!="WLED")result.append(QString::fromUtf8(descriptor.id.data(),int(descriptor.id.size())));
    // Converter is an explicit operation of the canonical configured-node resolver.
    if(!result.contains("Converter"))result.append("Converter");
    return result;
}
QString category(const QString& kind) {
    const auto descriptor=node_kind(kind.toStdString());
    const auto role=descriptor?node_role(*descriptor):NodeRole::Processor;
    return role==NodeRole::Source?"Sources":role==NodeRole::Sink?"Outputs":"Processors";
}
bool is_graph_view(const QString& kind) {
    const auto k=node_kind(kind.toStdString());
    return k && node_role(*k)==NodeRole::Sink && is_view(*k);
}
QString direction_group(const QString& kind) {
    if(kind=="AudioSource")return "Audio";
    if(kind=="TestSignal")return "Generators";
    if(kind=="NetworkInput" || kind=="WLEDOutput" || kind=="WLEDEncode")return "Transport";
    if(is_graph_view(kind))return "Graphs";
    if(kind=="InputSelector" || kind=="Switch" || kind=="Splitter" || kind=="Gate")return "Routing";
    if(kind=="Spectrogram")return "Accumulation";
    return "DSP";
}
namespace {
constexpr std::uint32_t channels[]={0,1};
NumericContract contract(Semantic semantic) {
    NumericContract c;c.channel_order=std::span(channels).first(semantic==Semantic::Scalar || semantic==Semantic::MelEnergy || semantic==Semantic::History?1:2);c.dtype=DType::Float32;
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
namespace {
bool scale_frequency(Ratio& ratio,std::uint32_t before,std::uint32_t after){
    if(!before || !ratio.denominator || ratio.numerator<0)return false;
    if(before==after)return true;
    auto numerator=std::uint64_t(ratio.numerator),denominator=ratio.denominator;
    auto multiplier=std::uint64_t(after),divisor=std::uint64_t(before);
    auto common=std::gcd(numerator,divisor);numerator/=common;divisor/=common;
    common=std::gcd(multiplier,denominator);multiplier/=common;denominator/=common;
    if(numerator>std::uint64_t(INT64_MAX)/multiplier || denominator>UINT64_MAX/divisor)return false;
    ratio={std::int64_t(numerator*multiplier),denominator*divisor};return true;
}
std::uint32_t declared_rate(const ConfiguredProcessingNode& node,const char* key,std::uint32_t fallback){
    const auto* p=std::get_if<TransformProcessingParameters>(&node.parameters);if(!p)return fallback;
    for(const auto& value:p->values)if(value.key==key)if(const auto* literal=std::get_if<NumericLiteral>(&value.value))
        return std::visit([&](auto n){return n>=8000 && n<=192000 && std::floor(double(n))==double(n)?std::uint32_t(n):0;},*literal);
    return fallback;
}
bool clock_node(ConfiguredProcessingNode& node,const ClockContract& clock,const ExtentBounds* pcm_extent=nullptr){
    const std::string domain(clock.domain);auto owned_clock=clock;owned_clock.domain=domain;
    const bool resampler=node.kind=="PcmResampler";
    const auto output_rate=resampler?declared_rate(node,"output_rate",48000):clock.sample_rate;
    if(clock.sample_rate<1000 || clock.sample_rate>192000 || !output_rate)return false;
    if(resampler && (node.inputs.size()!=1 || node.outputs.size()!=1 || !std::holds_alternative<TransformProcessingParameters>(node.parameters) ||
        clock.sample_rate<8000 || output_rate<8000 ||
        std::uint64_t(output_rate)>std::uint64_t(clock.sample_rate)*8 || std::uint64_t(clock.sample_rate)>std::uint64_t(output_rate)*8))return false;
    for(auto* ports:{&node.inputs,&node.outputs})for(auto& p:*ports){
        auto data=p.contract.view();auto* c=std::get_if<NumericContract>(&data);if(!c)continue;
        const bool output=ports==&node.outputs;
        auto next_clock=owned_clock;if(resampler && output){next_clock.sample_rate=output_rate;next_clock.tick_seconds={1,output_rate};}
        for(std::size_t axis=0;axis<c->coordinates.size();++axis){auto& coordinate=c->coordinates[axis];
            if(coordinate.mode==CoordinateMode::Regular && coordinate.unit==NumericUnit::Hertz)
                if(!scale_frequency(coordinate.origin,c->clock.sample_rate,next_clock.sample_rate) || !scale_frequency(coordinate.step,c->clock.sample_rate,next_clock.sample_rate))return false;
            if(coordinate.mode==CoordinateMode::Regular && coordinate.unit==NumericUnit::Seconds && c->shape.axes[axis]==Axis::Sample)
                if(!scale_frequency(coordinate.step,next_clock.sample_rate,c->clock.sample_rate))return false;
        }
        c->clock=next_clock;
        // Window owns its accumulated output size; fixed-window algorithms
        // retain theirs. Transport/routing/conversion sample blocks adopt ingress geometry.
        if(pcm_extent && c->shape.rank==Rank::Vector && c->shape.axes[0]==Axis::Sample &&
           (node.kind!="Window" || ports==&node.inputs) && node.kind!="WindowFunction" && node.kind!="StereoFFT")c->shape.extents[0]=*pcm_extent;
        if(resampler && output){
            if(node.inputs.empty())return false;
            const auto input_data=node.inputs[0].contract.view();const auto* input=std::get_if<NumericContract>(&input_data);if(!input)return false;
            const auto maximum=input->shape.extents[0].maximum;
            if(!maximum || maximum>8192)return false;
            c->shape.extents[0]={1,(maximum*output_rate+clock.sample_rate-1)/clock.sample_rate+1};
            if(c->shape.extents[0].maximum>65536)return false;
        }
        if(own_processing_contract(data,limits().frame,limits().max_owned_metadata_bytes,p.contract)!=Status::ok)return false;
    }
    return true;
}
}
bool set_sample_rate(ConfiguredProcessingNode& node,std::uint32_t rate){
    const auto minimum=node.kind=="AudioSource"?1000u:8000u;
    if(rate<minimum || rate>192000 || (node.kind!="AudioSource" && node.kind!="TestSignal" && node.kind!="PcmResampler"))return false;
    auto candidate=node;
    if(candidate.outputs.empty())return false;
    const auto output=candidate.outputs[0].contract.view();const auto* c=std::get_if<NumericContract>(&output);if(!c)return false;
    if(candidate.kind=="PcmResampler"){
        if(candidate.inputs.size()!=1 || !std::holds_alternative<TransformProcessingParameters>(candidate.parameters))return false;
        auto& values=std::get<TransformProcessingParameters>(candidate.parameters).values;
        auto parameter=std::find_if(values.begin(),values.end(),[](const auto& p){return p.key=="output_rate";});
        if(parameter==values.end())values.push_back({"output_rate",NumericLiteral{std::uint64_t(rate)}});else parameter->value=NumericLiteral{std::uint64_t(rate)};
        const auto input=candidate.inputs[0].contract.view();const auto* input_numeric=std::get_if<NumericContract>(&input);
        if(!input_numeric || !clock_node(candidate,input_numeric->clock))return false;
    }else{
        if(!std::holds_alternative<EndpointProcessingParameters>(candidate.parameters))return false;
        auto clock=c->clock;clock.sample_rate=rate;clock.tick_seconds={1,rate};if(!clock_node(candidate,clock))return false;
        auto& values=std::get<EndpointProcessingParameters>(candidate.parameters).values;
        auto parameter=std::find_if(values.begin(),values.end(),[](const auto& p){return p.key=="sample_rate";});
        if(parameter==values.end())values.push_back({"sample_rate",NumericLiteral{std::uint64_t(rate)}});else parameter->value=NumericLiteral{std::uint64_t(rate)};
    }
    node=std::move(candidate);return true;
}
bool view_accepts(const QString& kind,const NumericContract& c) {
    const auto rank=kind=="ScalarView"?Rank::Scalar:kind=="SpectrogramView"?Rank::Matrix:Rank::Vector;
    return is_graph_view(kind) && c.shape.rank==rank;
}
QString port_caption(const ProcessingPort& p,bool include_extents) {
    if(!p.contract)return ui::text("Unavailable");
    auto c=p.contract.view();
    if(const auto* s=std::get_if<StructuredContract>(&c))return QString::fromUtf8(s->schema.data(),int(s->schema.size()))+" v"+QString::number(s->version);
    const auto& n=std::get<NumericContract>(c);
    QString shape=n.shape.rank==Rank::Scalar?QStringLiteral("● ")+ui::text("Number"):n.shape.rank==Rank::Vector?QStringLiteral("≡ ")+ui::text("Array"):QStringLiteral("▦ ")+ui::text("Matrix");
    if(include_extents && n.shape.rank!=Rank::Scalar){shape+=" · "+QString::number(n.shape.extents[0].maximum);if(n.shape.rank==Rank::Matrix)shape+="×"+QString::number(n.shape.extents[1].maximum);}
    return dtype_caption(n.dtype)+"\n"+shape;
}
QString contract_details(const ProcessingPort& p) {
    if(!p.contract)return ui::text("Unavailable");auto data=p.contract.view();const auto* n=std::get_if<NumericContract>(&data);if(!n)return port_caption(p);
    const char* encodings[]={"Native","Little endian","Big endian","Packed PCM24 LE","Packed PCM24 BE"};
    const char* normalizations[]={"None","Signed unit","Unsigned unit","Float unit"};
    const char* axes[]={"None","Sample","Bin","Band","Time","Declared"};QStringList channel_ids,bounds;
    for(auto id:n->channel_order)channel_ids<<QString::number(id);
    for(unsigned i=0;i<unsigned(n->shape.rank);++i)bounds<<ui::text(axes[int(n->shape.axes[i])])+": "+QString::number(n->shape.extents[i].minimum)+"…"+QString::number(n->shape.extents[i].maximum);
    return port_caption(p)+"\n"+bounds.join(" · ")+"\n"+
        ui::text("Layout")+": "+ui::text(n->representation.layout==ChannelLayout::Planar?"Planar":"Interleaved")+" · "+ui::text(encodings[int(n->representation.encoding)])+"\n"+
        ui::text("Normalization")+": "+ui::text(normalizations[int(n->representation.normalization)])+"\n"+
        ui::text("Channels")+": ["+channel_ids.join(", ")+"]\n"+ui::text("Clock")+": "+QString::fromUtf8(n->clock.domain.data(),int(n->clock.domain.size()))+" · "+QString::number(n->clock.sample_rate)+" Hz; "+QString::number(n->clock.tick_seconds.numerator)+"/"+QString::number(n->clock.tick_seconds.denominator)+" s";
}
QString conversion_effects(DType from,DType to) {
    if(domain::dtype_conversion_is_exact(from,to))return ui::text("Every finite source value is preserved exactly");
    const auto* a=dtype_descriptor(from);const auto* b=dtype_descriptor(to);if(!a || !b)return ui::text("Incompatible connection");QStringList result;
    if(a->ieee && !b->ieee)result<<ui::text("Fractional values require rounding; exact mode rejects them");
    if(!b->signed_value && !b->ieee && (a->signed_value || a->ieee))result<<ui::text("Negative values are outside the destination range");
    if(b->ieee)result<<ui::text("Some values lose precision in the destination mantissa");
    if(!b->ieee){const unsigned bits=b->bytes*8;QString low="0",high;
        if(b->signed_value){low=bits==64?QString::number(std::numeric_limits<qint64>::min()):QString::number(-(qint64{1}<<(bits-1)));high=bits==64?QString::number(std::numeric_limits<qint64>::max()):QString::number((qint64{1}<<(bits-1))-1);}
        else high=bits==64?QString::number(std::numeric_limits<quint64>::max()):QString::number((quint64{1}<<bits)-1);
        result<<ui::text("Destination range: %1 … %2").arg(low,high);
    }
    result<<ui::text("Overflow is rejected or saturated according to the selected policy; no wrap");
    return result.join('\n');
}
QString conversion_policy_caption(std::uint8_t mode) {
    const char* names[]={"Exact; reject inexact or overflow","Round nearest; reject overflow","Round toward zero; reject overflow","Round nearest; saturate overflow"};
    return mode<4?ui::text(names[mode]):ui::text("Invalid policy");
}
QString conversion_policy_details(std::uint8_t mode) {
    const auto p=domain::edge_conversion_policy(mode);
    return ui::text("Rounding")+": "+ui::text(p.rounding==Rounding::TowardZero?"Toward zero":"Nearest ties to even")+"\n"+
        ui::text("Overflow")+": "+ui::text(p.overflow==Overflow::Reject?"Reject":"Saturate")+"\n"+
        ui::text("Precision loss")+": "+ui::text(p.precision_loss==PrecisionLoss::RejectInexact?"Reject inexact":"Allow declared")+"\n"+ui::text("Nonfinite values are rejected");
}
bool set_dtype(ConfiguredProcessingNode& n,PortDirection direction,PortId id,DType type) {
    auto& ports=direction==PortDirection::Input?n.inputs:n.outputs;
    auto it=std::find_if(ports.begin(),ports.end(),[&](const auto& p){return p.id==id;});if(it==ports.end() || !it->contract)return false;
    auto c=it->contract.view();auto* numeric=std::get_if<NumericContract>(&c);if(!numeric)return false;numeric->dtype=type;
    return set_contract(n,direction,id,c);
}
bool set_contract(ConfiguredProcessingNode& n,PortDirection direction,PortId id,const DataContract& c) {
    auto candidate=n;auto& ports=direction==PortDirection::Input?candidate.inputs:candidate.outputs;
    auto it=std::find_if(ports.begin(),ports.end(),[&](const auto& p){return p.id==id;});if(it==ports.end())return false;
    OwnedProcessingContract owner;if(own_processing_contract(c,limits().frame,limits().max_owned_metadata_bytes,owner)!=Status::ok)return false;it->contract=owner;
    // Routing preserves a single exact contract at every endpoint.
    if(candidate.kind=="InputSelector" || std::holds_alternative<RouteProcessingParameters>(candidate.parameters))for(auto* collection:{&candidate.inputs,&candidate.outputs})for(auto& p:*collection)p.contract=owner;
    n=std::move(candidate);return true;
}
bool add_input(ConfiguredProcessingNode& n) {
    if((n.kind!="InputSelector" && n.kind!="ScalarView") || n.inputs.empty() || n.inputs.size()>=limits().max_ports_per_node)return false;
    const auto maximum=std::max_element(n.inputs.begin(),n.inputs.end(),[](const auto& a,const auto& b){return a.id<b.id;})->id;
    if(maximum==std::numeric_limits<PortId>::max())return false;
    n.inputs.push_back({PortId(maximum+1),PortDirection::Input,n.inputs.front().contract});return true;
}
bool remove_input(ConfiguredProcessingNode& n,PortId id) {
    if((n.kind!="InputSelector" && n.kind!="ScalarView") || n.inputs.size()<=1)return false;
    const auto it=std::find_if(n.inputs.begin(),n.inputs.end(),[&](const auto& p){return p.id==id;});
    if(it==n.inputs.end())return false;
    const auto position=std::size_t(it-n.inputs.begin());n.inputs.erase(it);
    if(n.kind=="InputSelector")for(auto& p:std::get<TransformProcessingParameters>(n.parameters).values)if(p.key=="selected_input") {
        const auto selected=std::visit([](auto v){return std::uint64_t(v);},std::get<NumericLiteral>(p.value));
        if(selected==id)p.value=NumericLiteral{std::int64_t(n.inputs[std::min(position,n.inputs.size()-1)].id)};
    }
    return true;
}
bool set_input_count(ConfiguredProcessingNode& n,std::uint16_t count) {
    if(n.kind!="InputSelector" || !count || count>limits().max_ports_per_node || n.inputs.empty())return false;
    auto candidate=n;
    while(candidate.inputs.size()<count)if(!add_input(candidate))return false;
    while(candidate.inputs.size()>count)if(!remove_input(candidate,candidate.inputs.back().id))return false;
    n=std::move(candidate);return true;
}
bool set_output_count(ConfiguredProcessingNode& n,std::uint16_t count) {
    auto* p=std::get_if<RouteProcessingParameters>(&n.parameters);
    if(!p || (n.kind!="Splitter" && n.kind!="Switch") || count<2 || count>8 || n.inputs.empty())return false;
    p->outputs=count;p->selected=std::min<std::uint16_t>(p->selected,count-1);
    n.outputs.resize(count);for(std::uint16_t i=0;i<count;++i)n.outputs[i]={i,PortDirection::Output,n.inputs[0].contract};return true;
}
}

NumericEditorGraphModel::NumericEditorGraphModel(std::shared_ptr<QtNodes::NodeDelegateModelRegistry> registry):DataFlowGraphModel(std::move(registry)) {

}
bool NumericEditorGraphModel::has_numeric_nodes() const {return true;}
QString NumericEditorGraphModel::node_error(const domain::ConfiguredProcessingNode& node) const {
    domain::ResolvedProcessingNode resolved;
    if(!domain::resolve_processing_node(node,numeric_editor::limits(),resolved))return ui::text("Unsupported node settings");
    if(_node_validator && !_node_validator(node))return ui::text("Device does not support these node settings");
    return {};
}
bool NumericEditorGraphModel::connectionPossible(QtNodes::ConnectionId edge) const {
    if(!nodeExists(edge.outNodeId) || !nodeExists(edge.inNodeId) || edge.outPortIndex>=nodeData(edge.outNodeId,QtNodes::NodeRole::OutPortCount).toUInt() || edge.inPortIndex>=nodeData(edge.inNodeId,QtNodes::NodeRole::InPortCount).toUInt())return false;
    if(!DataFlowGraphModel::connectionPossible(edge))return false;
    auto* self=const_cast<NumericEditorGraphModel*>(this);
    auto* from=self->delegateModel<AudioNodeModel>(edge.outNodeId);auto* to=self->delegateModel<AudioNodeModel>(edge.inNodeId);
    if(!from || !to)return false;
    const auto* a=from->numeric_node();const auto* b=to->numeric_node();
    if(!a || !b || !node_error(*a).isEmpty() || !node_error(*b).isEmpty())return false;
    domain::ResolvedProcessingNode x,y;
    if(!domain::resolve_processing_node(*a,numeric_editor::limits(),x) || !domain::resolve_processing_node(*b,numeric_editor::limits(),y))return false;
    const auto find=[](auto ports,auto id)->const domain::ProcessingPort*{for(const auto& p:ports)if(p.id==id)return &p;return nullptr;};
    const auto* output=find(x.outputs,from->port_id(QtNodes::PortType::Out,edge.outPortIndex));const auto* input=find(y.inputs,to->port_id(QtNodes::PortType::In,edge.inPortIndex));
    if(output && input && numeric_editor::is_graph_view(to->name()) && (!std::holds_alternative<signal_contracts::NumericContract>(output->contract.view()) || !numeric_editor::view_accepts(to->name(),std::get<signal_contracts::NumericContract>(output->contract.view()))))return false;
    if(output && input && numeric_editor::is_graph_view(to->name()) && !signal_contracts::same_contract(output->contract.view(),input->contract.view())){
        if(!_allow_connection_clock_adaptation)return false;
        auto graph=numeric_graph();return adapt_connection_clock(edge,graph);
    }
    if(output && input && (domain::processing_ports_compatible(*output,*input) || domain::processing_ports_dtype_convertible(*output,*input)))return true;
    if(!_allow_connection_clock_adaptation)return false;
    auto graph=numeric_graph();return adapt_connection_clock(edge,graph);
}
namespace {
auto edge_key(QtNodes::ConnectionId e){return std::make_tuple(e.outNodeId,e.outPortIndex,e.inNodeId,e.inPortIndex);}
bool same_clock_ports(const domain::ConfiguredProcessingNode& a,const domain::ConfiguredProcessingNode& b){
    for(auto pair:{std::pair{&a.inputs,&b.inputs},std::pair{&a.outputs,&b.outputs}}){
        if(pair.first->size()!=pair.second->size())return false;
        for(std::size_t i=0;i<pair.first->size();++i)if((*pair.first)[i].id!=(*pair.second)[i].id || !signal_contracts::same_contract((*pair.first)[i].contract.view(),(*pair.second)[i].contract.view()))return false;
    }
    return true;
}
}
std::uint8_t NumericEditorGraphModel::connection_policy(QtNodes::ConnectionId edge) const {
    const auto it=_edge_policies.find(edge_key(edge));return it==_edge_policies.end()?0:it->second;
}
QString NumericEditorGraphModel::connection_status(QtNodes::ConnectionId edge) const {
    auto* self=const_cast<NumericEditorGraphModel*>(this);auto* a=self->delegateModel<AudioNodeModel>(edge.outNodeId);auto* b=self->delegateModel<AudioNodeModel>(edge.inNodeId);
    if(!a || !b || !a->numeric_node() || !b->numeric_node())return ui::text("Unavailable");
    const auto& outputs=a->numeric_node()->outputs;const auto& inputs=b->numeric_node()->inputs;
    const auto out=std::find_if(outputs.begin(),outputs.end(),[&](const auto& p){return p.id==a->port_id(QtNodes::PortType::Out,edge.outPortIndex);});
    const auto in=std::find_if(inputs.begin(),inputs.end(),[&](const auto& p){return p.id==b->port_id(QtNodes::PortType::In,edge.inPortIndex);});
    if(out==outputs.end() || in==inputs.end())return ui::text("Unavailable");
    const auto& output=*out;const auto& input=*in;
    if(domain::processing_ports_compatible(output,input))return ui::text("Exact connection");
    if(!domain::processing_ports_dtype_convertible(output,input))return ui::text("Incompatible connection");
    const auto from=std::get<signal_contracts::NumericContract>(output.contract.view()).dtype,to=std::get<signal_contracts::NumericContract>(input.contract.view()).dtype;
    return ui::text(domain::dtype_conversion_is_exact(from,to)?"Exact dtype widening":"Potential loss: range / rounding / precision")+"\n"+ui::text("Nonfinite values are rejected");
}
bool NumericEditorGraphModel::edit_connection_policy(QtNodes::ConnectionId edge,std::uint8_t value) {
    if(value>3 || !connectionExists(edge) || connection_policy(edge)==value)return false;
    const auto before=connection_policy(edge);
    auto apply=[this,edge](std::uint8_t v){_edge_policies[edge_key(edge)]=v;emit nodeUpdated(edge.inNodeId);};
    class Edit final:public QUndoCommand{std::function<void(bool)> action;public:Edit(std::function<void(bool)> a):action(std::move(a)){setText(ui::text("Connection conversion"));}void redo()override{action(true);}void undo()override{action(false);}};
    if(_undo)_undo->push(new Edit([apply,before,value](bool forward){apply(forward?value:before);}));else apply(value);
    return true;
}
QString NumericEditorGraphModel::connection_failure(QtNodes::ConnectionId edge) const {
    auto* self=const_cast<NumericEditorGraphModel*>(this);auto* a=self->delegateModel<AudioNodeModel>(edge.outNodeId);auto* b=self->delegateModel<AudioNodeModel>(edge.inNodeId);
    if(a){const auto error=a->diagnostic_failure(domain::PortDirection::Output,a->port_id(QtNodes::PortType::Out,edge.outPortIndex));if(!error.isEmpty())return error;}
    return b?b->diagnostic_failure(domain::PortDirection::Input,b->port_id(QtNodes::PortType::In,edge.inPortIndex)):QString{};
}
int NumericEditorGraphModel::connection_visual_state(QtNodes::ConnectionId edge) const {
    if(!connection_failure(edge).isEmpty())return 3;
    auto* self=const_cast<NumericEditorGraphModel*>(this);auto* a=self->delegateModel<AudioNodeModel>(edge.outNodeId);auto* b=self->delegateModel<AudioNodeModel>(edge.inNodeId);
    if(!a || !b || !a->numeric_node() || !b->numeric_node())return 3;
    const auto& outputs=a->numeric_node()->outputs;const auto& inputs=b->numeric_node()->inputs;
    if(edge.outPortIndex>=outputs.size() || edge.inPortIndex>=inputs.size())return 3;
    const auto& out=outputs[edge.outPortIndex];const auto& in=inputs[edge.inPortIndex];
    if(domain::processing_ports_compatible(out,in))return 0;
    if(!domain::processing_ports_dtype_convertible(out,in))return 3;
    return domain::dtype_conversion_is_exact(std::get<signal_contracts::NumericContract>(out.contract.view()).dtype,std::get<signal_contracts::NumericContract>(in.contract.view()).dtype)?1:2;
}
QString NumericEditorGraphModel::connection_effects(QtNodes::ConnectionId edge) const {
    auto* self=const_cast<NumericEditorGraphModel*>(this);auto* a=self->delegateModel<AudioNodeModel>(edge.outNodeId);auto* b=self->delegateModel<AudioNodeModel>(edge.inNodeId);
    if(!a || !b || !a->numeric_node() || !b->numeric_node() || edge.outPortIndex>=a->numeric_node()->outputs.size() || edge.inPortIndex>=b->numeric_node()->inputs.size())return {};
    const auto out=a->numeric_node()->outputs[edge.outPortIndex].contract.view(),in=b->numeric_node()->inputs[edge.inPortIndex].contract.view();
    const auto* x=std::get_if<signal_contracts::NumericContract>(&out);const auto* y=std::get_if<signal_contracts::NumericContract>(&in);
    return x && y?numeric_editor::conversion_effects(x->dtype,y->dtype):QString{};
}
void NumericEditorGraphModel::addConnection(QtNodes::ConnectionId edge) {
    if(connectionExists(edge) || !connectionPossible(edge))return;
    auto* from=delegateModel<AudioNodeModel>(edge.outNodeId);auto* to=delegateModel<AudioNodeModel>(edge.inNodeId);
    const auto& output=from->numeric_node()->outputs[edge.outPortIndex];const auto& input=to->numeric_node()->inputs[edge.inPortIndex];
    if(numeric_editor::is_graph_view(to->name()) && !signal_contracts::same_contract(output.contract.view(),input.contract.view())){
        auto graph=numeric_graph();if(adapt_connection_clock(edge,graph))replace_clock_graph(graph);return;
    }
    if(domain::processing_ports_compatible(output,input) || domain::processing_ports_dtype_convertible(output,input)){DataFlowGraphModel::addConnection(edge);return;}
    auto graph=numeric_graph();if(adapt_connection_clock(edge,graph))replace_clock_graph(graph);
}
QJsonObject NumericEditorGraphModel::connectionUndoState(QtNodes::ConnectionId edge) const {
    if(connectionExists(edge))return {};
    const auto original=numeric_graph();auto graph=original;if(!adapt_connection_clock(edge,graph))return {};
    QJsonArray before,after;
    for(std::size_t i=0;i<graph.nodes.size();++i)if(!same_clock_ports(original.nodes[i],graph.nodes[i])){
        QByteArray old_bytes,new_bytes;
        if(!desktop::encode_numeric_node(original.nodes[i],old_bytes) || !desktop::encode_numeric_node(graph.nodes[i],new_bytes))return {};
        before.append(QString::fromUtf8(old_bytes));after.append(QString::fromUtf8(new_bytes));
    }
    return {{"before",before},{"after",after}};
}
QJsonObject NumericEditorGraphModel::saveConnection(const QtNodes::ConnectionId& edge) const {
    auto value=QtNodes::toJson(edge);value["conversion"]=int(connection_policy(edge));return value;
}
void NumericEditorGraphModel::loadConnection(const QJsonObject& value){
    const auto conversion=value.value("conversion");
    if(!conversion.isUndefined() && (!conversion.isDouble() || conversion.toInt(-1)<0 || conversion.toInt(-1)>3 || conversion.toDouble()!=conversion.toInt(-1)))return;
    const auto edge=QtNodes::fromJson(value);if(connectionExists(edge))return;
    _edge_policies[edge_key(edge)]=std::uint8_t(conversion.toInt());addConnection(edge);
    if(!connectionExists(edge))_edge_policies.erase(edge_key(edge));
}
bool NumericEditorGraphModel::undoConnection(QtNodes::ConnectionId edge,const QJsonObject& state){
    if(!connectionExists(edge))return false;
    const auto before=state.value("before").toArray(),after=state.value("after").toArray();
    if(before.size()!=after.size() || before.size()>int(numeric_editor::limits().max_nodes))return DataFlowGraphModel::deleteConnection(edge);
    auto graph=numeric_graph();const auto removed=canonical_edge(edge);
    std::erase_if(graph.edges,[&](const auto& e){return e.from==removed.from && e.to==removed.to && e.input_port==removed.input_port && e.output_port==removed.output_port;});
    for(int i=0;i<after.size();++i){
        domain::ConfiguredProcessingNode old_node,new_node;
        if(!desktop::decode_numeric_node(before[i].toString().toUtf8(),old_node) || !desktop::decode_numeric_node(after[i].toString().toUtf8(),new_node) || old_node.id!=new_node.id)return DataFlowGraphModel::deleteConnection(edge);
        for(auto& node:graph.nodes)if(node.id==new_node.id && same_clock_ports(node,new_node)){
            // Preserve later parameter/presentation edits; only roll back the
            // contracts introduced by this connection, while still unchanged.
            node.inputs=old_node.inputs;node.outputs=old_node.outputs;
        }
    }
    bool compatible=true;
    for(const auto& e:graph.edges){const domain::ProcessingPort *out=nullptr,*in=nullptr;
        for(const auto& n:graph.nodes){if(n.id==e.from)for(const auto& p:n.outputs)if(p.id==e.output_port)out=&p;if(n.id==e.to)for(const auto& p:n.inputs)if(p.id==e.input_port)in=&p;}
        compatible=compatible && out && in && (domain::processing_ports_compatible(*out,*in) || domain::processing_ports_dtype_convertible(*out,*in));
    }
    if(!compatible)return DataFlowGraphModel::deleteConnection(edge);
    replace_clock_graph(graph);return true;
}
void NumericEditorGraphModel::set_capture_device_provider(std::function<QJsonArray(int,QString*)> provider) {
    _capture_device_provider=std::move(provider);
    for(auto id:allNodeIds())if(auto* node=delegateModel<AudioNodeModel>(id))node->set_capture_device_provider(_capture_device_provider);
}
void NumericEditorGraphModel::bind_node(QtNodes::NodeId id) {
    if(auto* node=delegateModel<AudioNodeModel>(id)){
        connect(node,&AudioNodeModel::diagnostics_changed,this,&NumericEditorGraphModel::connection_diagnostics_changed,Qt::UniqueConnection);
        if(const auto* current=node->numeric_node()){auto copy=*current;copy.id=domain::NodeId(id);node->set_numeric_node(copy);}
        node->set_numeric_edit_handler([this,id](const auto& value){return edit_node(id,value);});
        node->set_presentation_edit_handler([this,id](const auto& value){return edit_presentation(id,value);});
        node->set_scalar_topology_handler([this,id](const auto& value,const auto& presentation){
            if(!AudioNodeModel::valid_presentation(presentation))return false;
            if(_undo)_undo->beginMacro(ui::text("Edit metric inputs"));
            const bool accepted=edit_node(id,value);
            if(accepted)edit_presentation(id,presentation);
            if(_undo)_undo->endMacro();return accepted;
        });
        node->set_capture_device_provider(_capture_device_provider);
        node->set_adapter_edit_handler([this,id](const auto& value){return edit_adapter(id,value);});
    }
}
void NumericEditorGraphModel::loadNode(const QJsonObject& json){DataFlowGraphModel::loadNode(json);const auto id=QtNodes::NodeId(json.value("id").toInteger());bind_node(id);}
void NumericEditorGraphModel::load(const QJsonObject& json){
    if(json.value("version").toInt()!=3 || json.value("processing_version").toInt()!=2)return;
    domain::NumericProcessingGraph decoded;
    if(!desktop::decode_numeric_editor_graph(QJsonDocument(json).toJson(QJsonDocument::Compact),decoded))return;
    _execution_interval=decoded.execution_interval;
    _graph_id=json.value("graph_id").toString("main").toStdString();_edge_policies.clear();
    for(const auto value:json.value("connections").toArray()){auto object=value.toObject();_edge_policies[edge_key(QtNodes::fromJson(object))]=std::uint8_t(object.value("conversion").toInt());}
    QScopedValueRollback<bool> strict(_allow_connection_clock_adaptation,false);
    DataFlowGraphModel::load(json);
}
QtNodes::NodeId NumericEditorGraphModel::addNode(QString kind){auto id=DataFlowGraphModel::addNode(kind);if(auto* node=delegateModel<AudioNodeModel>(id)){node->set_numeric_node(numeric_editor::default_node(kind));}bind_node(id);return id;}
QJsonObject NumericEditorGraphModel::save() const {
    auto result=DataFlowGraphModel::save();
    // QtNodes stores connectivity in an unordered set. Reconnecting changed
    // contracts must not change the document solely through iteration order.
    std::vector<QJsonObject> nodes,connections;
    for(const auto value:result.value("nodes").toArray())nodes.push_back(value.toObject());
    std::sort(nodes.begin(),nodes.end(),[](const auto& a,const auto& b){return a.value("id").toInteger()<b.value("id").toInteger();});
    QJsonArray nodeArray;for(const auto& node:nodes)nodeArray.append(node);result["nodes"]=nodeArray;
    for(const auto value:result.value("connections").toArray()){
        auto edge=value.toObject();edge["conversion"]=int(connection_policy(QtNodes::fromJson(edge)));connections.push_back(edge);
    }
    const auto key=[](const QJsonObject& value){const auto e=QtNodes::fromJson(value);return std::tuple{e.outNodeId,e.outPortIndex,e.inNodeId,e.inPortIndex};};
    std::sort(connections.begin(),connections.end(),[&](const auto& a,const auto& b){return key(a)<key(b);});
    QJsonArray edges;for(const auto& edge:connections)edges.append(edge);result["connections"]=edges;
    if(has_numeric_nodes()){result["version"]=3;result["processing_version"]=2;result["graph_id"]=QString::fromStdString(_graph_id);result["execution_interval"]=QJsonObject{
    {"numerator",QJsonObject{{"dtype","int64"},{"decimal",QString::number(_execution_interval.numerator)}}},
    {"denominator",QJsonObject{{"dtype","uint64"},{"decimal",QString::number(_execution_interval.denominator)}}}};}return result;}
domain::Edge NumericEditorGraphModel::canonical_edge(QtNodes::ConnectionId edge) const {
    auto* self=const_cast<NumericEditorGraphModel*>(this);
    auto* a=self->delegateModel<AudioNodeModel>(edge.outNodeId);auto* b=self->delegateModel<AudioNodeModel>(edge.inNodeId);
    return {domain::NodeId(edge.outNodeId),domain::NodeId(edge.inNodeId),a->port_id(QtNodes::PortType::Out,edge.outPortIndex),b->port_id(QtNodes::PortType::In,edge.inPortIndex),connection_policy(edge)};
}
QtNodes::ConnectionId NumericEditorGraphModel::connection_id(const domain::Edge& edge) const {
    auto* self=const_cast<NumericEditorGraphModel*>(this);
    auto* a=self->delegateModel<AudioNodeModel>(QtNodes::NodeId(edge.from));auto* b=self->delegateModel<AudioNodeModel>(QtNodes::NodeId(edge.to));
    return {QtNodes::NodeId(edge.from),a?a->port_index(QtNodes::PortType::Out,edge.output_port):QtNodes::InvalidPortIndex,QtNodes::NodeId(edge.to),b?b->port_index(QtNodes::PortType::In,edge.input_port):QtNodes::InvalidPortIndex};
}
domain::NumericProcessingGraph NumericEditorGraphModel::numeric_graph() const {
    domain::NumericProcessingGraph graph;graph.graph_id=_graph_id;graph.versions=_versions;graph.execution_interval=_execution_interval;auto* self=const_cast<NumericEditorGraphModel*>(this);
    auto ids=allNodeIds();std::vector<QtNodes::NodeId> sorted(ids.begin(),ids.end());std::sort(sorted.begin(),sorted.end());
    for(auto id:sorted)if(auto* node=self->delegateModel<AudioNodeModel>(id);node && node->numeric_node())graph.nodes.push_back(*node->numeric_node());
    for(auto id:sorted)for(auto edge:allConnectionIds(id))if(edge.outNodeId==id)graph.edges.push_back(canonical_edge(edge));
    return graph;
}
bool NumericEditorGraphModel::export_numeric_graph(domain::NumericProcessingGraph& out,QString* error) const {
    auto* self=const_cast<NumericEditorGraphModel*>(this);
    for(auto id:allNodeIds())if(auto* node=self->delegateModel<AudioNodeModel>(id);!node || !node->numeric_node()){
        if(error)*error=ui::text("Unsupported numeric draft version");return false;
    }
    out=numeric_graph();
    if(!domain::valid_execution_interval(out.execution_interval)){if(error)*error=ui::text("Invalid execution interval");return false;}
    for(const auto& node:out.nodes){const auto diagnostic=node_error(node);if(!diagnostic.isEmpty()){if(error)*error=QString::fromStdString(node.kind)+": "+diagnostic;return false;}}return true;
}
bool NumericEditorGraphModel::set_numeric_graph(const domain::NumericProcessingGraph& graph,QString* error) {
    // Preflight without a scene: unsupported nodes or edges must not partially
    // replace the current draft or disappear on the next source-backed save.
    NumericEditorGraphModel candidate(dataModelRegistry());candidate.set_node_validator(_node_validator);candidate._allow_connection_clock_adaptation=false;
    if(domain::processing_versions_supported(graph.versions)!=signal_contracts::Status::ok){if(error)*error=ui::text("Unsupported numeric draft version");return false;}
    if(!domain::valid_execution_interval(graph.execution_interval)){if(error)*error=ui::text("Invalid execution interval");return false;}
    if(graph.nodes.size()>numeric_editor::limits().max_nodes || graph.edges.size()>numeric_editor::limits().max_edges){if(error)*error=ui::text("Draft exceeds editor limits");return false;}
    for(const auto& node:graph.nodes){
        if(node.id<0 || std::uint64_t(node.id)>=std::numeric_limits<QtNodes::NodeId>::max() || candidate.nodeExists(QtNodes::NodeId(node.id)) || !dataModelRegistry()->create(QString::fromStdString(node.kind))){
            if(error)*error=ui::text("Draft contains unsupported nodes");return false;
        }
        QJsonObject json{{"id",node.id},{"internal-data",QJsonObject{{"model-name",QString::fromStdString(node.kind)}}}};
        candidate.loadNode(json);auto* model=candidate.delegateModel<AudioNodeModel>(QtNodes::NodeId(node.id));
        if(!model){if(error)*error=ui::text("Draft contains unsupported nodes");return false;}model->set_numeric_node(node);
    }
    for(const auto& edge:graph.edges){
        if(edge.conversion>3){if(error)*error=ui::text("Invalid connection policy");return false;}
        const auto id=candidate.connection_id(edge);
        if(edge.from<0 || edge.to<0 || !candidate.connectionPossible(id)){
            if(error){
                const auto label=[&](domain::NodeId node){
                    const auto found=std::find_if(graph.nodes.begin(),graph.nodes.end(),[&](const auto& value){return value.id==node;});
                    return found==graph.nodes.end()?ui::text("Unavailable"):AudioNodeModel(QString::fromStdString(found->kind)).caption();
                };
                *error=ui::text("Draft contains incompatible connection: %1 [%2], output %3 → %4 [%5], input %6")
                    .arg(label(edge.from)).arg(edge.from).arg(edge.output_port)
                    .arg(label(edge.to)).arg(edge.to).arg(edge.input_port);
            }return false;
        }
        candidate.addConnection(id);
    }
    for(auto id:allNodeIds())deleteNode(id);_graph_id=graph.graph_id;_versions=graph.versions;_execution_interval=graph.execution_interval;
    for(const auto& node:graph.nodes){loadNode(candidate.saveNode(QtNodes::NodeId(node.id)));}
    _edge_policies.clear();for(const auto& edge:graph.edges){const auto id=connection_id(edge);_edge_policies[edge_key(id)]=edge.conversion;addConnection(id);}
    return true;
}
void NumericEditorGraphModel::replace_node(QtNodes::NodeId id,const domain::ConfiguredProcessingNode& value){
    if(auto* node=delegateModel<AudioNodeModel>(id)){
        // QtNodes shifts higher port indices on deletion. Our IDs are stable:
        // reconnect only the original endpoints after the structural update.
        std::vector<domain::Edge> edges;for(auto edge:allConnectionIds(id)){edges.push_back(canonical_edge(edge));_edge_policies.erase(edge_key(edge));DataFlowGraphModel::deleteConnection(edge);}
        node->set_numeric_node(value);
        for(const auto& edge:edges){const auto qt=connection_id(edge);if(connectionPossible(qt)){_edge_policies[edge_key(qt)]=edge.conversion;addConnection(qt);}}
    }
}
namespace {
bool adopt_extract_input(domain::ConfiguredProcessingNode& node,const signal_contracts::NumericContract& input){
    if(node.kind!="IndexExtract" || node.inputs.size()!=1 || node.outputs.size()!=1 || input.shape.rank==signal_contracts::Rank::Scalar)return false;
    std::uint64_t channel=0;
    const auto* parameters=std::get_if<domain::TransformProcessingParameters>(&node.parameters);if(!parameters)return false;
    for(const auto& parameter:parameters->values)if(parameter.key=="channel_index"){
        const auto* literal=std::get_if<signal_contracts::NumericLiteral>(&parameter.value);if(!literal)return false;
        const bool valid=std::visit([&](auto value){
            using Value=decltype(value);
            if constexpr(std::is_floating_point_v<Value>)if(!std::isfinite(value) || std::trunc(value)!=value)return false;
            if constexpr(std::is_signed_v<Value>)if(value<0)return false;
            if(static_cast<long double>(value)>=static_cast<long double>(input.channel_order.size()))return false;
            channel=static_cast<std::uint64_t>(value);return true;
        },*literal);
        if(!valid)return false;
    }
    if(channel>=input.channel_order.size())return false;
    auto scalar=input;scalar.shape={};scalar.coordinates={};scalar.channel_order=input.channel_order.subspan(std::size_t(channel),1);
    // Own both views before replacing either endpoint: input may borrow node's
    // current metadata, including the selected channel span.
    domain::OwnedProcessingContract input_owner,scalar_owner;
    const auto limits=numeric_editor::limits();
    if(domain::own_processing_contract(input,limits.frame,limits.max_owned_metadata_bytes,input_owner)!=signal_contracts::Status::ok ||
       domain::own_processing_contract(scalar,limits.frame,limits.max_owned_metadata_bytes,scalar_owner)!=signal_contracts::Status::ok)return false;
    node.inputs[0].contract=std::move(input_owner);node.outputs[0].contract=std::move(scalar_owner);return true;
}
}
bool NumericEditorGraphModel::adapt_connection_clock(QtNodes::ConnectionId edge,domain::NumericProcessingGraph& graph) const {
    const auto stable=canonical_edge(edge);domain::ConfiguredProcessingNode *from=nullptr,*to=nullptr;
    for(auto& node:graph.nodes){if(node.id==stable.from)from=&node;if(node.id==stable.to)to=&node;}
    if(!from || !to)return false;
    const domain::ProcessingPort* output=nullptr;for(const auto& p:from->outputs)if(p.id==stable.output_port)output=&p;
    if(!output)return false;const auto data=output->contract.view();const auto* numeric=std::get_if<signal_contracts::NumericContract>(&data);
    if(!numeric)return false;
    if(to->kind=="IndexExtract"){if(!adopt_extract_input(*to,*numeric))return false;graph.edges.push_back(stable);return propagate_clock(graph,to->id);}
    if(numeric_editor::is_graph_view(QString::fromStdString(to->kind))){
        if(!numeric_editor::view_accepts(QString::fromStdString(to->kind),*numeric))return false;
        auto input=std::find_if(to->inputs.begin(),to->inputs.end(),[&](const auto& p){return p.id==stable.input_port;});
        if(input==to->inputs.end())return false;
        input->contract=output->contract;
        graph.edges.push_back(stable);return propagate_clock(graph,to->id);
    }
    const auto extent=numeric->shape.extents[0];
    if(!numeric_editor::clock_node(*to,numeric->clock,numeric->shape.rank==signal_contracts::Rank::Vector && numeric->shape.axes[0]==signal_contracts::Axis::Sample?&extent:nullptr))return false;
    graph.edges.push_back(stable);return propagate_clock(graph,to->id);
}
bool NumericEditorGraphModel::propagate_clock(domain::NumericProcessingGraph& graph,domain::NodeId root) const {
    const auto find_node=[&](domain::NodeId id)->domain::ConfiguredProcessingNode*{for(auto& n:graph.nodes)if(n.id==id)return &n;return nullptr;};
    const auto find_port=[](auto& ports,domain::PortId id)->domain::ProcessingPort*{for(auto& p:ports)if(p.id==id)return &p;return nullptr;};
    std::vector<domain::NodeId> queue{root};std::set<domain::NodeId> visited;
    for(std::size_t cursor=0;cursor<queue.size();++cursor){
        const auto id=queue[cursor];if(!visited.insert(id).second)continue;
        auto* from=find_node(id);if(!from)return false;
        for(const auto& edge:graph.edges)if(edge.from==id){
            auto* to=find_node(edge.to);auto* output=find_port(from->outputs,edge.output_port);if(!to || !output)return false;
            auto* input=find_port(to->inputs,edge.input_port);if(!input)return false;
            const auto source=output->contract.view(),target=input->contract.view();
            const auto* clock=std::get_if<signal_contracts::NumericContract>(&source);const auto* old=std::get_if<signal_contracts::NumericContract>(&target);
            if(!clock || !old)continue;
            const auto extent=clock->shape.extents[0];
            if(to->kind=="IndexExtract"){if(!adopt_extract_input(*to,*clock))return false;}
            else if(numeric_editor::is_graph_view(QString::fromStdString(to->kind))){if(!numeric_editor::view_accepts(QString::fromStdString(to->kind),*clock))return false;input->contract=output->contract;}
            else if(!numeric_editor::clock_node(*to,clock->clock,clock->shape.rank==signal_contracts::Rank::Vector && clock->shape.axes[0]==signal_contracts::Axis::Sample?&extent:nullptr))return false;
            if(!visited.contains(to->id))queue.push_back(to->id);
        }
    }
    for(const auto id:visited){const auto* node=find_node(id);if(!node || !node_error(*node).isEmpty())return false;}
    // A second source feeding a multi-input processor must agree explicitly.
    // Never silently re-clock that independent upstream branch.
    for(const auto& edge:graph.edges){auto* from=find_node(edge.from);auto* to=find_node(edge.to);if(!from || !to)return false;
        const auto* out=find_port(from->outputs,edge.output_port);const auto* in=find_port(to->inputs,edge.input_port);
        if(!out || !in || (!domain::processing_ports_compatible(*out,*in) && !domain::processing_ports_dtype_convertible(*out,*in)))return false;
    }
    return true;
}
void NumericEditorGraphModel::replace_clock_graph(const domain::NumericProcessingGraph& graph){
    {
    QScopedValueRollback<bool> transaction(_atomic_edit_in_progress,true);
    // Preserve delegate objects, preferences, layout and unrelated branches.
    // Reconnect only endpoints whose actual port contract changes, before
    // publishing the preflighted graph as one editor command.
    const auto current_graph=numeric_graph();
    const auto desired_port=[&](domain::NodeId id,domain::PortDirection direction,domain::PortId port)->const domain::ProcessingPort*{
        for(const auto& node:graph.nodes)if(node.id==id)for(const auto& p:direction==domain::PortDirection::Input?node.inputs:node.outputs)if(p.id==port)return &p;return nullptr;
    };
    const auto current_port=[&](domain::NodeId id,domain::PortDirection direction,domain::PortId port)->const domain::ProcessingPort*{
        for(const auto& node:current_graph.nodes)if(node.id==id)for(const auto& p:direction==domain::PortDirection::Input?node.inputs:node.outputs)if(p.id==port)return &p;return nullptr;
    };
    const auto changed_port=[&](domain::NodeId id,domain::PortDirection direction,domain::PortId port){
        const auto* before=current_port(id,direction,port);const auto* after=desired_port(id,direction,port);
        return !before || !after || !signal_contracts::same_contract(before->contract.view(),after->contract.view());
    };
    std::vector<QtNodes::ConnectionId> removed;
    for(const auto& edge:current_graph.edges){
        const bool retained=std::any_of(graph.edges.begin(),graph.edges.end(),[&](const auto& e){return e.from==edge.from && e.to==edge.to && e.input_port==edge.input_port && e.output_port==edge.output_port;});
        if(!retained || changed_port(edge.from,domain::PortDirection::Output,edge.output_port) || changed_port(edge.to,domain::PortDirection::Input,edge.input_port))removed.push_back(connection_id(edge));
    }
    for(const auto& edge:removed){_edge_policies.erase(edge_key(edge));DataFlowGraphModel::deleteConnection(edge);}
    for(const auto& value:graph.nodes)if(auto* node=delegateModel<AudioNodeModel>(QtNodes::NodeId(value.id))){
        const auto* current=node->numeric_node();
        bool same=current && current->enabled==value.enabled && domain::same_processing_parameters(current->parameters,value.parameters);
        for(auto pair:{std::pair{current?&current->inputs:nullptr,&value.inputs},std::pair{current?&current->outputs:nullptr,&value.outputs}}){
            if(!pair.first || pair.first->size()!=pair.second->size()){same=false;continue;}
            for(std::size_t i=0;i<pair.first->size();++i)if((*pair.first)[i].id!=(*pair.second)[i].id || !signal_contracts::same_contract((*pair.first)[i].contract.view(),(*pair.second)[i].contract.view()))same=false;
        }
        if(!same)node->set_numeric_node(value);
    }
    for(const auto& edge:graph.edges){const auto qt=connection_id(edge);_edge_policies[edge_key(qt)]=edge.conversion;if(!connectionExists(qt))DataFlowGraphModel::addConnection(qt);}
    }
    emit connection_diagnostics_changed();
    emit atomic_edit_finished();
}
bool NumericEditorGraphModel::edit_node(QtNodes::NodeId id,const domain::ConfiguredProcessingNode& value){
    auto* node=delegateModel<AudioNodeModel>(id);if(!node || !node->numeric_node() || value.kind!=node->name().toStdString())return false;
    auto before=*node->numeric_node(),after=value;after.id=domain::NodeId(id);
    if(after.kind=="IndexExtract"){
        if(after.inputs.size()!=1)return false;const auto contract=after.inputs[0].contract.view();const auto* input=std::get_if<signal_contracts::NumericContract>(&contract);
        if(!input || !adopt_extract_input(after,*input))return false;
    }
    auto reject=[&](const QString& error){emit edit_rejected(error);return false;};
    if((after.kind=="AudioSource" || after.kind=="TestSignal" || after.kind=="PcmResampler") && !before.outputs.empty() && !after.outputs.empty()){
        const auto old_data=before.outputs[0].contract.view(),new_data=after.outputs[0].contract.view();
        const auto* old_clock=std::get_if<signal_contracts::NumericContract>(&old_data);const auto* new_clock=std::get_if<signal_contracts::NumericContract>(&new_data);
        if(old_clock && new_clock && (old_clock->clock.sample_rate!=new_clock->clock.sample_rate || old_clock->clock.tick_seconds!=new_clock->clock.tick_seconds)){
            if(after.kind=="AudioSource" && numeric_editor::capture_source_kind(before)!=numeric_editor::capture_source_kind(after))return reject(ui::text("Change capture source and sample rate separately"));
            auto original=numeric_graph(),candidate=original;for(auto& n:candidate.nodes)if(n.id==domain::NodeId(id))n=after;
            if(!propagate_clock(candidate,domain::NodeId(id)))return reject(ui::text("Connected nodes do not support this sample rate"));
            if(!_undo){replace_clock_graph(candidate);return true;}
            class RateEdit final:public QUndoCommand{std::function<void(bool)> action;public:explicit RateEdit(std::function<void(bool)> a):action(std::move(a)){setText(ui::text("Edit sample rate"));}void redo()override{action(true);}void undo()override{action(false);}};
            _undo->push(new RateEdit([this,original,candidate](bool forward){replace_clock_graph(forward?candidate:original);}));return true;
        }
    }
    if(const auto error=node_error(after);!error.isEmpty())return reject(QString::fromStdString(after.kind)+": "+error);
    if(after.kind=="AudioSource" && numeric_editor::capture_source_kind(before)!=numeric_editor::capture_source_kind(after)){
        const auto selected=node->adapter().value("device_identity").toString();
        if(!selected.isEmpty()){
            QString error;const auto choices=_capture_device_provider?_capture_device_provider(numeric_editor::capture_source_kind(after),&error):QJsonArray{};
            if(std::none_of(choices.begin(),choices.end(),[&](const auto& choice){return choice.toObject().value("id").toString()==selected;}))
                return reject(ui::text("Choose system default before changing capture source"));
        }
    }
    // Views describe their producer's numeric storage. Editing producer shape,
    // dtype or coordinates updates those sinks in the same undo transaction.
    auto original=numeric_graph(),candidate=original;bool adapted_views=false;
    for(auto& n:candidate.nodes)if(n.id==domain::NodeId(id))n=after;
    for(const auto& edge:candidate.edges)if(edge.from==domain::NodeId(id)){
        const auto output=std::find_if(after.outputs.begin(),after.outputs.end(),[&](const auto& p){return p.id==edge.output_port;});
        if(output==after.outputs.end() || !std::holds_alternative<signal_contracts::NumericContract>(output->contract.view()))continue;
        for(auto& target:candidate.nodes)if(target.id==edge.to && numeric_editor::is_graph_view(QString::fromStdString(target.kind)))
            for(auto& input:target.inputs)if(input.id==edge.input_port && !signal_contracts::same_contract(input.contract.view(),output->contract.view())){input.contract=output->contract;adapted_views=true;}
    }
    if(adapted_views){
        NumericEditorGraphModel preflight(dataModelRegistry());preflight.set_node_validator(_node_validator);
        if(!preflight.set_numeric_graph(candidate))return reject(ui::text("Disconnect affected ports before changing their contract"));
        if(!_undo){replace_clock_graph(candidate);return true;}
        class ViewEdit final:public QUndoCommand{std::function<void(bool)> action;public:explicit ViewEdit(std::function<void(bool)> a):action(std::move(a)){setText(ui::text("Edit parameters"));}void redo()override{action(true);}void undo()override{action(false);}};
        _undo->push(new ViewEdit([this,original,candidate](bool forward){replace_clock_graph(forward?candidate:original);}));return true;
    }
    for(const auto edge:allConnectionIds(id)){
        const auto* other=delegateModel<AudioNodeModel>(edge.outNodeId==id?edge.inNodeId:edge.outNodeId);
        if(!other || !other->numeric_node())return reject(ui::text("Disconnect affected ports before changing their contract"));
        const auto& from=edge.outNodeId==id?after:*other->numeric_node();const auto& to=edge.inNodeId==id?after:*other->numeric_node();
        const auto find=[](const auto& ports,auto index)->const domain::ProcessingPort*{for(const auto& p:ports)if(p.id==index)return &p;return nullptr;};
        const auto stable=canonical_edge(edge);const auto* output=find(from.outputs,stable.output_port);const auto* input=find(to.inputs,stable.input_port);
        if(!input && edge.inNodeId==id && (after.kind=="InputSelector" || after.kind=="ScalarView"))continue;
        if(!output || !input || (!domain::processing_ports_compatible(*output,*input) && !domain::processing_ports_dtype_convertible(*output,*input)))return reject(ui::text("Disconnect affected ports before changing their contract"));
    }
    auto same_ports=[](const auto& a,const auto& b){if(a.size()!=b.size())return false;for(std::size_t i=0;i<a.size();++i)if(a[i].id!=b[i].id || a[i].direction!=b[i].direction || !a[i].contract || !b[i].contract || !signal_contracts::same_contract(a[i].contract.view(),b[i].contract.view()))return false;return true;};
    if(before.enabled==after.enabled && domain::same_processing_parameters(before.parameters,after.parameters) && same_ports(before.inputs,after.inputs) && same_ports(before.outputs,after.outputs))return false;
    if(!_undo){replace_node(id,after);return true;}
    class Edit final:public QUndoCommand {
        std::function<void(bool)> action;
    public:explicit Edit(std::function<void(bool)> a):action(std::move(a)){setText(ui::text("Edit parameters"));}void redo() override{action(true);}void undo() override{action(false);}
    };
    std::vector<domain::Edge> edges;for(auto e:allConnectionIds(id))edges.push_back(canonical_edge(e));
    _undo->push(new Edit([this,id,before,after,edges](bool forward){replace_node(id,forward?after:before);for(const auto& e:edges){const auto qt=connection_id(e);if(connectionPossible(qt)){_edge_policies[edge_key(qt)]=e.conversion;addConnection(qt);}}}));return true;
}
bool NumericEditorGraphModel::edit_presentation(QtNodes::NodeId id,const QJsonObject& value) {
    auto* node=delegateModel<AudioNodeModel>(id);if(!node || !AudioNodeModel::valid_presentation(value) || node->presentation()==value)return false;
    const auto before=node->presentation();
    auto apply=[this,id](const QJsonObject& presentation){if(auto* model=delegateModel<AudioNodeModel>(id))model->set_presentation(presentation);};
    if(!_undo){apply(value);return true;}
    class Edit final:public QUndoCommand {
        std::function<void(bool)> action;
    public:explicit Edit(std::function<void(bool)> a):action(std::move(a)){setText(ui::text("Edit node display"));}void redo() override{action(true);}void undo() override{action(false);}
    };
    _undo->push(new Edit([apply,before,value](bool forward){apply(forward?value:before);}));return true;
}
bool NumericEditorGraphModel::edit_adapter(QtNodes::NodeId id,const QJsonObject& value) {
    auto* node=delegateModel<AudioNodeModel>(id);QJsonObject checked;
    if(!node || !desktop::decode_numeric_node_adapter(QJsonDocument(value).toJson(QJsonDocument::Compact),checked) || node->adapter()==checked)return false;
    if(node->name()=="AudioSource" && node->numeric_node()){
        auto reject=[&](const QString& error){emit edit_rejected(error);return false;};
        if(const auto error=node_error(*node->numeric_node());!error.isEmpty())return reject(error);
        const auto selected=checked.value("device_identity").toString();
        if(!selected.isEmpty() && selected!=node->adapter().value("device_identity").toString()){
            QString error;const auto choices=_capture_device_provider?_capture_device_provider(numeric_editor::capture_source_kind(*node->numeric_node()),&error):QJsonArray{};
            const bool known=std::any_of(choices.begin(),choices.end(),[&](const auto& choice){return choice.toObject().value("id").toString()==selected;});
            if(!known)return reject(error.isEmpty()?ui::text("Selected capture device is unavailable"):error);
        }
    }
    const auto before=node->adapter();auto apply=[this,id](const QJsonObject& adapter){if(auto* model=delegateModel<AudioNodeModel>(id))model->set_adapter(adapter);};
    if(!_undo){apply(checked);return true;}
    class Edit final:public QUndoCommand {
        std::function<void(bool)> action;
    public:explicit Edit(std::function<void(bool)> a):action(std::move(a)){setText(ui::text("Edit network source"));}void redo()override{action(true);}void undo()override{action(false);}
    };
    _undo->push(new Edit([apply,before,checked](bool forward){apply(forward?checked:before);}));return true;
}
