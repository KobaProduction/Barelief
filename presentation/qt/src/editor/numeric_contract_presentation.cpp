#include "core_app/scheme_editor/numeric_node_operations.h"
#include "barelief/ui_text.h"
#include <limits>
#include "numeric_graph_draft.h"
#include "parameter_field_specs.h"
namespace numeric_editor {
using namespace domain;
using namespace signal_contracts;
bool view_accepts(const QString& kind,const NumericContract& c) {return draft_view_accepts(kind.toStdString(),c);}
namespace {
QString processing_reason_caption(domain::ProcessingReason reason) {
    static constexpr const char* captions[]{
        "", "FFT size does not match", "Sample rate does not match",
        "Spectrum layout does not match", "Spectrum representation does not match",
        "Window function does not match", "Window coefficient sum does not match",
        "Window coefficient square sum does not match", "Audio frontend does not match",
        "Resampler settings do not match", "Gain count does not match",
        "Gain values must be finite and valid", "Profile encoding is invalid"};
    const auto index=std::size_t(reason);
    return index<std::size(captions)?ui::text(captions[index]):ui::text("Unsupported node settings");
}
}

QString processing_reason_text(domain::ProcessingReason reason,std::string_view parameter) {
    auto result=processing_reason_caption(reason);
    if(!parameter.empty()) {
        const auto key=QString::fromUtf8(parameter.data(),qsizetype(parameter.size()));
        result+="\n"+ui::text("Parameter: %1").arg(ui::text(parameter_title(key).toUtf8().constData()));
    }
    return result;
}

QString processing_diagnostic_text(const domain::ProcessingDiagnostic& diagnostic) {
    static constexpr const char* messages[]{"", "Unsupported version", "Graph exceeds limits", "Invalid node", "Unknown node kind",
        "Invalid parameters", "Invalid port", "Invalid contract", "Incompatible connection", "Multiple connections to one input",
        "Missing endpoint", "Graph contains a cycle", "Unsupported operation combination", "Resource limit exceeded", "Insufficient workspace",
        "Preparation value unavailable", "Preparation value type mismatch", "Audio frontend settings do not match",
        "Preparation value is invalid", "Preparation value is outside the allowed range"};
    const auto index=std::size_t(diagnostic.error);
    if(!index)return {};
    const auto message=diagnostic.reason==domain::ProcessingReason::None
        ?ui::text(index<std::size(messages)?messages[index]:"Unsupported node settings")
        :processing_reason_caption(diagnostic.reason);
    auto result=ui::text("Node %1, port %2: %3").arg(diagnostic.node).arg(diagnostic.port).arg(message);
    if(diagnostic.edge)result+="\n"+ui::text("Connection #%1").arg(qulonglong(*diagnostic.edge));
    if(!diagnostic.parameter.empty()) {
        const auto key=QString::fromUtf8(diagnostic.parameter.data(),qsizetype(diagnostic.parameter.size()));
        result+="\n"+ui::text("Parameter: %1").arg(ui::text(parameter_title(key).toUtf8().constData()));
    }
    return result;
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
QString port_caption(const ProcessingPort& p,bool include_extents) {
    if(!p.contract)return ui::text("Unavailable");
    auto c=p.contract.view();
    if(const auto* s=std::get_if<StructuredContract>(&c))return QString::fromUtf8(s->schema.data(),int(s->schema.size()))+" v"+QString::number(s->version);
    const auto& n=std::get<NumericContract>(c);
    if(n.shape.rank==Rank::Scalar)return dtype_caption(n.dtype);
    if(n.shape.rank==Rank::Vector)return include_extents?QStringLiteral("%1[%2]").arg(dtype_caption(n.dtype)).arg(n.shape.extents[0].maximum):dtype_caption(n.dtype)+QStringLiteral("[]");
    if(!include_extents)return dtype_caption(n.dtype)+QStringLiteral("[,]");
    return dtype_caption(n.dtype)+QStringLiteral("[%1×%2]").arg(n.shape.extents[0].maximum).arg(n.shape.extents[1].maximum);
}
QStringList contract_bounds(const NumericContract& contract) {
    const char* axes[]={"None","Sample","Bin","Band","Time","Declared"};
    QStringList bounds;
    for(unsigned i=0;i<unsigned(contract.shape.rank);++i)
        bounds<<ui::text(axes[int(contract.shape.axes[i])])+": "+QString::number(contract.shape.extents[i].minimum)+"…"+QString::number(contract.shape.extents[i].maximum);
    return bounds;
}
QStringList contract_channels(const NumericContract& contract) {
    QStringList channel_ids;
    for(auto id:contract.channel_order)channel_ids<<QString::number(id);
    return channel_ids;
}
QString contract_details(const ProcessingPort& p) {
    if(!p.contract)return ui::text("Unavailable");auto data=p.contract.view();const auto* n=std::get_if<NumericContract>(&data);if(!n)return port_caption(p);
    const char* encodings[]={"Native","Little endian","Big endian","Packed PCM24 LE","Packed PCM24 BE"};
    const char* normalizations[]={"None","Signed unit","Unsigned unit","Float unit"};
    return port_caption(p)+"\n"+contract_bounds(*n).join(" · ")+"\n"+
        ui::text("Layout")+": "+ui::text(n->representation.layout==ChannelLayout::Planar?"Planar":"Interleaved")+" · "+ui::text(encodings[int(n->representation.encoding)])+"\n"+
        ui::text("Normalization")+": "+ui::text(normalizations[int(n->representation.normalization)])+"\n"+
        ui::text("Channels")+": ["+contract_channels(*n).join(", ")+"]\n"+ui::text("Clock")+": "+QString::fromUtf8(n->clock.domain.data(),int(n->clock.domain.size()))+" · "+QString::number(n->clock.sample_rate)+" Hz; "+QString::number(n->clock.tick_seconds.numerator)+"/"+QString::number(n->clock.tick_seconds.denominator)+" s";
}
QString destination_integer_range(const DTypeDescriptor& target) {
    const unsigned bits=target.bytes*8;
    QString low="0",high;
    if(target.signed_value){
        low=bits==64?QString::number(std::numeric_limits<qint64>::min()):QString::number(-(qint64{1}<<(bits-1)));
        high=bits==64?QString::number(std::numeric_limits<qint64>::max()):QString::number((qint64{1}<<(bits-1))-1);
    } else high=bits==64?QString::number(std::numeric_limits<quint64>::max()):QString::number((quint64{1}<<bits)-1);
    return ui::text("Destination range: %1 … %2").arg(low,high);
}
void append_destination_effects(const DTypeDescriptor& source,const DTypeDescriptor& target,QStringList& result) {
    if(source.ieee && !target.ieee)result<<ui::text("Fractional values require rounding; exact mode rejects them");
    if(!target.signed_value && !target.ieee && (source.signed_value || source.ieee))result<<ui::text("Negative values are outside the destination range");
    if(target.ieee)result<<ui::text("Some values lose precision in the destination mantissa");
    if(!target.ieee)result<<destination_integer_range(target);
}
QString conversion_effects(DType from,DType to) {
    if(domain::dtype_conversion_is_exact(from,to))return ui::text("Every finite source value is preserved exactly");
    const auto* a=dtype_descriptor(from);const auto* b=dtype_descriptor(to);if(!a || !b)return ui::text("Incompatible connection");QStringList result;
    append_destination_effects(*a,*b,result);
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
}
