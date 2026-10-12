#include "core_app/scheme_editor/numeric_node_operations.h"
#include "visualization/ui_text.h"
#include <limits>
#include "numeric_graph_draft.h"
namespace numeric_editor {
using namespace domain;
using namespace signal_contracts;
bool view_accepts(const QString& kind,const NumericContract& c) {return draft_view_accepts(kind.toStdString(),c);}
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
}
