#include <core_app/numeric_default_graph.h>
#include "example_builder.h"
#include <stdexcept>
namespace {
using namespace domain;using namespace signal_contracts;
OwnedProcessingContract series(examples::Builder& b,const OwnedProcessingContract& source,std::size_t count,DType dtype=DType::Float32) {
    auto value=std::get<NumericContract>(source.view());
    value.dtype=dtype;value.semantic=Semantic::Numeric;value.quantity=Quantity::Unspecified;value.unit=NumericUnit::Values;value.clock={};
    value.representation={};value.representation.alignment=dtype==DType::UInt8?1:alignof(float);
    value.shape={Rank::Vector,{Axis::Declared,Axis::None},{ExtentBounds{0,count},ExtentBounds{1,1}}};value.coordinates={};
    OwnedProcessingContract result;
    if(own_processing_contract(value,b.limits.frame,b.limits.max_owned_metadata_bytes,result)!=Status::ok)throw std::runtime_error("Reference beat series exceeds profile");
    return result;
}
OwnedProcessingContract scalar(examples::Builder& b,const OwnedProcessingContract& source) {
    const auto owned=series(b,source,1);
    auto value=std::get<NumericContract>(owned.view());value.shape={};
    OwnedProcessingContract result;
    if(own_processing_contract(value,b.limits.frame,b.limits.max_owned_metadata_bytes,result)!=Status::ok)throw std::runtime_error("Reference tempo scalar exceeds profile");
    return result;
}
void output(ConfiguredProcessingNode& node,PortId id,OwnedProcessingContract contract){node.outputs.push_back({id,PortDirection::Output,std::move(contract)});}
void spectral_frames(examples::Builder& b) {
    b.add(1,"AudioSource",EndpointProcessingParameters{{{"source",NumericLiteral{std::uint64_t{0}}},{"output_count",NumericLiteral{std::uint64_t{1}}}}});
    examples::add_window(b,1,20,22,21,23,2048);
    b.chain(2,"ArrayLength",1);
    std::get<TransformProcessingParameters>(b.node(22).parameters).values.push_back({"periodic",true});
    b.chain(24,"RealFourier",23,examples::transform({{"fft_size",NumericLiteral{std::uint64_t{2048}}}}));
    b.connect(21,24,0,window_size_input);
    b.chain(80,"Reroute",1,{},sample_rate_output);
    b.chain(81,"Reroute",80);b.chain(82,"Reroute",81);
    b.chain(84,"Reroute",2);
}
void power_mel(examples::Builder& b,std::uint32_t rate) {
    b.add(25,"AmplitudePhase",examples::transform({{"fft_size",NumericLiteral{std::uint64_t{2048}}},{"first_bin",NumericLiteral{std::uint64_t{1}}},
        {"sample_rate",NumericLiteral{std::uint64_t(rate)}},{"normalize",false},{"representation",std::string{"power"}},{"real_endpoints",true}}));
    b.connect(24,25,0,0);b.connect(24,25,1,1);b.derive(25);
    b.connect(21,25,0,window_size_input);b.connect(80,25,0,target_sample_rate_input);
    b.connect(22,25,0,window_coefficients_input);
    b.chain(26,"Mel",25,examples::transform({{"bands",NumericLiteral{std::uint64_t{128}}},{"sample_rate",NumericLiteral{std::uint64_t(rate)}},
        {"fft_size",NumericLiteral{std::uint64_t{2048}}},{"first_bin",NumericLiteral{std::uint64_t{1}}},{"scale",std::string{"slaney"}},{"norm",std::string{"slaney"}}}));
    b.connect(21,26,0,window_size_input);b.connect(81,26,0,target_sample_rate_input);
}
void onset_history(examples::Builder& b,std::size_t frames) {
    b.chain(29,"PowerToDb",26,examples::transform({{"reference",NumericLiteral{1.f}},{"top_db_enabled",false}}));
    b.chain(30,"OnsetStrength",29,examples::transform({{"lag",NumericLiteral{std::uint64_t{1}}},{"max_size",NumericLiteral{std::uint64_t{1}}},
        {"center",false},{"detrend",false},{"aggregate",std::string{"none"}}}));
    b.chain(3,"FeatureAggregate",30,examples::transform({{"method",NumericLiteral{std::uint64_t{2}}}}));
    b.chain(27,"Window",3,examples::transform({{"size",NumericLiteral{std::uint64_t(frames)}}}));
    b.chain(31,"FrameStride",27,examples::transform({{"every_n",NumericLiteral{std::uint64_t{20}}}}));
}
void rate_connections(examples::Builder& b,NodeId target) {
    b.connect(82,target,0,target_sample_rate_input);b.connect(84,target,0,hop_samples_input);
}
void tempo(examples::Builder& b,std::uint32_t rate,std::size_t frames) {
    b.chain(38,"ArrayLength",27);
    b.chain(34,"AutocorrelationTempogram",31,examples::transform({{"window_length",NumericLiteral{std::uint64_t(frames)}},
        {"center",false},{"padding",NumericLiteral{std::uint64_t{0}}},{"frame_index",NumericLiteral{std::uint64_t{0}}},
        {"sample_rate",NumericLiteral{std::uint64_t(rate)}},{"hop_samples",NumericLiteral{std::uint64_t{512}}}}));
    b.connect(38,34,0,window_size_input);rate_connections(b,34);
    b.add(35,"TempoEstimate",examples::transform({{"sample_rate",NumericLiteral{std::uint64_t(rate)}},
        {"hop_samples",NumericLiteral{std::uint64_t{512}}},{"silence_zero",true}}));
    auto& node=b.node(35);node.inputs.clear();node.outputs.clear();
    output(node,0,scalar(b,b.output(34)));b.connect(34,35);rate_connections(b,35);
}
void tracker(examples::Builder& b,std::uint32_t rate,std::size_t frames) {
    b.add(4,"DynamicBeatPath",examples::transform({{"sample_rate",NumericLiteral{std::uint64_t(rate)}},
        {"hop_samples",NumericLiteral{std::uint64_t{512}}},{"tightness",NumericLiteral{100.}},{"trim",true}}));
    auto& node=b.node(4);node.inputs.clear();node.outputs.clear();
    output(node,0,series(b,b.output(31),frames,DType::UInt8));
    output(node,1,series(b,b.output(31),frames));output(node,2,series(b,b.output(31),frames));output(node,3,series(b,b.output(31),frames,DType::Int32));
    b.connect(31,4);b.connect(35,4,0,1);rate_connections(b,4);
}
void views(examples::Builder& b) {
    b.chain(10,"VectorLineView",27);b.chain(11,"VectorLineView",4);
    b.chain(13,"ScalarMetricsView",35);b.chain(14,"VectorLineView",4,{},1);
    b.chain(40,"VectorLineView",20);b.chain(44,"VectorLineView",25);b.chain(45,"VectorLineView",26);
    b.chain(47,"VectorLineView",34);b.chain(49,"VectorLineView",22);
    b.chain(37,"Window",35,examples::transform({{"size",NumericLiteral{std::uint64_t{256}}}}));
    b.chain(48,"VectorLineView",37);
}
void replace_source(examples::Builder& b,NodeId target,PortId input,NodeId from) {
    for(auto& edge:b.graph.edges)if(edge.to==target && edge.input_port==input){edge.from=from;edge.output_port=0;return;}
    throw std::runtime_error("Missing reference graph connection");
}
void route_controls(examples::Builder& b) {
    b.chain(79,"Reroute",1,{},sample_rate_output);replace_source(b,80,0,79);
    b.chain(95,"Reroute",21);b.chain(96,"Reroute",95);
    replace_source(b,25,window_size_input,95);replace_source(b,26,window_size_input,96);
}
void route_data(examples::Builder& b) {
    b.chain(90,"Reroute",31);replace_source(b,4,0,90);
}

void layout(NumericExampleGraph& graph) {
    graph.positions={{1,581.9688349021903,4.931939278832118},
        {2,1538.9837316811143,-344.2774017901271},
        {3,8295.829062420167,239.44939847020026},
        {4,13846.544133764792,229.0656501583145},
        {10,9457.897080022904,4.602137303799623},
        {11,14570.155878328082,4.816248289433247},
        {13,13526.971446601245,-121.94093135575461},
        {14,14841.22694835905,186.19762226010778},
        {20,1167.9423946875916,0},
        {21,2169.7074348333654,657.1740459789736},
        {22,2131.9574054315326,260.0809110497755},
        {23,2713.9876525071636,4.91849471930189},
        {24,3318.551900246632,63.88051790067465},
        {25,4200.661788540337,165.39840428784018},
        {26,5535.33329287701,55.86901757197245},
        {27,8927.600154198306,226.35233691469375},
        {29,6785.717100578102,106.0366944948906},
        {30,7457.262116714808,215.35386151722415},
        {31,9872.42735490496,222.41783723802436},
        {34,11645.217805766839,192.62257981361506},
        {35,12811.407194411562,303.32604037517416},
        {37,13500.531968165546,874.5424041485528},
        {38,10184.086838950656,58.953834256932055},
        {40,1748.9001939651364,-104.12657787461183},
        {44,5190.0546791463985,-12.52011942013121},
        {45,6147.192247365966,-166.0473654264673},
        {47,12194.002651263447,167.02554861624344},
        {48,14037.6948479101,658.2668427244064},
        {49,2550.,-330.},
        {79,1662.8888582095365,769.7002381843729},
        {80,3574.280429071281,768.4856876969768},
        {81,5128.404991740932,871.1333214200267},
        {82,11793.155734455559,1187.4007896710687},
        {84,11160.036380391179,-668.1133574991313},
        {90,12766.279196261521,-297.68084702661145},
        {95,3575.3998899746507,616.4129222444174},
        {96,4913.301138850318,601.2324324885001}};
    graph.port_orders={{4,{0,hop_samples_input,1,target_sample_rate_input},{}},
        {25,{0,1,window_coefficients_input,window_size_input,target_sample_rate_input},{}},
        {26,{0,window_size_input,target_sample_rate_input},{}},
        {34,{hop_samples_input,window_size_input,0,target_sample_rate_input},{}},
        {35,{hop_samples_input,0,target_sample_rate_input},{}}};
    graph.node_names={{10,"Onset strength history"},{11,"Beat grid history"},{13,"Tempo, BPM"},{14,"Beat local scores"},
        {40,"Short signal window"},{44,"Spectral power"},{45,"Linear Mel values"},{47,"Autocorrelation"},{48,"Tempo history"},{49,"Window coefficients"}};
    graph.scalar_names={{13,{"Tempo, BPM"}}};
}
}
NumericExampleGraph numeric_reference_beat_example(std::uint32_t rate) {
    if(rate<8000 || rate>96000)throw std::invalid_argument("Reference beat sample rate invalid");
    const auto frames=std::size_t(rate)*8/512+1;
    examples::Builder b({rate,2048,512,400});b.graph.graph_id="reference-beat";
    spectral_frames(b);power_mel(b,rate);onset_history(b,frames);tempo(b,rate,frames);tracker(b,rate,frames);views(b);route_controls(b);route_data(b);
    NumericExampleGraph graph{"reference-beat","Rhythm: streaming spectral pipeline",b.finish()};layout(graph);return graph;
}
