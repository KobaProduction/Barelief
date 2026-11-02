#include <domain/music/contracts.h>
#include <domain/processing_contracts.h>
#include <cmath>
#include <limits>

namespace domain {
using namespace signal_contracts;
namespace {
bool flag(const ResolvedProcessingNode& node,std::string_view key,bool& out) noexcept {
    ProcessingParameterView value;if(resolve_processing_parameter(node,key,value)!=Status::ok)return false;
    const auto* result=std::get_if<bool>(&value);if(!result)return false;
    out=*result;return true;
}
bool threshold(const ResolvedProcessingNode& node,float& out) noexcept {
    ProcessingParameterView value;if(resolve_processing_parameter(node,"magnitude_threshold",value)!=Status::ok)return false;
    const auto* literal=std::get_if<NumericLiteral>(&value);if(!literal)return false;
    const auto number=std::visit([](auto v){return double(v);},*literal);
    if(!std::isfinite(number) || number<0 || number>std::numeric_limits<float>::max())return false;
    out=float(number);return true;
}
bool complex_geometry(const NumericContract& c) noexcept {
    return c.dtype==DType::Float32 && c.channel_order.size()==1 && c.shape.rank==Rank::Matrix &&
        c.shape.axes==std::array{Axis::Bin,Axis::Component} && c.shape.extents[0].minimum &&
        c.shape.extents[0].minimum==c.shape.extents[0].maximum && c.shape.extents[1]==ExtentBounds{2,2} &&
        c.representation.encoding==StorageEncoding::Native && c.representation.layout==ChannelLayout::Planar &&
        !c.representation.valid_bits && c.representation.normalization==Normalization::None &&
        c.quantity==Quantity::SpectrumComplex && c.unit==NumericUnit::RawAmplitude;
}
}
bool spectral_descriptor_kind(std::string_view kind) noexcept {
    return kind=="SpectralFlux" || kind=="ComplexSpectralDifference" || kind=="PhaseDeviation" || kind=="HighFrequencyContent";
}
bool spectral_descriptor_parameters(const ResolvedProcessingNode& node,SpectralDescriptorParameters& out) noexcept {
    SpectralDescriptorParameters p;
    if(node.kind=="SpectralFlux" || node.kind=="ComplexSpectralDifference"){
        if(!flag(node,"half_wave",p.half_wave))return false;
    }else if(node.kind=="PhaseDeviation"){
        if(!flag(node,"weighted",p.weighted) || !threshold(node,p.magnitude_threshold))return false;
    }else if(node.kind=="HighFrequencyContent"){
        if(!flag(node,"squared",p.squared))return false;
    }else return false;
    out=p;return true;
}
Status spectral_descriptor_output_contract(const ConfiguredProcessingNode& node,const NumericContract& input,OwnedProcessingContract& out) noexcept {
    const ResolvedProcessingNode view{node.id,node.kind,NodeRole::Processor,ProcessingOperation::MusicTransform,node.inputs,node.outputs,&node.parameters,node.enabled};
    SpectralDescriptorParameters p;if(!complex_geometry(input) || !spectral_descriptor_parameters(view,p))return Status::incompatible_contract;
    auto c=input;c.semantic=Semantic::Numeric;c.history_element=Semantic::Numeric;c.quantity=Quantity::Unspecified;
    c.shape={};c.coordinates={};c.unit=node.kind=="PhaseDeviation" && !p.weighted?NumericUnit::Radians:NumericUnit::Values;
    return own_processing_contract(c,FrameLimits{1,SIZE_MAX,SIZE_MAX,SIZE_MAX,SIZE_MAX,SIZE_MAX},SIZE_MAX,out);
}
}
