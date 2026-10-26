#include <domain/music/contracts.h>
#include <domain/graph/admission.h>
#include <domain/signal/contracts.h>
#include <signal_contracts/window_policy.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <numeric>
namespace domain {
using namespace signal_contracts;
namespace {
struct SpectralContract {
    const ConfiguredProcessingNode& node;
    NumericContract input,output;
    bool number(std::string_view key,double& result) const {
        const ResolvedProcessingNode view{node.id,node.kind,NodeRole::Processor,ProcessingOperation::MusicTransform,node.inputs,node.outputs,&node.parameters,node.enabled};
        ProcessingParameterView value;
        if(resolve_processing_parameter(view,key,value)!=Status::ok)return false;
        const auto* literal=std::get_if<NumericLiteral>(&value);if(!literal)return false;
        result=std::visit([](auto v){return double(v);},*literal);return std::isfinite(result);
    }
    std::string_view text(std::string_view key) const {
        const ResolvedProcessingNode view{node.id,node.kind,NodeRole::Processor,ProcessingOperation::MusicTransform,node.inputs,node.outputs,&node.parameters,node.enabled};
        ProcessingParameterView value;
        if(resolve_processing_parameter(view,key,value)!=Status::ok)return {};
        const auto* text=std::get_if<std::string_view>(&value);return text?*text:std::string_view{};
    }
    bool fixed(std::size_t axis=0) const {return input.shape.extents[axis].minimum==input.shape.extents[axis].maximum;}
    AxisCoordinates frequency(std::uint64_t size) const {
        const auto divisor=std::gcd(std::uint64_t(input.clock.sample_rate),size);
        return {CoordinateMode::Regular,NumericUnit::Hertz,{0,1},{std::int64_t(input.clock.sample_rate/divisor),size/divisor}};
    }
};
bool ordinary_spectrum(const SpectralContract& context) {
    const auto& input=context.input;
    return (input.quantity==Quantity::SpectrumPower || input.quantity==Quantity::SpectrumBinPower) && input.unit==NumericUnit::SquaredAmplitude &&
        input.representation.normalization==Normalization::None && input.shape.rank==Rank::Vector &&
        input.shape.axes[0]==Axis::Bin && context.fixed() && input.shape.extents[0].maximum>=2;
}
bool spectrum_rate_matches(const NumericContract& input,const AxisCoordinates& axis,std::uint64_t samples,std::uint64_t divisor) {
    auto numerator=std::uint64_t(axis.step.numerator),denominator=axis.step.denominator;
    const auto factor=std::gcd(numerator,denominator);numerator/=factor;denominator/=factor;
    return numerator==input.clock.sample_rate/divisor && denominator==samples/divisor;
}
bool spectrum_grid(const SpectralContract& context) {
    if(!ordinary_spectrum(context))return false;
    const auto& input=context.input;const auto& axis=input.coordinates[0];
    const bool no_dc=axis.origin==axis.step;
    double bin_hz=0;
    if(!context.number("bin_hz",bin_hz) || bin_hz<=0 ||
       !declared_frequency_axis_matches(axis,bin_hz,no_dc?1:0))return false;
    const auto bins=input.shape.extents[0].maximum;
    if(bins>UINT32_MAX)return false;
    const auto samples=(no_dc?bins:bins-1)*2;const auto divisor=std::gcd(std::uint64_t(input.clock.sample_rate),samples);
    return spectrum_rate_matches(input,axis,samples,divisor);
}
bool declared_fft(const SpectralContract& context,std::size_t& out) {
    double size=0;
    if(!context.number("fft_size",size) || size<2 || size>UINT32_MAX || std::floor(size)!=size)return false;
    out=std::size_t(size);return !(out&(out-1));
}
Status real_fft(SpectralContract& context) {
    std::size_t size=0;if(!declared_fft(context,size))return Status::invalid_argument;
    const auto radix=context.text("radix");if(radix!="radix2" && radix!="radix4")return Status::invalid_argument;
    const auto& input=context.input;
    if(input.shape.rank!=Rank::Vector || input.shape.axes[0]!=Axis::Sample || !context.fixed() || input.shape.extents[0].maximum!=size)return Status::incompatible_contract;
    auto& output=context.output;output.quantity=Quantity::SpectrumComplex;output.unit=NumericUnit::RawAmplitude;
    output.shape={Rank::Matrix,{Axis::Bin,Axis::Component},{ExtentBounds{size/2+1,size/2+1},ExtentBounds{2,2}}};
    output.coordinates={context.frequency(size),AxisCoordinates{CoordinateMode::Regular,NumericUnit::Values,{0,1},{1,1}}};
    return Status::ok;
}
struct Projection {std::string_view id;Quantity quantity;NumericUnit unit;};
constexpr Projection projections[]{
    {"real",Quantity::SpectrumReal,NumericUnit::RawAmplitude},
    {"imaginary",Quantity::SpectrumImaginary,NumericUnit::RawAmplitude},
    {"phase",Quantity::SpectrumPhase,NumericUnit::Radians},
    {"magnitude",Quantity::SpectrumMagnitude,NumericUnit::RawAmplitude},
    {"peak_amplitude",Quantity::SpectrumPeakAmplitude,NumericUnit::RawAmplitude},
    {"power",Quantity::SpectrumPower,NumericUnit::SquaredAmplitude},
    {"psd",Quantity::SpectrumPsd,NumericUnit::SquaredAmplitudePerHertz},
    {"rms_amplitude",Quantity::SpectrumRmsAmplitude,NumericUnit::RawAmplitude},
    {"bin_power",Quantity::SpectrumBinPower,NumericUnit::SquaredAmplitude},
    {"window_scaled_magnitude",Quantity::SpectrumWindowScaledMagnitude,NumericUnit::RawAmplitude}};
bool projection_input(const SpectralContract& context,std::size_t size) {
    const auto& input=context.input;
    if(input.quantity!=Quantity::SpectrumComplex || input.unit!=NumericUnit::RawAmplitude || input.shape.rank!=Rank::Matrix || input.shape.axes!=std::array{Axis::Bin,Axis::Component})return false;
    if(!context.fixed() || !context.fixed(1) || input.shape.extents[1].maximum!=2 || input.shape.extents[0].maximum!=size/2+1)return false;
    return input.coordinates[0]==context.frequency(size) && input.coordinates[1]==AxisCoordinates{CoordinateMode::Regular,NumericUnit::Values,{0,1},{1,1}};
}
Status project_representation(SpectralContract& context) {
    const auto representation=context.text("representation");
    const auto selected=std::find_if(std::begin(projections),std::end(projections),[&](const auto& p){return p.id==representation;});
    if(selected==std::end(projections))return Status::invalid_argument;
    auto& output=context.output;output.shape.rank=Rank::Vector;output.shape.axes={Axis::Bin,Axis::None};output.shape.extents[1]={1,1};output.coordinates[1]={};
    output.quantity=selected->quantity;output.unit=selected->unit;return Status::ok;
}
Status project_spectrum(SpectralContract& context) {
    std::size_t size=0;if(!declared_fft(context,size))return Status::invalid_argument;
    double sample_rate=0;
    if(!context.number("sample_rate",sample_rate) || sample_rate<=0)return Status::invalid_argument;
    if(sample_rate!=context.input.clock.sample_rate)return Status::incompatible_contract;
    if(!projection_input(context,size))return Status::incompatible_contract;
    WindowFunction window;if(!parse_window_function(context.text("window_function"),window))return Status::invalid_argument;
    return project_representation(context);
}
Status normalize_spectrum(SpectralContract& context) {
    const auto status=validate_frequency_normalizer(context.node,context.input);
    if(status==Status::ok)context.output=context.input;
    return status;
}
bool band_boundaries(const SpectralContract& context,std::size_t& count) {
    const auto& node=context.node;
    const ResolvedProcessingNode view{node.id,node.kind,NodeRole::Processor,ProcessingOperation::MusicTransform,node.inputs,node.outputs,&node.parameters,node.enabled};
    ProcessingParameterView value;
    if(resolve_processing_parameter(view,"boundaries_hz",value)!=Status::ok)return false;
    const auto* edges=std::get_if<std::span<const float>>(&value);if(!edges || edges->size()<2 || edges->size()>129)return false;
    double previous=-1;
    for(const auto edge:*edges){if(!std::isfinite(edge) || edge<0 || edge<=previous || edge>context.input.clock.sample_rate*.5)return false;previous=edge;}
    count=edges->size()-1;return true;
}
struct BandStatistic {std::string_view id;Quantity quantity;NumericUnit unit=NumericUnit::SquaredAmplitude;};
constexpr BandStatistic band_statistics[]{
    {"sum_energy",Quantity::BroadbandEnergy},{"mean_power",Quantity::BroadbandPower},
    {"rms",Quantity::BroadbandRms,NumericUnit::RawAmplitude},{"maximum",Quantity::BroadbandMaximum},{"flux",Quantity::SpectralFlux}};
Quantity broadband_quantity(const BandStatistic& statistic,Quantity input) {
    if(input!=Quantity::SpectrumBinPower)return statistic.quantity;
    if(statistic.id=="sum_energy")return Quantity::BroadbandBinPowerSum;
    if(statistic.id=="mean_power")return Quantity::BroadbandBinPowerMean;
    if(statistic.id=="rms")return Quantity::BroadbandBinPowerRms;
    return statistic.id=="maximum"?Quantity::BroadbandBinPowerMaximum:Quantity::BroadbandBinPowerFlux;
}
Status broadband_features(SpectralContract& context) {
    if(!spectrum_grid(context))return Status::incompatible_contract;
    std::size_t count=0;if(!band_boundaries(context,count))return Status::invalid_argument;
    const auto statistic=context.text("statistic");
    const auto selected=std::find_if(std::begin(band_statistics),std::end(band_statistics),[&](const auto& p){return p.id==statistic;});
    if(selected==std::end(band_statistics))return Status::invalid_argument;
    auto& output=context.output;output.shape.axes={Axis::Band,Axis::None};output.shape.extents[0]={count,count};
    output.coordinates[0]={CoordinateMode::ExplicitFloat64LE,NumericUnit::Hertz,{0,1},{0,1}};
    output.quantity=broadband_quantity(*selected,context.input.quantity);output.unit=selected->unit;return Status::ok;
}
bool salience_representation(const SpectralContract& context) {
    const auto representation=context.text("input_representation");
    const auto selected=std::find_if(std::begin(projections),std::end(projections),[&](const auto& p){return p.id==representation;});
    if(selected==std::end(projections) || representation=="real" || representation=="imaginary" || representation=="phase")return false;
    double coherent=0;
    const bool legacy_scaled=representation=="magnitude" && context.input.quantity==Quantity::SpectrumWindowScaledMagnitude &&
        context.number("coherent_gain",coherent) && coherent==1;
    return (legacy_scaled || context.input.quantity==selected->quantity) && context.input.unit==selected->unit;
}
bool salience_nyquist(const SpectralContract& context,bool& out) {
    const auto& node=context.node;
    const ResolvedProcessingNode view{node.id,node.kind,NodeRole::Processor,ProcessingOperation::MusicTransform,node.inputs,node.outputs,&node.parameters,node.enabled};
    ProcessingParameterView value;
    if(resolve_processing_parameter(view,"include_nyquist",value)!=Status::ok)return false;
    const auto* selected=std::get_if<bool>(&value);if(!selected)return false;
    out=*selected;return true;
}
Status salience_grid(const SpectralContract& context) {
    const auto& input=context.input;
    if(input.shape.rank!=Rank::Vector || input.shape.axes[0]!=Axis::Bin || !context.fixed() ||
       input.shape.extents[0].maximum<2 || input.shape.extents[0].maximum>UINT32_MAX ||
       input.representation.normalization!=Normalization::None)return Status::incompatible_contract;
    double first=0,step=0,bandwidth=0,coherent_gain=0;bool nyquist=false;
    if(!context.number("first_bin",first) || (first!=0 && first!=1) || !context.number("bin_hz",step) || step<=0 ||
       !context.number("power_bandwidth_hz",bandwidth) || bandwidth<=0 ||
       !context.number("coherent_gain",coherent_gain) || coherent_gain<=0 ||
       !salience_nyquist(context,nyquist))return Status::invalid_argument;
    if(!salience_representation(context))return Status::incompatible_contract;
    const auto nyquist_bin=double(input.shape.extents[0].maximum)-1+first+double(!nyquist);
    if(std::abs(nyquist_bin*step-double(input.clock.sample_rate)*.5)>1e-9*step)return Status::incompatible_contract;
    OwnedProcessingContract annotated;
    return explicit_frequency_grid_contract(context.node,input,annotated);
}

Status note_salience(SpectralContract& context) {
    const auto grid=salience_grid(context);if(grid!=Status::ok)return grid;
    double first=0,count=0,tuning=0,harmonics=0;
    if(!context.number("first_midi",first) || !context.number("notes",count) || !context.number("a4_hz",tuning) || !context.number("harmonics",harmonics))return Status::invalid_argument;
    if(first<0 || first+count>128 || count<1 || count>128 || tuning<=0 || harmonics<1 || harmonics>16)return Status::invalid_argument;
    auto& output=context.output;output.shape.axes={Axis::Band,Axis::None};output.shape.extents[0]={std::uint64_t(count),std::uint64_t(count)};
    output.coordinates[0]={CoordinateMode::Regular,NumericUnit::Values,{std::int64_t(first),1},{1,1}};
    output.quantity=Quantity::NoteSalience;output.unit=NumericUnit::RawAmplitude;return Status::ok;
}
Status quantize_spectrum(SpectralContract& context) {
    double bits=0,minimum=0,maximum=0;
    if(context.input.shape.rank!=Rank::Vector || !context.fixed() || !context.number("bits",bits) || (bits!=8 && bits!=16) || !context.number("min_value",minimum) || !context.number("max_value",maximum) || minimum>=maximum)return Status::invalid_argument;
    context.output=context.input;context.output.dtype=bits==8?DType::UInt8:DType::UInt16;
    context.output.quantity=Quantity::Unspecified;context.output.unit=NumericUnit::Values;
    context.output.representation.normalization=Normalization::None;context.output.representation.alignment=bits==8?alignof(std::uint8_t):alignof(std::uint16_t);
    return Status::ok;
}
struct SpectralRule {std::string_view kind;Status (*derive)(SpectralContract&);};
constexpr SpectralRule spectral_rules[]{
    {"RFFT",real_fft},{"SpectrumProjection",project_spectrum},{"FrequencyNormalizer",normalize_spectrum},
    {"BroadbandFeatures",broadband_features},{"NoteSalience",note_salience},{"SpectralQuantize",quantize_spectrum}};
Status derive_spectrum(SpectralContract& context) {
    for(const auto& rule:spectral_rules)if(rule.kind==context.node.kind)return rule.derive(context);
    return Status::invalid_argument;
}
}
namespace {
Status materialize_music_contract(const ConfiguredProcessingNode& node,const NumericContract& input,OwnedProcessingContract& owner) {
    auto output=input;output.semantic=Semantic::Numeric;output.history_element=Semantic::Numeric;output.quantity=Quantity::Unspecified;output.unit=NumericUnit::Values;output.representation.normalization=Normalization::None;
    SpectralContract context{node,input,output};const auto status=derive_spectrum(context);
    if(status!=Status::ok)return status;
    return own_processing_contract(context.output,FrameLimits{1,SIZE_MAX,SIZE_MAX,SIZE_MAX,SIZE_MAX,SIZE_MAX},SIZE_MAX,owner);
}
}
Status music_output_contract(const ConfiguredProcessingNode& node,const NumericContract& input,OwnedProcessingContract& owner) noexcept {
    if(node.kind=="RhythmFeatures")return rhythm_features_output_contract(node,input,owner);
    if(node.kind=="MusicalSpectrogram") {
        std::array<OwnedProcessingContract,4> outputs;const auto status=musical_spectrogram_output_contracts(node,input,outputs);
        if(status==Status::ok)owner=std::move(outputs[0]);
        return status;
    }
    if(!std::holds_alternative<TransformProcessingParameters>(node.parameters) || input.dtype!=DType::Float32 || input.channel_order.size()!=1 || input.representation.encoding!=StorageEncoding::Native || input.representation.layout!=ChannelLayout::Planar || input.representation.valid_bits || !input.clock.sample_rate || input.clock.domain.empty())return Status::incompatible_contract;
    return materialize_music_contract(node,input,owner);
}
ProcessingDiagnostic note_salience_calibration_diagnostic(const ConfiguredProcessingNode& node,const NumericContract& input) noexcept {
    if(node.kind!="NoteSalience" || input.quantity!=Quantity::SpectrumWindowScaledMagnitude)return {};
    const SpectralContract context{node,input,{}};
    if(context.text("input_representation")!="magnitude")return {};
    double coherent=0;
    if(context.number("coherent_gain",coherent) && coherent==1)return {};
    return {ProcessingError::InvalidParameters,node.id,0,{},ProcessingReason::CoherentGain,"coherent_gain"};
}
}
