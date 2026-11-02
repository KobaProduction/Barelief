#include <domain/signal/contracts.h>
#include <algorithm>
#include <cmath>

namespace domain {
using namespace signal_contracts;
namespace {
bool number(const ResolvedProcessingNode& n,std::string_view key,double& out) noexcept {
    ProcessingParameterView field;if(resolve_processing_parameter(n,key,field)!=Status::ok)return false;
    const auto* value=std::get_if<NumericLiteral>(&field);if(!value)return false;
    out=std::visit([](auto v){return double(v);},*value);return std::isfinite(out);
}
std::string_view text(const ResolvedProcessingNode& n,std::string_view key) noexcept {
    ProcessingParameterView field;if(resolve_processing_parameter(n,key,field)!=Status::ok)return {};
    const auto* value=std::get_if<std::string_view>(&field);return value?*value:std::string_view{};
}
bool choices(std::string_view value,std::initializer_list<std::string_view> options) noexcept {
    return std::find(options.begin(),options.end(),value)!=options.end();
}
bool frequency_step(const ResolvedProcessingNode& node,MelFilterParameters& p) noexcept {
    double size=0;
    if(!number(node,"fft_size",size) || size<0 || std::floor(size)!=size)return false;
    if(size)p.bin_hz=p.sample_rate/size;
    else if(!number(node,"bin_hz",p.bin_hz))return false;
    return std::isfinite(p.bin_hz) && p.bin_hz>0;
}
bool grid_parameters(const ResolvedProcessingNode& n,MelFilterParameters& p) noexcept {
    double first=0,bands=0;ProcessingParameterView field;
    if(!number(n,"sample_rate",p.sample_rate) || !frequency_step(n,p) || !number(n,"first_bin",first) ||
       !number(n,"bands",bands) || p.sample_rate<=0 || p.bin_hz<=0 || first<0 || first>UINT32_MAX || std::floor(first)!=first ||
       bands<1 || bands>UINT32_MAX || std::floor(bands)!=bands || resolve_processing_parameter(n,"frequencies_hz",field)!=Status::ok)return false;
    const auto* values=std::get_if<std::span<const float>>(&field);if(!values)return false;
    p.first_bin=std::uint64_t(first);p.bands=std::size_t(bands);p.frequencies_hz=*values;return true;
}
bool frequencies_valid(std::uint64_t count,const MelFilterParameters& p) noexcept {
    double previous=-1;
    for(std::size_t i=0;i<count;++i){
        const auto hz=p.frequencies_hz.empty()?(double(i)+double(p.first_bin))*p.bin_hz:double(p.frequencies_hz[i]);
        if(!std::isfinite(hz) || hz<0 || hz<=previous || hz>p.sample_rate/2)return false;
        previous=hz;
    }
    return true;
}
bool grid_matches(const NumericContract& c,std::size_t axis,const MelFilterParameters& p) noexcept {
    const auto count=c.shape.extents[axis].maximum;const auto& coordinates=c.coordinates[axis];
    if(coordinates.mode!=CoordinateMode::None && coordinates.unit!=NumericUnit::Hertz)return false;
    if(!count || c.shape.extents[axis].minimum!=count ||
       (!p.frequencies_hz.empty() && (p.frequencies_hz.size()!=count || coordinates.mode!=CoordinateMode::ExplicitFloat64LE)))return false;
    if(p.frequencies_hz.empty() && coordinates.mode!=CoordinateMode::ExplicitFloat64LE &&
       !declared_frequency_axis_matches(coordinates,p.bin_hz,p.first_bin))return false;
    return frequencies_valid(count,p);
}
bool input_valid(const NumericContract& c,const MelFilterParameters& p) noexcept {
    const auto axis=c.shape.rank==Rank::Matrix?1u:0u;
    return c.dtype==DType::Float32 && c.channel_order.size()==1 && c.clock.sample_rate==p.sample_rate &&
        c.representation.encoding==StorageEncoding::Native && c.representation.layout==ChannelLayout::Planar && !c.representation.valid_bits &&
        (c.representation.normalization==Normalization::None || c.representation.normalization==Normalization::FloatUnit) &&
        (c.shape.rank==Rank::Vector || (c.shape.rank==Rank::Matrix && c.shape.axes[0]==Axis::Time)) && c.shape.axes[axis]==Axis::Bin &&
        (c.unit==NumericUnit::RawAmplitude || c.unit==NumericUnit::SquaredAmplitude || c.unit==NumericUnit::Values) && grid_matches(c,axis,p);
}
NumericContract output(const NumericContract& input,const MelFilterParameters& p) noexcept {
    auto c=input;c.semantic=c.shape.rank==Rank::Matrix?Semantic::History:Semantic::MelEnergy;
    c.history_element=c.shape.rank==Rank::Matrix?Semantic::MelEnergy:Semantic::Numeric;c.quantity=Quantity::MelEnergy;
    c.representation.normalization=Normalization::None;const auto axis=c.shape.rank==Rank::Matrix?1u:0u;
    c.shape.axes[axis]=Axis::Band;c.shape.extents[axis]={p.bands,p.bands};
    c.coordinates[axis]={CoordinateMode::ExplicitFloat64LE,NumericUnit::Hertz,{0,1},{1,1}};return c;
}
}
bool mel_parameters(const ResolvedProcessingNode& node,MelFilterParameters& out) noexcept {
    MelFilterParameters p;if(!grid_parameters(node,p) || !number(node,"fmin_hz",p.fmin_hz) || !number(node,"fmax_hz",p.fmax_hz) ||
       !number(node,"norm_p",p.norm_p))return false;
    p.scale=text(node,"scale");p.norm=text(node,"norm");const auto high=p.fmax_hz?p.fmax_hz:p.sample_rate/2;
    if(p.fmin_hz<0 || high<=p.fmin_hz || !choices(p.scale,{"htk","slaney"}) ||
       !choices(p.norm,{"slaney","none","l0","lp","maximum","minimum"}) || (p.norm=="lp" && p.norm_p<=0))return false;
    out=p;return true;
}
Status mel_output_contract(const ConfiguredProcessingNode& node,const NumericContract& input,OwnedProcessingContract& out) noexcept {
    const ResolvedProcessingNode n{node.id,node.kind,NodeRole::Processor,ProcessingOperation::Transform,node.inputs,node.outputs,&node.parameters,node.enabled};
    MelFilterParameters p;if(!mel_parameters(n,p) || !input_valid(input,p))return Status::incompatible_contract;
    return own_processing_contract(output(input,p),FrameLimits{1,SIZE_MAX,SIZE_MAX,SIZE_MAX,SIZE_MAX,SIZE_MAX},SIZE_MAX,out);
}
}
