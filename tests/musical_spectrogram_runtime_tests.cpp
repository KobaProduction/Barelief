#include <dataflow_runtime/runtime.h>
#include <array>
#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <new>

using namespace domain;using namespace signal_contracts;using namespace dataflow_runtime;
namespace {
std::size_t allocations=0;
void check(bool condition,const char* message){if(!condition){std::fprintf(stderr,"FAIL: %s\n",message);std::exit(1);}}
void near(double actual,double expected){check(std::abs(actual-expected)<2e-5,"independent row arithmetic");}
constexpr FrameLimits frames{1,32768,262144,16384,256,8192};
constexpr ProcessingAdmissionLimits limits{32,64,16,16,256,65536,frames};
OwnedProcessingContract own(const NumericContract& c){OwnedProcessingContract result;check(own_processing_contract(c,frames,65536,result)==Status::ok,"own contract");return result;}
NumericRuntimeProfile profile(){NumericRuntimeProfile p{limits,{{4194304,4194304,4194304,131072},0,16777216},{32768,8,262144,262144,262144},{true,true,true}};p.music_enabled=true;return p;}
struct Result {
    std::array<std::array<float,256>,4> values{};std::array<std::size_t,4> count{};std::array<bool,4> available{};
    static void receive(void* context,const NumericPortFrameView& frame){if(frame.identity.node!=2 || frame.direction!=PortDirection::Output)return;
        auto& r=*static_cast<Result*>(context);const auto p=frame.identity.port;check(p<4,"four output IDs");r.available[p]=frame.frame.stamp.available;
        if(!r.available[p])return;const auto& view=std::get<NumericView>(frame.frame.payload);r.count[p]=view.contract->shape.rank==Rank::Scalar?1:view.actual[0];check(r.count[p]<=256,"bounded result");
        for(std::size_t i=0;i<r.count[p];++i)check(load_element(view,{0,i,0},r.values[p][i])==Status::ok,"load result");
    }
};
void scenario(){
    static constexpr std::uint32_t mono=0;NumericContract c;c.channel_order={&mono,1};c.dtype=DType::Float32;c.semantic=Semantic::Numeric;c.quantity=Quantity::SpectrumPeakAmplitude;
    c.unit=NumericUnit::RawAmplitude;c.clock={"test",{1,3200},3200};
    c.shape={Rank::Vector,{Axis::Bin,Axis::None},{ExtentBounds{16,16},ExtentBounds{}}};c.coordinates[0]={CoordinateMode::Regular,NumericUnit::Hertz,{100,1},{100,1}};c.representation.alignment=alignof(float);
    ConfiguredProcessingNode source;source.id=1;source.kind="NetworkInput";source.parameters=EndpointProcessingParameters{};source.outputs={{0,PortDirection::Output,own(c)}};
    ConfiguredProcessingNode row;row.id=2;row.kind="MusicalSpectrogram";row.parameters=TransformProcessingParameters{{
        {"low_low_hz",NumericLiteral{100.}},{"low_high_hz",NumericLiteral{400.}},
        {"mid_low_hz",NumericLiteral{400.}},{"mid_high_hz",NumericLiteral{1000.}},{"mid_count",NumericLiteral{std::uint64_t{2}}},
        {"high_low_hz",NumericLiteral{1000.}},{"high_high_hz",NumericLiteral{1600.}},{"high_count",NumericLiteral{std::uint64_t{2}}},{"high_reduction",std::string{"rms"}},
        {"first_midi",NumericLiteral{std::uint64_t{69}}},{"last_midi",NumericLiteral{std::uint64_t{69}}},
        {"bin_hz",NumericLiteral{100.}},{"first_bin",NumericLiteral{std::uint64_t{1}}},
        {"input_representation",std::string{"peak_amplitude"}}}};
    row.inputs={{0,PortDirection::Input,own(c)}};std::array<OwnedProcessingContract,4> outputs;check(musical_spectrogram_output_contracts(row,c,outputs)==Status::ok,"derive row outputs");
    for(PortId p=0;p<4;++p)row.outputs.push_back({p,PortDirection::Output,outputs[p]});
    NumericProcessingGraph graph;graph.graph_id="musical-row";graph.nodes={source,row};graph.edges={{1,2,0,0}};
    std::shared_ptr<const PreparedGraph> plan;std::unique_ptr<RuntimeContext> state;const auto prepared=PreparedGraph::prepare_numeric(graph,profile(),plan,state);
    if(prepared!=Status::ok)std::fprintf(stderr,"prepare status=%u\n",unsigned(prepared));check(prepared==Status::ok,"prepare four-part row");
    std::array<float,16> input{};for(std::size_t i=0;i<input.size();++i)input[i]=float(i+1);
    const PlaneLayout plane{0,{sizeof(float),0}};NumericView view{&c,std::as_bytes(std::span(input)),{&plane,1},{16,1},{},{}};
    FrameStamp stamp;stamp.available=true;stamp.sequence=1;stamp.producer_epoch=1;stamp.timestamp_ticks=16;Result result;
    auto before=allocations;check(plan->process_numeric_ports(1,{stamp,view},*state,{}, {Result::receive,nullptr,nullptr,&result})==Status::ok,"process row");check(allocations==before,"row hot path allocates");
    check(result.available[0] && result.count[0]==8 && result.available[3] && result.count[3]==1,"row sections and note output geometry");
    near(result.values[0][0],1);near(result.values[0][1],2);near(result.values[0][2],3);near(result.values[0][3],5);near(result.values[0][4],8);
    near(result.values[0][5],std::sqrt(365./3));near(result.values[0][6],std::sqrt(846./4));near(result.values[0][7],4.4);near(result.values[3][0],4.4);
        input.fill(0);for(std::size_t k=2;k<=4;++k)input[k]=float(1-.25*std::pow(double(k)-3.4,2));stamp.sequence++;stamp.timestamp_ticks++;
        before=allocations;check(plan->process_numeric_ports(1,{stamp,view},*state,{}, {Result::receive,nullptr,nullptr,&result})==Status::ok,"process A4 interpolated peak");check(allocations==before,"peak score hot path allocates");
        check(result.available[1] && result.available[2],"A4 full/selected scores defined");near(result.values[1][0],100);near(result.values[2][0],100);
        input.fill(0);stamp.sequence++;stamp.timestamp_ticks++;check(plan->process_numeric_ports(1,{stamp,view},*state,{}, {Result::receive,nullptr,nullptr,&result})==Status::ok,"process silence");
        check(result.available[0] && !result.available[1] && !result.available[2],"silence has valid zero row and unavailable agreements");
    auto wrong_unit=c;wrong_unit.unit=NumericUnit::Radians;check(musical_spectrogram_output_contracts(row,wrong_unit,outputs)!=Status::ok,"quantity/unit mismatch rejected");
    auto nonlinear=c;nonlinear.unit=NumericUnit::Decibels;nonlinear.quantity=Quantity::SpectrumAmplitudeDb;
    check(musical_spectrogram_output_contracts(row,nonlinear,outputs)!=Status::ok,"nonlinear dB input rejected");
    auto invalid=row;std::get<TransformProcessingParameters>(invalid.parameters).values.push_back({"mid_reduction",std::string{"invalid"}});
    check(musical_spectrogram_output_contracts(invalid,c,outputs)!=Status::ok,"invalid statistic rejected");
    auto malformed=graph;malformed.nodes[1].outputs[2].contract=malformed.nodes[1].outputs[0].contract;
    check(PreparedGraph::prepare_numeric(malformed,profile(),plan,state)!=Status::ok,"forged scalar contract rejected");
}

std::uint64_t history_fill(const PreparedGraph& plan,RuntimeContext& state,NodeId id) {
    DataFrameView frame;std::uint64_t count=999;
    check(plan.history(id,0,state,frame,&count)==Status::ok,"read content-continuity history fill");return count;
}
void check_topology_history(const NumericProcessingGraph& graph,const PreparedGraph& previous,
    const RuntimeContext& state,std::uint64_t expected) {
    std::shared_ptr<const PreparedGraph> next;std::unique_ptr<RuntimeContext> context;
    check(PreparedGraph::prepare_numeric(graph,profile(),next,context,&previous,&state)==Status::ok,"prepare explicit content topology revision");
    check(history_fill(*next,*context,3)==expected,"content stage insertion/removal/replacement continuity");
    check(history_fill(*next,*context,5)==2,"content topology edit reset unrelated branch");
}
void history_topology_reprepare(const NumericProcessingGraph& graph,const PreparedGraph& plan,const RuntimeContext& state) {
    auto candidate=graph;
    candidate.nodes[1].id=20;
    for(auto& edge:candidate.edges){if(edge.from==2)edge.from=20;if(edge.to==2)edge.to=20;}
    check_topology_history(candidate,plan,state,0);
    candidate=graph;
    auto source=graph.nodes.front();source.id=7;source.outputs={{0,PortDirection::Output,graph.nodes[2].inputs.front().contract}};
    candidate.nodes.push_back(std::move(source));
    std::erase_if(candidate.edges,[](const auto& edge){return edge.to==3;});candidate.edges.push_back({7,3,0,0});
    check_topology_history(candidate,plan,state,0);
}
void history_neutral_reprepare(const NumericProcessingGraph& graph,const PreparedGraph& plan,const RuntimeContext& state) {
    auto candidate=graph;const auto contract=graph.nodes[2].inputs.front().contract;
    ConfiguredProcessingNode gate;gate.id=7;gate.kind="Gate";gate.parameters=RouteProcessingParameters{1,0,true};
    gate.inputs={{0,PortDirection::Input,contract}};gate.outputs={{0,PortDirection::Output,contract}};
    candidate.nodes.push_back(std::move(gate));
    std::erase_if(candidate.edges,[](const auto& edge){return edge.to==3;});candidate.edges.push_back({2,7,0,0});candidate.edges.push_back({7,3,0,0});
    check_topology_history(candidate,plan,state,2);
    candidate=graph;std::erase_if(candidate.edges,[](const auto& edge){return edge.to==3;});
    check_topology_history(candidate,plan,state,2);
}
void history_partition_reprepare() {
    static constexpr std::uint32_t mono=0;
    NumericContract input;input.channel_order={&mono,1};input.dtype=DType::Float32;input.semantic=Semantic::Numeric;
    input.quantity=Quantity::SpectrumMagnitude;input.unit=NumericUnit::RawAmplitude;
    input.clock={"history-test",{1,3200},3200};input.representation.alignment=alignof(float);
    input.shape={Rank::Vector,{Axis::Bin,Axis::None},{ExtentBounds{16,16},ExtentBounds{}}};
    input.coordinates[0]={CoordinateMode::Regular,NumericUnit::Hertz,{100,1},{100,1}};
    ConfiguredProcessingNode source;source.id=1;source.kind="NetworkInput";source.parameters=EndpointProcessingParameters{};
    source.outputs={{0,PortDirection::Output,own(input)}};
    ConfiguredProcessingNode row;row.id=2;row.kind="MusicalSpectrogram";
    row.parameters=TransformProcessingParameters{{{"first_midi",NumericLiteral{std::uint64_t{69}}},{"last_midi",NumericLiteral{std::uint64_t{72}}},
        {"bin_hz",NumericLiteral{100.}},{"first_bin",NumericLiteral{std::uint64_t{1}}},{"input_representation",std::string{"magnitude"}}}};
    row.inputs={{0,PortDirection::Input,own(input)}};
    std::array<OwnedProcessingContract,4> projected;
    check(musical_spectrogram_output_contracts(row,input,projected)==Status::ok,"derive retained row");
    for(PortId p=0;p<4;++p)row.outputs.push_back({p,PortDirection::Output,projected[p]});
    const auto history=[&](NodeId id){
        const auto value=std::get<NumericContract>(projected[0].view());auto matrix=value;
        matrix.semantic=Semantic::History;matrix.history_element=value.semantic;
        matrix.shape={Rank::Matrix,{Axis::Time,Axis::Band},{ExtentBounds{0,4},value.shape.extents[0]}};
        matrix.coordinates={AxisCoordinates{CoordinateMode::ExplicitFloat64LE,NumericUnit::Seconds},value.coordinates[0]};
        NumericContract count;count.dtype=DType::UInt64;count.channel_order={&mono,1};count.clock=value.clock;
        count.quantity=Quantity::FilledFrameCount;count.unit=NumericUnit::Frames;count.representation.alignment=alignof(std::uint64_t);
        ConfiguredProcessingNode result;result.id=id;result.kind="Spectrogram";result.parameters=HistoryProcessingParameters{4};
        result.inputs={{0,PortDirection::Input,projected[0]}};
        result.outputs={{0,PortDirection::Output,own(matrix)},{1,PortDirection::Output,own(count)}};return result;
    };
    auto unaffected=row;unaffected.id=4;
    auto independent_source=source;independent_source.id=6;
    NumericProcessingGraph graph;graph.graph_id="row-partition-history";
    graph.nodes={source,row,history(3),unaffected,history(5),independent_source};
    graph.edges={{1,2,0,0},{2,3,0,0},{6,4,0,0},{4,5,0,0}};
    std::shared_ptr<const PreparedGraph> plan;std::unique_ptr<RuntimeContext> state;
    check(PreparedGraph::prepare_numeric(graph,profile(),plan,state)==Status::ok,"prepare retained history branches");
    std::array<float,16> values{};for(std::size_t i=0;i<values.size();++i)values[i]=float(i+1);
    const PlaneLayout plane{0,{sizeof(float),0}};NumericView view{&input,std::as_bytes(std::span(values)),{&plane,1},{16,1},{},{}};
    FrameStamp stamp;stamp.available=true;stamp.producer_epoch=1;
    for(unsigned i=1;i<=2;++i){stamp.sequence=i;stamp.timestamp_ticks=i;
        check(plan->process_numeric_ports(1,{stamp,view},*state,{})==Status::ok &&
            plan->process_numeric_ports(6,{stamp,view},*state,{})==Status::ok,"fill both row histories");}
    const auto filled=[&](const PreparedGraph& p,RuntimeContext& context,NodeId id){DataFrameView frame;std::uint64_t count=999;
        check(p.history(id,0,context,frame,&count)==Status::ok,"read history fill");return count;};
    check(filled(*plan,*state,3)==2 && filled(*plan,*state,5)==2,"initial histories not filled equally");
    for(unsigned change=0;change<5;++change){
        auto candidate=graph;auto& parameters=std::get<TransformProcessingParameters>(candidate.nodes[1].parameters).values;
        if(change==0){parameters[0].value=NumericLiteral{std::uint64_t{70}};parameters[1].value=NumericLiteral{std::uint64_t{73}};}
        if(change==1)parameters.push_back({"mid_reduction",std::string{"rms"}});
        if(change==2)parameters.push_back({"mid_low_hz",NumericLiteral{350.}});
        if(change==4)parameters.push_back({"a4_hz",NumericLiteral{442.}});
        auto changed_input=input;
        if(change==3){changed_input.quantity=Quantity::SpectrumPeakAmplitude;
            auto representation=std::find_if(parameters.begin(),parameters.end(),[](const auto& field){return field.key=="input_representation";});
            check(representation!=parameters.end(),"explicit row representation");representation->value=std::string{"peak_amplitude"};
            candidate.nodes[0].outputs[0].contract=own(changed_input);candidate.nodes[1].inputs[0].contract=own(changed_input);}
        std::array<OwnedProcessingContract,4> changed_outputs;
        check(musical_spectrogram_output_contracts(candidate.nodes[1],changed_input,changed_outputs)==Status::ok,"derive changed partition");
        check(same_physical_contract(changed_outputs[0].view(),projected[0].view()),"test partition must retain row physical shape");
        std::shared_ptr<const PreparedGraph> next;std::unique_ptr<RuntimeContext> next_state;
        check(PreparedGraph::prepare_numeric(candidate,profile(),next,next_state,plan.get(),state.get())==Status::ok,"reprepare same-size partition change");
        check(filled(*next,*next_state,3)==0,"changed note grid/statistic/boundaries did not reset affected History");
        check(filled(*next,*next_state,5)==2,"partition change discarded independent History");
        stamp.sequence=3;stamp.timestamp_ticks=3;
        struct NoteAxes {
            std::array<std::array<double,4>,2> hz{};
            std::array<std::array<float,4>,2> values{};
            std::array<bool,2> received{};
            static void receive(void* context,const NumericPortFrameView& frame){
                if(frame.identity.port!=3 || (frame.identity.node!=2 && frame.identity.node!=4) || !frame.frame.stamp.available)return;
                auto& result=*static_cast<NoteAxes*>(context);const auto slot=frame.identity.node==2?0u:1u;
                const auto& value=std::get<NumericView>(frame.frame.payload);const auto bytes=value.coordinates.axes[0];
                check(value.actual[0]==4 && bytes.size()==32,"prepared note Hz geometry changed unexpectedly");
                for(std::size_t note=0;note<4;++note){std::uint64_t bits=0;
                    for(std::size_t byte=0;byte<8;++byte)bits|=std::uint64_t(std::to_integer<unsigned char>(bytes[note*8+byte]))<<(byte*8);
                    result.hz[slot][note]=std::bit_cast<double>(bits);
                    check(load_element(value,{0,note,0},result.values[slot][note])==Status::ok,"load retuned note payload");
                }
                result.received[slot]=true;
            }
        } axes;
        const NumericRuntimeCallbacks observed{NoteAxes::receive,nullptr,nullptr,&axes};
        const auto hot_allocations=allocations;
        auto changed_view=view;changed_view.contract=&changed_input;
        check(next->process_numeric_ports(1,{stamp,changed_view},*next_state,{},observed)==Status::ok &&
            next->process_numeric_ports(6,{stamp,view},*next_state,{},observed)==Status::ok,"process reconfigured row histories");
        check(allocations==hot_allocations,"reconfigured history path allocates");
        check(filled(*next,*next_state,3)==1 && filled(*next,*next_state,5)==3,"post-reprepare history counts not independent");
        check(axes.received[0] && axes.received[1],"missing reconfigured / independent note outputs");
        for(std::size_t note=0;note<4;++note){
            const double expected=(change==4?442.:440.)*std::pow(2.,(double(note)+(change==0?70.:69.)-69)/12);
            const double untouched=440.*std::pow(2.,double(note)/12);
            check(std::abs(axes.hz[0][note]-expected)<1e-9,"retained metadata overwrote new prepared note Hz");
            check(std::abs(axes.hz[1][note]-untouched)<1e-9,"reconfigured note Hz changed independent branch");
            near(axes.values[0][note],expected/100);near(axes.values[1][note],untouched/100);
        }
    }
    history_topology_reprepare(graph,*plan,*state);
    history_neutral_reprepare(graph,*plan,*state);
}
ConfiguredProcessingNode& content_node(NumericProcessingGraph& graph,NodeId id) {
    const auto found=std::find_if(graph.nodes.begin(),graph.nodes.end(),[=](const auto& value){return value.id==id;});
    check(found!=graph.nodes.end(),"content fixture node exists");return *found;
}
void swap_concat_contributions(NumericProcessingGraph& graph) {
    auto& join=content_node(graph,4);
    std::swap(join.inputs[0].contract,join.inputs[1].contract);
    for(auto& edge:graph.edges)if(edge.to==4)edge.input_port=PortId(1-edge.input_port);
    OwnedProcessingContract output;
    check(array_concat_output_contract(join,output)==Status::ok,"derive reordered concat");join.outputs.front().contract=std::move(output);
}
void check_concat_revision(NumericProcessingGraph graph,const PreparedGraph& plan,const RuntimeContext& state) {
    swap_concat_contributions(graph);
    std::shared_ptr<const PreparedGraph> next;std::unique_ptr<RuntimeContext> context;
    check(PreparedGraph::prepare_numeric(graph,profile(),next,context,&plan,&state)==Status::ok,"prepare reordered content contributions");
    check(history_fill(*next,*context,5)==0,"ordered concat contributions did not invalidate combined History");
    check(history_fill(*next,*context,6)==2 && history_fill(*next,*context,7)==2,"concat reordering reset independent input histories");
}
NumericProcessingGraph neutral_concat_branch(NumericProcessingGraph graph) {
    auto source=graph.nodes.front();const auto output=content_node(graph,3).outputs.front().contract;
    source.id=3;source.outputs={{0,PortDirection::Output,output}};
    content_node(graph,3)=std::move(source);
    std::erase_if(graph.edges,[](const auto& edge){return edge.to==3;});return graph;
}
void fill_concat_baseline(const PreparedGraph& plan,RuntimeContext& state,const NumericView& spectrum,const NumericView& notes) {
    FrameStamp stamp;stamp.available=true;stamp.producer_epoch=1;
    for(unsigned sequence=1;sequence<=2;++sequence){
        stamp.sequence=sequence;stamp.timestamp_ticks=sequence;
        check(plan.process_numeric_ports(1,{stamp,spectrum},state,{})==Status::ok,"fill semantic concat input");
        check(plan.process_numeric_ports(3,{stamp,notes},state,{})==Status::ok,"fill neutral concat input");
    }
    check(history_fill(plan,state,5)==2 && history_fill(plan,state,6)==2 && history_fill(plan,state,7)==2,"ordered concat baseline histories not filled");
}
void concat_empty_contributions(NumericProcessingGraph graph,const PreparedGraph& previous,RuntimeContext& previous_state,
    const NumericView& spectrum,std::span<const float> notes) {
    graph=neutral_concat_branch(std::move(graph));
    const auto contract=std::get<NumericContract>(content_node(graph,3).outputs.front().contract.view());
    DataFrameView history;
    check(previous.history(7,0,previous_state,history)==Status::ok,"read explicit note coordinates for neutral source");
    const auto& matrix=std::get<NumericView>(history.payload);
    const PlaneLayout plane{0,{sizeof(float),0}};
    NumericView values{&contract,std::as_bytes(notes),{&plane,1},{notes.size(),1},{},{}};
    values.coordinates.axes[0]=matrix.coordinates.axes[1];
    std::shared_ptr<const PreparedGraph> baseline;std::unique_ptr<RuntimeContext> state;
    check(PreparedGraph::prepare_numeric(graph,profile(),baseline,state)==Status::ok,"prepare semantic/empty lineage groups");
    fill_concat_baseline(*baseline,*state,spectrum,values);
    check_concat_revision(std::move(graph),*baseline,*state);
}
void independent_rows(){
    static constexpr std::uint32_t mono=0;NumericContract input;input.channel_order={&mono,1};input.dtype=DType::Float32;input.unit=NumericUnit::Values;
    input.clock={"rows",{1,3200},3200};input.shape={Rank::Vector,{Axis::Sample,Axis::None},{ExtentBounds{16,16},ExtentBounds{}}};input.representation.alignment=alignof(float);
    ConfiguredProcessingNode source;source.id=1;source.kind="NetworkInput";source.outputs={{0,PortDirection::Output,own(input)}};
    ConfiguredProcessingNode broad;broad.id=2;broad.kind="BroadbandRow";broad.parameters=TransformProcessingParameters{{
        {"bin_hz",NumericLiteral{100.}},{"input_representation",std::string{"linear"}},{"low_low_hz",NumericLiteral{100.}},{"low_high_hz",NumericLiteral{400.}},
        {"mid_low_hz",NumericLiteral{400.}},{"mid_high_hz",NumericLiteral{1000.}},{"mid_count",NumericLiteral{std::uint64_t{2}}},
        {"high_low_hz",NumericLiteral{1000.}},{"high_high_hz",NumericLiteral{1600.}},{"high_count",NumericLiteral{std::uint64_t{2}}},{"high_reduction",std::string{"rms"}}}};
    ConfiguredProcessingNode notes;notes.id=3;notes.kind="NoteSpectrum";notes.parameters=TransformProcessingParameters{{{"bin_hz",NumericLiteral{100.}},{"input_representation",std::string{"linear"}},{"first_midi",NumericLiteral{std::uint64_t{69}}},{"last_midi",NumericLiteral{std::uint64_t{72}}}}};
    for(auto* row:{&broad,&notes}){row->inputs={{0,PortDirection::Input,own(input)}};std::array<OwnedProcessingContract,4> outputs;
        check(musical_spectrogram_output_contracts(*row,input,outputs)==Status::ok,"independent row accepts ordinary array plus numeric step, without implicit coordinates");
        const PortId count=row==&broad?1:4;for(PortId p=0;p<count;++p)row->outputs.push_back({p,PortDirection::Output,outputs[p]});check(attach_numeric_parameter_ports(*row,limits)==Status::ok,"attach explicit step to each independent row");}
    ConfiguredProcessingNode join;join.id=4;join.kind="ArrayConcat";join.parameters=TransformProcessingParameters{};join.inputs={{0,PortDirection::Input,broad.outputs[0].contract},{1,PortDirection::Input,notes.outputs[0].contract}};OwnedProcessingContract combined;
    check(array_concat_output_contract(join,combined)==Status::ok,"join two different row lengths");join.outputs={{0,PortDirection::Output,combined}};
    const auto history=[&](NodeId id,const OwnedProcessingContract& value){
        auto c=std::get<NumericContract>(value.view());auto output=c;output.history_element=c.semantic;output.semantic=Semantic::History;output.shape={Rank::Matrix,{Axis::Time,Axis::Band},{ExtentBounds{0,8},c.shape.extents[0]}};output.coordinates[1]=c.coordinates[0];output.coordinates[0]={CoordinateMode::ExplicitFloat64LE,NumericUnit::Seconds,{0,1},{1,1}};
        auto count=c;count.shape={};count.coordinates={};count.dtype=DType::UInt64;count.semantic=Semantic::Scalar;count.quantity=Quantity::FilledFrameCount;count.unit=NumericUnit::Frames;count.representation.alignment=alignof(std::uint64_t);
        ConfiguredProcessingNode h;h.id=id;h.kind="Spectrogram";h.parameters=HistoryProcessingParameters{8};h.inputs={{0,PortDirection::Input,value}};h.outputs={{0,PortDirection::Output,own(output)},{1,PortDirection::Output,own(count)}};return h;
    };
    NumericContract scalar;scalar.channel_order={&mono,1};scalar.dtype=DType::UInt32;scalar.semantic=Semantic::Scalar;scalar.unit=NumericUnit::Values;scalar.representation.alignment=alignof(std::uint32_t);
    ConfiguredProcessingNode fs;fs.id=8;fs.kind="UInt32Constant";fs.parameters=EndpointProcessingParameters{{{"value",NumericLiteral{std::uint64_t{3200}}}}};fs.outputs={{0,PortDirection::Output,own(scalar)}};
    scalar.unit=NumericUnit::Values;ConfiguredProcessingNode n;n.id=9;n.kind="UInt32Constant";n.parameters=EndpointProcessingParameters{{{"value",NumericLiteral{std::uint64_t{32}}}}};n.outputs={{0,PortDirection::Output,own(scalar)}};
    ConfiguredProcessingNode step;step.id=10;step.kind="FrequencyStep";step.parameters=TransformProcessingParameters{{{"fft_size",NumericLiteral{std::uint64_t{32}}}}};step.inputs={{0,PortDirection::Input,fs.outputs[0].contract}};OwnedProcessingContract delta;check(frequency_step_output_contract(std::get<NumericContract>(fs.outputs[0].contract.view()),delta)==Status::ok,"independent rows Float64 step");step.outputs={{0,PortDirection::Output,delta}};check(attach_numeric_parameter_ports(step,limits)==Status::ok,"measured N step port");
    NumericProcessingGraph graph;graph.graph_id="independent-rows";graph.nodes={source,broad,notes,join,history(5,combined),history(6,broad.outputs[0].contract),history(7,notes.outputs[0].contract),fs,n,step};
    graph.edges={{1,2,0,0},{1,3,0,0},{2,4,0,0},{3,4,0,1},{4,5,0,0},{2,6,0,0},{3,7,0,0},{8,10,0,0},{9,10,0,window_size_input},{10,2,0,frequency_step_input},{10,3,0,frequency_step_input}};
    std::shared_ptr<const PreparedGraph> plan;std::unique_ptr<RuntimeContext> state;
    NumericProcessingGraph evaluated,executable;const auto binding=resolve_numeric_parameter_bindings(graph,limits,evaluated,executable);
    if(!binding)std::fprintf(stderr,"split binding error=%u node=%d port=%u\n",unsigned(binding.error),binding.node,unsigned(binding.port));
    std::array<std::size_t,64> workspace{};ProcessingGraphAdmission admission;if(binding){const auto accepted=admit_processing_graph(executable,limits,PreparedGraph::numeric_capabilities(profile()),workspace,admission);if(!accepted)std::fprintf(stderr,"split admission error=%u node=%d port=%u\n",unsigned(accepted.error),accepted.node,unsigned(accepted.port));}
    const auto prepared=PreparedGraph::prepare_numeric(graph,profile(),plan,state);if(prepared!=Status::ok)std::fprintf(stderr,"split prepare status=%u\n",unsigned(prepared));check(prepared==Status::ok,"prepare independent histories and explicit numeric step wires");
    struct Observed{std::array<std::array<float,16>,3> rows{};std::array<std::size_t,3> count{};unsigned joined=0;static void receive(void* user,const NumericPortFrameView& frame){if(frame.identity.node<2 || frame.identity.node>4 || frame.identity.port || !frame.frame.stamp.available)return;auto& r=*static_cast<Observed*>(user);const auto slot=frame.identity.node-2;const auto& value=std::get<NumericView>(frame.frame.payload);r.count[slot]=value.actual[0];for(std::size_t i=0;i<value.actual[0];++i)check(load_element(value,{0,i,0},r.rows[slot][i])==Status::ok,"read independent row");if(slot==2)++r.joined;}} observed;
    std::array<float,16> values{};for(std::size_t i=0;i<16;++i)values[i]=float(i+1);const PlaneLayout plane{0,{4,0}};NumericView frame{&input,std::as_bytes(std::span(values)),{&plane,1},{16,1},{},{}};FrameStamp stamp;stamp.available=true;stamp.producer_epoch=1;const auto before=allocations;
    for(unsigned i=1;i<=2;++i){stamp.sequence=i;stamp.timestamp_ticks=i;check(plan->process_numeric_ports(1,{stamp,frame},*state,{},NumericRuntimeCallbacks{Observed::receive,nullptr,nullptr,&observed})==Status::ok,"process independent rows and joined row");}
    check(allocations==before && observed.count==std::array<std::size_t,3>{7,4,11} && observed.joined==2,"separate row dimensions, one joined frame per interval, no allocation");
    near(observed.rows[0][0],1);near(observed.rows[0][1],2);near(observed.rows[0][2],3);near(observed.rows[0][3],5);near(observed.rows[0][4],8);near(observed.rows[0][5],std::sqrt((100.+121+144)/3));near(observed.rows[0][6],std::sqrt((169.+196+225+256)/4));
    for(unsigned i=0;i<4;++i)near(observed.rows[1][i],440.*std::pow(2.,double(i)/12)/100);
    for(unsigned i=0;i<11;++i)near(observed.rows[2][i],i<7?observed.rows[0][i]:observed.rows[1][i-7]);
    const auto filled=[&](const PreparedGraph& p,RuntimeContext& c,NodeId id){DataFrameView value;std::uint64_t count=0;check(p.history(id,0,c,value,&count)==Status::ok,"read independent History");return count;};
    check(filled(*plan,*state,5)==2 && filled(*plan,*state,6)==2 && filled(*plan,*state,7)==2,"all three spectrograms accumulate independently");
    check_concat_revision(graph,*plan,*state);
    concat_empty_contributions(graph,*plan,*state,frame,std::span<const float>(observed.rows[1].data(),observed.count[1]));
    for(bool changeNotes:{true,false}){auto candidate=graph;auto& fields=std::get<TransformProcessingParameters>(candidate.nodes[changeNotes?2:1].parameters).values;
        if(changeNotes){for(auto& field:fields){if(field.key=="first_midi")field.value=NumericLiteral{std::uint64_t{70}};if(field.key=="last_midi")field.value=NumericLiteral{std::uint64_t{73}};}}
        else fields.push_back({"mid_reduction",std::string{"rms"}});
        std::shared_ptr<const PreparedGraph> next;std::unique_ptr<RuntimeContext> context;check(PreparedGraph::prepare_numeric(candidate,profile(),next,context,plan.get(),state.get())==Status::ok,"reprepare one independently configured branch");
        check(filled(*next,*context,5)==0 && filled(*next,*context,changeNotes?7:6)==0 && filled(*next,*context,changeNotes?6:7)==2,"only edited branch and combined History reset, other spectrogram retained");
    }
    struct Rejected {
        unsigned origin=0,downstream=0;
        static void receive(void* user,const NumericPortFrameView& frame){
            auto& r=*static_cast<Rejected*>(user);
            if(frame.identity.node==2 || frame.identity.node==3){
                check(!frame.frame.stamp.available && frame.status==Status::invalid_argument,"negative spectrum rejected at musical operation");++r.origin;
            }else if(frame.identity.node>=4 && frame.identity.node<=7){
                check(!frame.frame.stamp.available && frame.status==Status::unavailable,"dependent operation is unavailable without duplicated upstream error");++r.downstream;
            }
        }
    } rejected;
    values[0]=-10;stamp.sequence=3;stamp.timestamp_ticks=3;const auto rejectAllocations=allocations;
    check(plan->process_numeric_ports(1,{stamp,frame},*state,{},NumericRuntimeCallbacks{Rejected::receive,nullptr,nullptr,&rejected})==Status::invalid_argument,"negative correction result is not accepted as note amplitudes");
    check(allocations==rejectAllocations && rejected.origin==5 && rejected.downstream>0,"rejection and error localization allocate no memory");
    check(plan->node_stats(3,*state)->errors==1 && plan->node_stats(4,*state)->errors==0,"only actual rejecting node records an error");
    values[0]=1;stamp.sequence=4;stamp.timestamp_ticks=4;
    check(plan->process_numeric_ports(1,{stamp,frame},*state,{},NumericRuntimeCallbacks{Observed::receive,nullptr,nullptr,&observed})==Status::ok && observed.joined==3,"valid next frame recovers joined note display");
}
}
void* operator new(std::size_t n){++allocations;if(void* p=std::malloc(n?n:1))return p;throw std::bad_alloc();}
void operator delete(void* p) noexcept{std::free(p);}void operator delete(void* p,std::size_t) noexcept{std::free(p);}
int main(){scenario();history_partition_reprepare();independent_rows();std::puts("Independent musical rows, explicit step, joined histories, noalloc and agreement regressions passed");}
