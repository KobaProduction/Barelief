#include <domain/graph/node_factory.h>
#include <domain/processing_contracts.h>
#include <algorithm>
#include <array>
#include <numeric>
#include <utility>
namespace domain {
using namespace signal_contracts;
namespace {
constexpr std::uint32_t mono=0;
Ratio frequency_step(std::uint32_t sample_rate,std::uint32_t size) {
    const auto divisor=std::gcd(sample_rate,size);
    return {std::int64_t(sample_rate/divisor),size/divisor};
}
struct Factory {
    const ProcessingAdmissionLimits& limits;
    NodeFactorySettings settings;
    ConfiguredProcessingNode node;
    bool valid=true;
    NumericContract vector(Semantic semantic,std::size_t count,Axis axis=Axis::Sample) const {
        NumericContract value;value.dtype=DType::Float32;value.semantic=semantic;
        value.channel_order={&mono,1};value.clock={"local",{1,settings.sample_rate},settings.sample_rate};
        value.shape={Rank::Vector,{axis,Axis::None},{ExtentBounds{count,count},ExtentBounds{}}};
        return value;
    }
    NumericContract scalar(DType dtype=DType::Float32,NumericUnit unit=NumericUnit::Values) const {
        auto value=vector(Semantic::Scalar,1);value.shape={};value.dtype=dtype;value.unit=unit;
        value.representation.alignment=dtype==DType::Float64?alignof(double):dtype==DType::UInt64?alignof(std::uint64_t):alignof(float);
        return value;
    }
    void port(PortDirection direction,PortId id,const DataContract& value) {
        ProcessingPort result{id,direction,{}};
        valid=own_processing_contract(value,limits.frame,limits.max_owned_metadata_bytes,result.contract)==Status::ok && valid;
        auto& ports=direction==PortDirection::Input?node.inputs:node.outputs;
        ports.push_back(std::move(result));
    }
    void field(std::string_view key,ProcessingParameterValue value) {
        auto* p=std::get_if<TransformProcessingParameters>(&node.parameters);
        if(p)p->values.push_back({std::string(key),std::move(value)});
        else if(auto* endpoint=std::get_if<EndpointProcessingParameters>(&node.parameters))endpoint->values.push_back({std::string(key),std::move(value)});
        else valid=false;
    }
    void io(const DataContract& input,const DataContract& output,std::size_t inputs=1,std::size_t outputs=1) {
        node.inputs.clear();node.outputs.clear();
        for(PortId i=0;i<inputs;++i)port(PortDirection::Input,i,input);
        for(PortId i=0;i<outputs;++i)port(PortDirection::Output,i,output);
    }
    NumericContract spectrum(bool dc=false) const {
        auto value=vector(Semantic::SpectrumMagnitude,settings.fft_size/2+std::size_t(dc),Axis::Bin);
        value.quantity=Quantity::SpectrumPeakAmplitude;value.unit=NumericUnit::RawAmplitude;
        const auto step=frequency_step(settings.sample_rate,settings.fft_size);
        value.coordinates[0]={CoordinateMode::Regular,NumericUnit::Hertz,dc?Ratio{0,1}:step,step};
        return value;
    }
};
bool one_of(std::string_view kind,std::initializer_list<std::string_view> names) {
    return std::find(names.begin(),names.end(),kind)!=names.end();
}
void initialize_constant(Factory& f,std::string_view kind) {
        const bool real=kind=="Number";auto value=f.scalar(real?DType::Float32:DType::UInt32,kind=="Fs"?NumericUnit::Hertz:NumericUnit::Values);
        value.clock={"configuration",{1,120},0};f.port(PortDirection::Output,0,value);
        f.field("value",real?NumericLiteral{float{0}}:NumericLiteral{std::uint64_t(kind=="Fs"?f.settings.sample_rate:f.settings.fft_size)});return;
}
void initialize_source(Factory& f,std::string_view kind) {
    if(kind=="SampleClock"){f.node.parameters=SampleClockProcessingParameters{f.settings.sample_rate,"local"};return;}
    if(numeric_constant_kind(kind)){initialize_constant(f,kind);return;}
    auto value=f.vector(Semantic::PCM,f.settings.packet_samples);
    if(kind=="AudioSource" || kind=="NetworkInput" || kind=="TestSignal")f.field("sample_rate",NumericLiteral{std::uint64_t(f.settings.sample_rate)});
    if(kind=="SignalGenerator"){value.shape.extents[0].minimum=1;f.node.parameters=SignalGeneratorProcessingParameters{};}
    f.port(PortDirection::Output,0,value);
}
void initialize_routes(Factory& f,std::string_view kind) {
    const auto value=f.vector(Semantic::PCM,f.settings.packet_samples);
    if(kind=="InputSelector"){f.io(value,value,2,1);return;}
    RouteProcessingParameters route;route.outputs=one_of(kind,{"Gate","Reroute"})?1:min_route_outputs;
    f.node.parameters=route;f.io(value,value,1,route.outputs);
}
NumericContract matrix_input(Factory& f,std::string_view kind) {
    auto value=f.vector(Semantic::Numeric,50,Axis::Band);value.shape.rank=Rank::Matrix;
    value.shape.axes={Axis::Band,Axis::Time};value.shape.extents={ExtentBounds{50,50},ExtentBounds{f.settings.history_capacity,f.settings.history_capacity}};
    value.coordinates[1]={CoordinateMode::ExplicitFloat64LE,NumericUnit::Seconds,{0,1},{1,1}};
    if(kind=="MatrixTranspose"){std::swap(value.shape.axes[0],value.shape.axes[1]);std::swap(value.shape.extents[0],value.shape.extents[1]);std::swap(value.coordinates[0],value.coordinates[1]);}
    return value;
}
void initialize_matrix(Factory& f,std::string_view kind) {
    auto value=matrix_input(f,kind);
    OwnedProcessingContract row_input;
    if(kind=="MatrixRow") {
        if(matrix_fft_output_contract(value,512,row_input)!=Status::ok){f.valid=false;return;}
        value=std::get<NumericContract>(row_input.view());
    }
    if(kind=="MatrixWindowFunction")f.node.enabled=false;
    f.io(value,value);
}
void initialize_fourier(Factory& f,std::string_view kind) {
    const auto input=f.vector(Semantic::Windowed,f.settings.fft_size);
    f.field("fft_size",NumericLiteral{std::uint64_t(f.settings.fft_size)});
    f.io(input,input,kind=="Fourier"?2:1,4);
}
void initialize_polar(Factory& f,std::string_view kind) {
    auto input=f.vector(Semantic::Numeric,f.settings.fft_size/2,Axis::Bin);
    input.quantity=Quantity::SpectrumReal;input.unit=NumericUnit::RawAmplitude;
    f.io(input,input,2,2);
    if(kind=="AmplitudePhase")f.field("sample_rate",NumericLiteral{std::uint64_t(f.settings.sample_rate)});
}
NumericContract music_input(Factory& f,std::string_view kind) {
    auto input=f.spectrum(true);input.quantity=Quantity::SpectrumPower;input.unit=NumericUnit::SquaredAmplitude;
    if(kind=="RFFT")input=f.vector(Semantic::Windowed,f.settings.fft_size);
    if(kind=="SpectrumProjection") {
        input.quantity=Quantity::SpectrumComplex;input.unit=NumericUnit::RawAmplitude;
        input.shape.rank=Rank::Matrix;input.shape.axes[1]=Axis::Component;input.shape.extents[1]={2,2};
        input.coordinates[1]={CoordinateMode::Regular,NumericUnit::Values,{0,1},{1,1}};
    }
    if(one_of(kind,{"MusicalSpectrogram","BroadbandRow","NoteSpectrum"}))input=f.spectrum();
    return input;
}
void initialize_broadband_boundaries(Factory& f) {
    const auto* descriptor=parameter_descriptor(NodeKind::BroadbandFeatures,"boundaries_hz");
    if(!descriptor){f.valid=false;return;}
    const auto nyquist=float(f.settings.sample_rate)/2;
    NumericArray boundaries;
    for(const auto boundary:descriptor->default_array)if(boundary<nyquist)boundaries.values.push_back(boundary);
    if(descriptor->default_array.empty() || descriptor->default_array.back()>=nyquist)boundaries.values.push_back(nyquist);
    f.field("boundaries_hz",std::move(boundaries));
}
void initialize_music(Factory& f,std::string_view kind) {
    const auto input=music_input(f,kind);
    if(one_of(kind,{"RFFT","SpectrumProjection","FrequencyNormalizer"}))f.field("fft_size",NumericLiteral{std::uint64_t(f.settings.fft_size)});
    if(kind=="SpectrumProjection")f.field("sample_rate",NumericLiteral{std::uint64_t(f.settings.sample_rate)});
    if(kind=="FrequencyNormalizer"){f.field("sample_rate",NumericLiteral{std::uint64_t(f.settings.sample_rate)});f.field("first_bin",NumericLiteral{std::uint64_t{0}});}
    if(one_of(kind,{"BroadbandFeatures","NoteSalience","MusicalSpectrogram","BroadbandRow","NoteSpectrum"}))f.field("bin_hz",NumericLiteral{double(f.settings.sample_rate)/f.settings.fft_size});
    if(kind=="NoteSalience"){
        f.field("power_bandwidth_hz",NumericLiteral{double(f.settings.sample_rate)/f.settings.fft_size});
        f.field("coherent_gain",NumericLiteral{1.0});
    }
    if(kind=="BroadbandFeatures")initialize_broadband_boundaries(f);
    f.io(input,input,1,one_of(kind,{"MusicalSpectrogram","NoteSpectrum"})?4:1);
}
void initialize_generator(Factory& f) {
    auto input=f.scalar(DType::UInt32);f.io(input,f.vector(Semantic::Numeric,f.settings.fft_size));
    f.field("size",NumericLiteral{std::uint64_t(f.settings.fft_size)});
}
void initialize_metadata(Factory& f,std::string_view kind) {
    const auto input=kind=="FrequencyStep"?f.scalar(DType::UInt32,NumericUnit::Hertz):f.vector(Semantic::Numeric,f.settings.fft_size);
    f.io(input,input);
}
void initialize_history(Factory& f) {
    auto input=f.vector(Semantic::Numeric,50,Axis::Band);auto output=input;
    output.semantic=Semantic::History;output.history_element=input.semantic;
    output.shape={Rank::Matrix,{Axis::Time,Axis::Band},{ExtentBounds{0,f.settings.history_capacity},input.shape.extents[0]}};
    output.coordinates={AxisCoordinates{CoordinateMode::ExplicitFloat64LE,NumericUnit::Seconds,{0,1},{1,1}},input.coordinates[0]};
    f.node.parameters=HistoryProcessingParameters{f.settings.history_capacity};f.io(input,output);
    auto count=f.scalar(DType::UInt64,NumericUnit::Frames);count.quantity=Quantity::FilledFrameCount;
    f.port(PortDirection::Output,1,count);
}

void initialize_number_accumulator(Factory& f) {
    auto input=f.scalar();auto output=input;
    output.semantic=Semantic::History;output.history_element=input.semantic;
    output.shape={Rank::Vector,{Axis::Time,Axis::None},{ExtentBounds{0,f.settings.history_capacity},ExtentBounds{}}};
    output.coordinates[0]={CoordinateMode::ExplicitFloat64LE,NumericUnit::Seconds,{0,1},{1,1}};
    f.node.parameters=HistoryProcessingParameters{f.settings.history_capacity};f.io(input,output);
    auto count=f.scalar(DType::UInt64,NumericUnit::Frames);count.quantity=Quantity::FilledFrameCount;
    f.port(PortDirection::Output,1,count);
}
NumericContract view_input(Factory& f,std::string_view kind) {
    auto input=f.vector(Semantic::Numeric,f.settings.packet_samples);
    if(kind=="ScalarView" || one_of(kind,{"ScalarMetricsView","ScalarSignedBarView"}))input=f.scalar();
    if(kind=="FFTView")input=f.spectrum();
    if(kind=="MelView")input=f.vector(Semantic::MelEnergy,50,Axis::Band);
    if(kind=="WindowView" || kind=="WindowedView")input=f.vector(kind=="WindowView"?Semantic::Window:Semantic::Windowed,f.settings.fft_size);
    if(one_of(kind,{"MatrixImageView","MatrixSurfaceView","AudioImageView","AudioSurfaceView"})) {
        input.shape.rank=Rank::Matrix;
        input.shape.axes={Axis::Sample,Axis::Component};
        input.shape.extents={ExtentBounds{16,16},ExtentBounds{16,16}};
    }
    if(kind=="SpectrogramView") {
        input.semantic=Semantic::History;input.history_element=Semantic::Numeric;input.shape.rank=Rank::Matrix;
        input.shape.axes={Axis::Time,Axis::Band};input.shape.extents={ExtentBounds{0,f.settings.history_capacity},ExtentBounds{50,50}};
        input.coordinates[0]={CoordinateMode::ExplicitFloat64LE,NumericUnit::Seconds,{0,1},{1,1}};
    }
    return input;
}
void initialize_views(Factory& f,std::string_view kind) {
    f.port(PortDirection::Input,0,view_input(f,kind));
    if(one_of(kind,{"AudioLineView","AudioPointView","AudioBarView","AudioPieView"})) {
        f.field("bin_hz",NumericLiteral{0.0});
        f.field("first_bin",NumericLiteral{std::uint64_t{0}});
    }
}
void initialize_numeric(Factory& f,std::string_view kind) {
    auto input=f.vector(Semantic::Numeric,f.settings.packet_samples);
    if(kind=="Converter")f.node.parameters=ConverterProcessingParameters{};
    if(kind=="ScalarReduce"){f.node.parameters=ReduceProcessingParameters{};f.io(input,f.scalar());return;}
    if(kind=="IndexExtract"){
        auto output=input;output.shape={};output.coordinates={};output.channel_order=input.channel_order.first(1);
        f.io(input,output);return;
    }
    f.io(input,input,one_of(kind,{"ArrayMultiply","ArrayConcat"})?2:1,1);
}
NumericContract signal_input(Factory& f,std::string_view kind) {
    auto input=f.vector(Semantic::PCM,f.settings.packet_samples);
    if(kind=="StereoFFT")input=f.vector(Semantic::Windowed,f.settings.fft_size);
    if(one_of(kind,{"SpectrumGain","SpectrumMerge","BarkWeighting","Mel","SpectrumAmplitude"}))input=f.spectrum();
    if(kind=="SpectrumAmplitude")input.quantity=Quantity::SpectrumMagnitude;
    if(kind=="Mixer"){input.shape.extents[0].minimum=1;f.node.parameters=MixerProcessingParameters{};}
    return input;
}
void initialize_signal(Factory& f,std::string_view kind) {
    const auto input=signal_input(f,kind);auto output=f.vector(Semantic::PCM,f.settings.packet_samples);
    if(kind=="Window")f.field("size",NumericLiteral{std::uint64_t(f.settings.fft_size)});
    if(kind=="Window")f.field("input_rate",NumericLiteral{std::uint64_t(f.settings.sample_rate)});
    if(kind=="PcmResampler")f.field("input_rate",NumericLiteral{std::uint64_t(f.settings.sample_rate)});
    if(kind=="BarkWeighting" || kind=="Mel") {
        f.field("sample_rate",NumericLiteral{std::uint64_t(f.settings.sample_rate)});
        f.field("bin_hz",NumericLiteral{double(f.settings.sample_rate)/f.settings.fft_size});
        f.field("first_bin",NumericLiteral{std::uint64_t{1}});
    }
    if(one_of(kind,{"Mixer","SpectrumGain","SpectrumMerge","BarkWeighting"}))output=input;
    f.io(input,output,one_of(kind,{"Mixer","ChannelComparison"})?2:1,1);
    if(kind=="ChannelComparison"){f.node.outputs.clear();f.port(PortDirection::Output,0,f.scalar());}
}
void initialize_peak(Factory& f) {
    const auto input=f.spectrum();
    f.port(PortDirection::Input,0,input);
    f.port(PortDirection::Output,0,f.scalar(DType::Float32,NumericUnit::Hertz));
    f.port(PortDirection::Output,1,f.scalar(DType::Float32,NumericUnit::RawAmplitude));
    f.field("bin_hz",NumericLiteral{double(f.settings.sample_rate)/f.settings.fft_size});
    f.field("first_bin",NumericLiteral{std::uint64_t(1)});
}

void initialize_transport(Factory& f,std::string_view kind) {
    const StructuredContract wled{"wled-audio-sync",1,"audio-sync-44-le",{44,44}};
    if(kind=="WLEDOutput"){f.port(PortDirection::Input,0,wled);return;}
    auto input=f.vector(Semantic::MelEnergy,50,Axis::Band);
    if(kind=="WLEDEncode"){
        f.io(input,wled);
        f.port(PortDirection::Input,wled_peak_hz_input,f.scalar(DType::Float32,NumericUnit::Hertz));
        auto magnitude=f.scalar(DType::Float32,NumericUnit::RawAmplitude);
        magnitude.quantity=Quantity::SpectrumPeakAmplitude;
        f.port(PortDirection::Input,wled_magnitude_input,magnitude);
        return;
    }
    if(kind=="NetworkOutput"){f.port(PortDirection::Input,0,f.vector(Semantic::PCM,f.settings.packet_samples));return;}
    f.port(PortDirection::Input,0,input);
}
bool initialize_fourier_family(Factory& f,std::string_view kind) {
    if(kind=="Fourier" || kind=="RealFourier"){initialize_fourier(f,kind);return true;}
    if(kind=="ComplexPolar" || kind=="AmplitudePhase"){initialize_polar(f,kind);return true;}
    return false;
}
bool initialize_numerical(Factory& f,std::string_view kind) {
    if(kind.starts_with("Matrix")){initialize_matrix(f,kind);return true;}
    if(initialize_fourier_family(f,kind))return true;
    if(kind=="ArrayFill" || kind=="WindowFunction"){initialize_generator(f);return true;}
    if(one_of(kind,{"ArrayLength","SampleRate","WindowGain","FrequencyStep"})){initialize_metadata(f,kind);return true;}
    return false;
}
bool initialize_operations(Factory& f,std::string_view kind) {
    if(initialize_numerical(f,kind))return true;
    if(kind=="Spectrogram"){initialize_history(f);return true;}
    if(kind=="NumberAccumulator"){initialize_number_accumulator(f);return true;}
    if(one_of(kind,{"RFFT","SpectrumProjection","FrequencyNormalizer","BroadbandFeatures","NoteSalience","MusicalSpectrogram","BroadbandRow","NoteSpectrum","SpectralQuantize"})){initialize_music(f,kind);return true;}
    if(one_of(kind,{"Window","StereoFFT","SpectrumGain","SpectrumMerge","BarkWeighting","Mel","SpectrumAmplitude","PcmResampler","Mixer","ChannelComparison"})){initialize_signal(f,kind);return true;}
    return false;
}
void initialize(Factory& f,std::string_view kind,NodeRole role) {
    if(kind=="SpectrumPeak"){initialize_peak(f);return;}
    if(role==NodeRole::Source){initialize_source(f,kind);return;}
    if(one_of(kind,{"Splitter","Switch","Gate","Reroute","InputSelector"})){initialize_routes(f,kind);return;}
    if(const auto k=node_kind(kind);k && is_view(*k)){initialize_views(f,kind);return;}
    if(one_of(kind,{"WLED","WLEDEncode","WLEDOutput","NetworkOutput"})){initialize_transport(f,kind);return;}
    if(!initialize_operations(f,kind))initialize_numeric(f,kind);
}
ProcessingDiagnostic finish_factory(Factory& factory,std::string_view kind) {
    if(!factory.valid)return {ProcessingError::InvalidContract,factory.node.id};
    if(!factory.node.inputs.empty() && !factory.node.outputs.empty()) {
        const auto derived=derive_processing_node_contracts(factory.node,factory.limits);
        if(!derived && kind!="Converter" && kind!="WLEDEncode")return derived;
    }
    if(attach_numeric_parameter_ports(factory.node,factory.limits)!=Status::ok)return {ProcessingError::InvalidPort,factory.node.id};
    return {};
}
}
ProcessingDiagnostic make_processing_node(std::string_view kind,NodeId id,const ProcessingAdmissionLimits& limits,ConfiguredProcessingNode& out,NodeFactorySettings settings) {
    const auto registered=node_kind(kind);
    if(!registered && kind!="Converter")return {ProcessingError::UnknownKind,id};
    if(!settings.sample_rate || !settings.fft_size || !settings.packet_samples || !settings.history_capacity)return {ProcessingError::InvalidParameters,id};
    Factory factory{limits,settings,{}};factory.node.id=id;factory.node.kind=kind;
    const auto role=registered?node_role(*registered):NodeRole::Processor;
    factory.node.parameters=role==NodeRole::Processor?ProcessingParameters{TransformProcessingParameters{}}:ProcessingParameters{EndpointProcessingParameters{}};
    initialize(factory,kind,role);
    const auto finished=finish_factory(factory,kind);if(!finished)return finished;
    out=std::move(factory.node);return {};
}
ProcessingDiagnostic make_processing_node(NodeKind kind,NodeId id,const ProcessingAdmissionLimits& limits,ConfiguredProcessingNode& out,NodeFactorySettings settings) {
    return make_processing_node(kind_id(kind),id,limits,out,settings);
}
}
