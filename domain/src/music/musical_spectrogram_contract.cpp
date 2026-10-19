#include <domain/music/contracts.h>
#include <domain/signal/contracts.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <string>
#include <utility>
namespace domain {
using namespace signal_contracts;
namespace {
struct MusicalParameters {
    const ConfiguredProcessingNode& node;
    bool number(std::string_view key,double& out) const {
        const ResolvedProcessingNode view{node.id,node.kind,NodeRole::Processor,ProcessingOperation::MusicTransform,node.inputs,node.outputs,&node.parameters,node.enabled};
        ProcessingParameterView value;
        if(resolve_processing_parameter(view,key,value)!=Status::ok)return false;
        const auto* literal=std::get_if<NumericLiteral>(&value);if(!literal)return false;
        out=std::visit([](auto v){return double(v);},*literal);return std::isfinite(out);
    }
    std::string_view text(std::string_view key) const {
        const ResolvedProcessingNode view{node.id,node.kind,NodeRole::Processor,ProcessingOperation::MusicTransform,node.inputs,node.outputs,&node.parameters,node.enabled};
        ProcessingParameterView value;
        if(resolve_processing_parameter(view,key,value)!=Status::ok)return {};
        const auto* text=std::get_if<std::string_view>(&value);return text?*text:std::string_view{};
    }
    bool weights(std::size_t bins) const {
        const ResolvedProcessingNode view{node.id,node.kind,NodeRole::Processor,ProcessingOperation::MusicTransform,node.inputs,node.outputs,&node.parameters,node.enabled};
        ProcessingParameterView value;
        if(resolve_processing_parameter(view,"weights",value)!=Status::ok)return false;
        const auto* weights=std::get_if<std::span<const float>>(&value);
        return weights && (weights->empty() || weights->size()==bins) && std::all_of(weights->begin(),weights->end(),[](float v){return std::isfinite(v) && v>=0;});
    }
};
bool valid_shape(const NumericContract& input) {
    return input.dtype==DType::Float32 && input.channel_order.size()==1 && input.shape.rank==Rank::Vector &&
        input.shape.axes[0]==Axis::Bin && input.shape.extents[0].minimum==input.shape.extents[0].maximum &&
        input.shape.extents[0].maximum>=2 && input.shape.extents[0].maximum<=UINT32_MAX &&
        input.representation.encoding==StorageEncoding::Native && input.representation.layout==ChannelLayout::Planar && input.representation.normalization==Normalization::None;
}
bool valid_frequency_axis(const AxisCoordinates& axis) {
    return axis.mode==CoordinateMode::Regular && axis.unit==NumericUnit::Hertz &&
        axis.origin.denominator && axis.origin.numerator>=0 && axis.step.denominator && axis.step.numerator>0;
}
bool valid_note_range(const MusicalParameters& params,std::string_view first_key,std::string_view last_key,double& first,double& last) {
    return params.number(first_key,first) && params.number(last_key,last) &&
        first>=0 && last<=127 && first<=last && std::floor(first)==first && std::floor(last)==last;
}
bool valid_note_settings(const MusicalParameters& params,double& first,double& last) {
    double assessment_first=0,assessment_last=0,a4=0,tolerance=0,threshold=0;
    if(!valid_note_range(params,"first_midi","last_midi",first,last))return false;
    if(!valid_note_range(params,"assessment_first_midi","assessment_last_midi",assessment_first,assessment_last))return false;
    return params.number("a4_hz",a4) && a4>0 && params.number("cents_tolerance",tolerance) && tolerance>=0 && tolerance<=50 && params.number("minimum_peak_level",threshold) && threshold>=0;
}
struct Section {
    double low=0,high=0,count=0;
    std::string_view reduction;
};
bool read_section(const MusicalParameters& params,std::string_view prefix,Section& out) {
    const std::string name(prefix);
    if(!params.number(name+"_low_hz",out.low) || !params.number(name+"_high_hz",out.high) || !params.number(name+"_count",out.count))return false;
    out.reduction=params.text(name+"_reduction");
    return out.low>=0 && out.low<out.high && out.count>=0 && out.count<=2048 && std::floor(out.count)==out.count;
}
Status own(const NumericContract& value,OwnedProcessingContract& out) {
    return own_processing_contract(value,FrameLimits{1,SIZE_MAX,SIZE_MAX,SIZE_MAX,SIZE_MAX,SIZE_MAX},SIZE_MAX,out);
}
struct MusicalLayout {
    const ConfiguredProcessingNode& node;
    MusicalParameters params{node};
    NumericContract input;
    OwnedProcessingContract annotated;
    bool broadband=node.kind!="NoteSpectrum",notes=node.kind!="BroadbandRow";
    NumericUnit output_unit=NumericUnit::Values;
    double note_first=0,note_last=0;
    std::uint64_t total=0;
    std::array<std::size_t,4> sections{};
    Status annotate() {
        const auto status=explicit_frequency_grid_contract(node,input,annotated);
        if(status!=Status::ok)return status;
        input=std::get<NumericContract>(annotated.view());input.shape.axes[0]=Axis::Bin;return Status::ok;
    }
    Status classify_quantity() {
        const auto weight=params.text("peak_weight");
        if(weight!="value" && weight!="squared_value")return Status::invalid_argument;
        const auto representation=params.text("input_representation");
        if(representation=="linear") {
            if(node.kind=="MusicalSpectrogram")return Status::invalid_argument;
            if(input.quantity!=Quantity::Unspecified || input.unit!=NumericUnit::Values)return Status::incompatible_contract;
            output_unit=NumericUnit::Values;
        } else if(representation=="magnitude") {
            if(input.quantity!=Quantity::SpectrumMagnitude || input.unit!=NumericUnit::RawAmplitude)return Status::incompatible_contract;
            output_unit=NumericUnit::RawAmplitude;
        } else if(representation=="peak_amplitude") {
            if(input.quantity!=Quantity::SpectrumPeakAmplitude || input.unit!=NumericUnit::RawAmplitude)return Status::incompatible_contract;
            output_unit=NumericUnit::RawAmplitude;
        } else if(representation=="power") {
            if(input.quantity!=Quantity::SpectrumPower || input.unit!=NumericUnit::SquaredAmplitude)return Status::incompatible_contract;
            output_unit=NumericUnit::SquaredAmplitude;
        } else if(representation=="psd") {
            if(input.quantity!=Quantity::SpectrumPsd || input.unit!=NumericUnit::SquaredAmplitudePerHertz)return Status::incompatible_contract;
            output_unit=NumericUnit::SquaredAmplitudePerHertz;
        } else return Status::invalid_argument;
        return Status::ok;
    }
    std::uint64_t selected_bins(const Section& section,bool last) const {
        const auto& axis=input.coordinates[0];
        const auto origin=double(axis.origin.numerator)/axis.origin.denominator,step=double(axis.step.numerator)/axis.step.denominator;
        const auto bins=input.shape.extents[0].maximum;const auto final=origin+double(bins-1)*step;
        std::uint64_t count=0;
        for(std::uint64_t i=0;i<bins;++i) {
            const auto hz=origin+double(i)*step;
            if(hz>=section.low && (hz<section.high || (last && section.high==final && hz==section.high)))++count;
        }
        return count;
    }
    Status append_section(std::string_view prefix,std::size_t index,double& previous_high) {
        Section section;
        if(!read_section(params,prefix,section) || section.low<previous_high)return Status::invalid_argument;
        previous_high=section.high;
        if(section.reduction=="pass_through") {
            if(section.count!=0)return Status::invalid_argument;
            sections[index]=std::size_t(selected_bins(section,index==2));
        } else {
            if(section.count<1 || (section.reduction!="mean" && section.reduction!="rms" && section.reduction!="weighted_mean" && section.reduction!="maximum"))return Status::invalid_argument;
            sections[index]=std::size_t(section.count);
        }
        total+=sections[index];return total<=UINT32_MAX?Status::ok:Status::exhausted;
    }
    Status prepare_sections() {
        if(notes) {
            if(!valid_note_settings(params,note_first,note_last))return Status::invalid_argument;
            total=std::uint64_t(note_last-note_first+1);sections[3]=std::size_t(total);
        }
        if(!broadband)return Status::ok;
        if(!params.weights(std::size_t(input.shape.extents[0].maximum)))return Status::invalid_argument;
        constexpr std::array<std::string_view,3> prefixes{"low","mid","high"};double previous_high=-1;
        for(std::size_t i=0;i<prefixes.size();++i){const auto status=append_section(prefixes[i],i,previous_high);if(status!=Status::ok)return status;}
        return Status::ok;
    }
    NumericContract row() const {
        auto value=input;value.semantic=Semantic::Numeric;value.history_element=Semantic::Numeric;
        value.quantity=Quantity::Unspecified;value.unit=output_unit;value.shape.axes={Axis::Band,Axis::None};
        value.shape.extents[0]={total,total};value.coordinates={};
        if(node.kind!="MusicalSpectrogram")value.coordinates[0]={CoordinateMode::ExplicitFloat64LE,NumericUnit::Hertz,{0,1},{1,1}};
        return value;
    }
    Status note_output(NumericContract value,NumericContract scalar,OwnedProcessingContract& out) const {
        if(node.kind=="NoteSpectrum") {
            scalar.dtype=DType::UInt32;scalar.quantity=Quantity::Unspecified;scalar.representation={};scalar.representation.alignment=alignof(std::uint32_t);
            return own(scalar,out);
        }
        if(!notes)return Status::ok;
        const auto count=std::uint64_t(note_last-note_first+1);value.shape.extents[0]={count,count};
        value.coordinates[0]={CoordinateMode::ExplicitFloat64LE,NumericUnit::Hertz,{0,1},{1,1}};
        return own(value,out);
    }
    Status materialize(std::array<OwnedProcessingContract,4>& out) const {
        const auto value=row();auto scalar=value;
        scalar.semantic=Semantic::Scalar;scalar.shape={};scalar.coordinates={};scalar.unit=NumericUnit::Values;
        if(own(value,out[0])!=Status::ok)return Status::exhausted;
        for(unsigned i=1;i<3;++i)if(own(scalar,out[i])!=Status::ok)return Status::exhausted;
        return note_output(value,scalar,out[3]);
    }
};
}
Status musical_spectrogram_output_contracts(const ConfiguredProcessingNode& node,const NumericContract& supplied,
    std::array<OwnedProcessingContract,4>& outputs,std::array<std::size_t,4>* section_counts) noexcept {
    if(node.kind!="MusicalSpectrogram" && node.kind!="BroadbandRow" && node.kind!="NoteSpectrum")return Status::invalid_argument;
    MusicalLayout layout{node,MusicalParameters{node},supplied};
    auto status=layout.annotate();if(status!=Status::ok)return status;
    if(!valid_shape(layout.input) || !valid_frequency_axis(layout.input.coordinates[0]))return Status::incompatible_contract;
    status=layout.classify_quantity();if(status!=Status::ok)return status;
    status=layout.prepare_sections();if(status!=Status::ok)return status;
    std::array<OwnedProcessingContract,4> candidate;
    status=layout.materialize(candidate);if(status!=Status::ok)return status;
    outputs=std::move(candidate);if(section_counts)*section_counts=layout.sections;return Status::ok;
}
}
