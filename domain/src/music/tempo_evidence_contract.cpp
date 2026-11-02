#include <domain/music/contracts.h>
#include <domain/processing_contracts.h>
#include <algorithm>
#include <cmath>

namespace domain {
using namespace signal_contracts;
namespace {
template<class T>
bool number(const ResolvedProcessingNode& node,std::string_view key,T& result) noexcept {
    ProcessingParameterView value;
    if(resolve_processing_parameter(node,key,value)!=Status::ok)return false;
    const auto* literal=std::get_if<NumericLiteral>(&value);
    if(!literal)return false;
    result=std::visit([](auto v){return T(v);},*literal);return std::isfinite(result);
}
bool sizes(const ResolvedProcessingNode& node,TempoEvidenceParameters& p) noexcept {
    double rate=0,short_size=0,long_size=0;
    if(!number(node,"frame_rate_fps",rate) || !number(node,"short_fft",short_size) || !number(node,"long_fft",long_size))return false;
    if(rate<10 || rate>1000 || std::floor(rate)!=rate || short_size<32 || long_size>16384 || long_size<short_size ||
       std::floor(short_size)!=short_size || std::floor(long_size)!=long_size)return false;
    p.frame_rate_fps=std::uint64_t(rate);p.short_fft=std::size_t(short_size);p.long_fft=std::size_t(long_size);
    return !(p.short_fft&(p.short_fft-1)) && !(p.long_fft&(p.long_fft-1));
}
bool native_mono(const NumericContract& c) noexcept {
    return c.channel_order.size()==1 && c.representation.encoding==StorageEncoding::Native &&
        c.representation.layout==ChannelLayout::Planar && !c.representation.valid_bits && c.representation.normalization==Normalization::None;
}
bool step_matches(const AxisCoordinates& axis,double step) noexcept {
    return axis.mode==CoordinateMode::Regular && axis.origin.numerator==0 && axis.step.denominator &&
        std::abs(double(axis.step.numerator)/double(axis.step.denominator)-step)<1e-12;
}
bool curve_valid(const NumericContract& c,PortId port,const TempoEvidenceParameters& p) noexcept {
    const auto size=port%2?p.long_fft:p.short_fft;
    if(!native_mono(c) || c.dtype!=DType::Float32 || c.shape.rank!=Rank::Vector)return false;
    const auto& axis=c.coordinates[0];
    const auto bins=c.shape.extents[0];
    if(port<2)return c.shape.axes[0]==Axis::Bin && bins.minimum==bins.maximum &&
        (bins.maximum==size/2 || bins.maximum==size/2+1) && axis.unit==NumericUnit::Hertz &&
        (axis.mode==CoordinateMode::ExplicitFloat64LE || step_matches(axis,double(p.frame_rate_fps)/double(size)));
    return c.shape.axes[0]==Axis::Time && c.shape.extents[0].maximum>=2 && c.shape.extents[0].maximum<=size &&
        axis.unit==NumericUnit::Seconds && step_matches(axis,1./double(p.frame_rate_fps));
}
bool clock_equal(const ClockContract& a,const ClockContract& b) noexcept {
    return a.domain==b.domain && a.tick_seconds==b.tick_seconds && a.sample_rate==b.sample_rate;
}
bool count_valid(const NumericContract& c) noexcept {
    return native_mono(c) && c.shape.rank==Rank::Scalar && c.dtype==DType::UInt64 && c.unit==NumericUnit::Frames;
}
bool compatible_input(const ProcessingPort& input,const TempoEvidenceParameters& p,const ClockContract& clock) noexcept {
    const auto data=input.contract.view();const auto* c=std::get_if<NumericContract>(&data);
    return input.id<tempo_evidence_inputs && c && clock_equal(clock,c->clock) &&
        (input.id<4?curve_valid(*c,input.id,p):count_valid(*c));
}
NumericContract result_contract(const NumericContract& input,PortId port) noexcept {
    auto c=input;c.semantic=Semantic::Numeric;c.history_element=Semantic::Numeric;c.quantity=Quantity::Unspecified;
    c.dtype=port==1?DType::UInt32:port==3?DType::UInt8:DType::Float32;
    c.representation={};c.representation.alignment=port==1?alignof(std::uint32_t):port==3?1:alignof(float);
    c.unit=NumericUnit::Values;c.coordinates={};c.shape={};
    if(port==0)c.shape={Rank::Matrix,{Axis::Declared,Axis::Component},{ExtentBounds{8,8},ExtentBounds{tempo_candidate_columns,tempo_candidate_columns}}};
    return c;
}
}
bool tempo_evidence_parameters(const ResolvedProcessingNode& node,TempoEvidenceParameters& out) noexcept {
    TempoEvidenceParameters p;
    if(!sizes(node,p) || !number(node,"min_bpm",p.min_bpm) || !number(node,"max_bpm",p.max_bpm) ||
       !number(node,"min_observation_s",p.min_observation_s) || !number(node,"mature_observation_s",p.mature_observation_s))return false;
    if(p.min_bpm<20 || p.max_bpm<=p.min_bpm || p.max_bpm>=30*float(p.frame_rate_fps) ||
       p.min_observation_s<=0 || p.min_observation_s>p.mature_observation_s)return false;
    out=p;return true;
}
bool tempo_evidence_parameters(const ConfiguredProcessingNode& node,TempoEvidenceParameters& out) noexcept {
    return tempo_evidence_parameters(ResolvedProcessingNode{node.id,node.kind,NodeRole::Processor,ProcessingOperation::TempoEvidence,
        node.inputs,node.outputs,&node.parameters,node.enabled},out);
}
bool tempo_evidence_inputs_valid(const ConfiguredProcessingNode& node,const TempoEvidenceParameters& p) noexcept {
    if(node.inputs.size()!=tempo_evidence_inputs)return false;
    const auto zero=std::find_if(node.inputs.begin(),node.inputs.end(),[](const auto& input){return input.id==0;});
    if(zero==node.inputs.end())return false;
    const auto base=zero->contract.view();const auto* first=std::get_if<NumericContract>(&base);
    if(!first || first->clock.domain.empty() || first->clock.tick_seconds.numerator<=0 || !first->clock.tick_seconds.denominator)return false;
    for(const auto& input:node.inputs){
        if(!compatible_input(input,p,first->clock))return false;
    }
    return true;
}
Status tempo_evidence_output_contracts(const ConfiguredProcessingNode& node,const NumericContract& input,
                                      std::array<OwnedProcessingContract,tempo_evidence_outputs>& out) noexcept {
    TempoEvidenceParameters p;
    if(!tempo_evidence_parameters(node,p) || !curve_valid(input,0,p))return Status::incompatible_contract;
    std::array<OwnedProcessingContract,tempo_evidence_outputs> result;
    for(PortId port=0;port<result.size();++port){
        const auto status=own_processing_contract(result_contract(input,port),FrameLimits{1,SIZE_MAX,SIZE_MAX,SIZE_MAX,SIZE_MAX,SIZE_MAX},SIZE_MAX,result[port]);
        if(status!=Status::ok)return status;
    }
    out=std::move(result);return Status::ok;
}
}
