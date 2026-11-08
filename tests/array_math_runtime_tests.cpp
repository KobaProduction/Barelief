#include <dataflow_runtime/runtime.h>
#include <dsp_graph_adapters/profile.h>
#include <domain/graph/node_factory.h>
#include <domain/numeric/array_math.h>
#include "numeric_test_builders.h"
#include <array>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <new>
namespace {
using namespace signal_contracts;using namespace domain;using namespace dataflow_runtime;
std::size_t allocations=0;
void check(bool value,const char* why){if(!value){std::fprintf(stderr,"array math: %s\n",why);std::abort();}}
NumericRuntimeProfile profile() {
    NumericRuntimeProfile p{{16,32,8,32,1048576,16777216,{8,800000,3200000,65536,4096,65536}},
        {{16777216,67108864,33554432,1048576},0,268435456},{800000,8,3200000,3200000,16777216},{true,true,true}};
    return dsp_graph_adapters::with_dsp_operations(std::move(p));
}
NumericContract contract(DType dtype,Rank rank,std::size_t rows=1,std::size_t columns=1) {
    static constexpr std::uint32_t mono=0;NumericContract c;c.dtype=dtype;c.channel_order={&mono,1};
    c.representation.alignment=dtype==DType::Float64?alignof(double):alignof(float);
    c.shape={rank,{rank==Rank::Scalar?Axis::None:Axis::Declared,rank==Rank::Matrix?Axis::Declared:Axis::None},
        {ExtentBounds{rows,rows},ExtentBounds{columns,columns}}};return c;
}
OwnedProcessingContract own(const NumericContract& c) {
    OwnedProcessingContract result;check(own_processing_contract(c,profile().admission.frame,65536,result)==Status::ok,"own contract");return result;
}
ConfiguredProcessingNode source(NodeId id,const NumericContract& c) {
    ConfiguredProcessingNode result;result.id=id;result.kind="NetworkInput";result.parameters=EndpointProcessingParameters{};
    result.outputs={{0,PortDirection::Output,own(c)}};return result;
}
NumericProcessingGraph binary(const NumericContract& left,const NumericContract& right,ArrayMathOperation operation,BinaryInput driver=BinaryInput::Left) {
    NumericProcessingGraph g;g.graph_id="array-math";g.nodes={source(1,left),source(2,right)};
    ConfiguredProcessingNode node;check(bool(make_processing_node("ArrayMultiply",3,profile().admission,node)),"create arithmetic");
    node.parameters=TransformProcessingParameters{{{"operation",std::string(array_math_name(operation))},{"frame_input",NumericLiteral{std::uint64_t(driver)}}}};
    node.inputs={{PortId(BinaryInput::Left),PortDirection::Input,own(left)},{PortId(BinaryInput::Right),PortDirection::Input,own(right)}};
    check(bool(derive_processing_node_contracts(node,profile().admission)),"derive arithmetic");g.nodes.push_back(std::move(node));
    g.edges={{1,3,0,PortId(BinaryInput::Left)},{2,3,0,PortId(BinaryInput::Right)}};return g;
}
struct Seen {
    std::array<double,8> expected{};std::array<std::size_t,2> shape{1,1};std::size_t good=0,unavailable=0;std::uint64_t source_sequence=0;
    static void output(void* context,const NumericPortFrameView& event) {
        if(event.identity.node!=3)return;auto& seen=*static_cast<Seen*>(context);
        if(event.status!=Status::ok || !event.frame.stamp.available){++seen.unavailable;return;}
        if(seen.source_sequence)check(event.frame.stamp.source_sequence==seen.source_sequence,"publication retains leading source sequence");
        const auto value=std::get<NumericView>(event.frame.payload);check(value.actual==seen.shape,"actual output geometry");
        for(std::size_t row=0;row<seen.shape[0];++row)for(std::size_t column=0;column<seen.shape[1];++column){
            const auto index=row*seen.shape[1]+column;double actual=0;
            if(value.contract->dtype==DType::Float64)check(load_element(value,{0,row,column},actual)==Status::ok,"load Double");
            else {float single=0;check(load_element(value,{0,row,column},single)==Status::ok,"load Float32");actual=single;}
            check(actual==seen.expected[index],"numeric arithmetic result");
            if(actual==0)check(!std::signbit(actual),"absolute and finite division positive zero");
        }
        ++seen.good;
    }
};
struct Execution {
    NumericProcessingGraph graph;std::shared_ptr<const PreparedGraph> plan;std::unique_ptr<RuntimeContext> state;Seen seen;
    explicit Execution(NumericProcessingGraph value):graph(std::move(value)) {
        check(PreparedGraph::prepare_numeric(graph,profile(),plan,state)==Status::ok,"prepare math graph");
    }
    template<class T> Status feed(NodeId id,std::span<const T> values,std::uint64_t sequence=1,
        std::size_t rows=SIZE_MAX,bool available=true) {
        const auto node=std::find_if(graph.nodes.begin(),graph.nodes.end(),[=](const auto& value){return value.id==id;});
        const auto data=node->outputs[0].contract.view();const auto& c=std::get<NumericContract>(data);
        const auto columns=std::size_t(c.shape.extents[1].maximum);const PlaneLayout plane{0,{columns*sizeof(T),sizeof(T)}};
        FrameStamp stamp;stamp.producer_epoch=1;stamp.sequence=stamp.source_sequence=sequence;stamp.available=available;
        const auto frame=numeric_test_builders::frame(stamp,c,available?std::as_bytes(values):std::span<const std::byte>{},std::span(&plane,1),
            {rows==SIZE_MAX?std::size_t(c.shape.extents[0].maximum):rows,columns});
        return plan->process_numeric_ports(id,0,frame,*state,{},NumericRuntimeCallbacks{Seen::output,nullptr,nullptr,&seen});
    }
};
void scalar_broadcast_and_zero() {
    Execution run(binary(contract(DType::Float32,Rank::Vector,3),contract(DType::Float32,Rank::Scalar),ArrayMathOperation::Divide));
    run.seen.expected={2,3,4};run.seen.shape={3,1};const std::array left{4.f,6.f,8.f};const std::array right{2.f},zero{0.f};
    const auto before=allocations;
    check(run.feed<float>(2,right)==Status::ok && run.feed<float>(1,left)==Status::ok && run.seen.good==1,"scalar denominator broadcast");
    check(run.feed<float>(2,zero,2)==Status::ok,"retain explicit zero denominator");
    check(run.feed<float>(1,left,2)==Status::invalid_argument && run.seen.good==1 && run.seen.unavailable>0,"zero denominator rejects without stale publication");
    check(allocations==before,"divide hot path allocates no memory");
}
void row_broadcast_subtract() {
    Execution run(binary(contract(DType::Float32,Rank::Matrix,2,3),contract(DType::Float32,Rank::Vector,3),ArrayMathOperation::Subtract));
    run.seen.expected={1,2,3,4,5,6};run.seen.shape={2,3};const std::array left{2.f,4.f,6.f,5.f,7.f,9.f};const std::array right{1.f,2.f,3.f};
    check(run.feed<float>(2,right)==Status::ok && run.feed<float>(1,left)==Status::ok && run.seen.good==1,"row vector subtract broadcast");
}
void leading_operand() {
    auto g=binary(contract(DType::Float32,Rank::Scalar),contract(DType::Float32,Rank::Scalar),ArrayMathOperation::Add,BinaryInput::Right);
    check(bool(derive_processing_node_contracts(g.nodes.back(),profile().admission)),"select right leading operand");
    Execution run(std::move(g));run.seen.expected={5};const std::array first{2.f},second{3.f},next_left{7.f},next_right{11.f};
    check(run.feed<float>(1,first)==Status::ok && run.feed<float>(2,second)==Status::ok && run.seen.good==1,"right operand drives first result");
    check(run.feed<float>(1,next_left,2)==Status::ok && run.seen.good==1,"held update does not republish stale leading frame");
    run.seen.expected={18};check(run.feed<float>(2,next_right,2)==Status::ok && run.seen.good==2,"updated left combined with current right");
}
void late_vector_trigger() {
    auto g=binary(contract(DType::Float32,Rank::Matrix,2,3),contract(DType::Float32,Rank::Vector,3),ArrayMathOperation::Multiply);
    std::get<TransformProcessingParameters>(g.nodes.back().parameters).values.push_back({"trigger_input",NumericLiteral{std::uint64_t(BinaryInput::Right)}});
    check(bool(derive_processing_node_contracts(g.nodes.back(),profile().admission)),"separate late vector trigger");
    Execution run(std::move(g));run.seen.expected={2,4,6,8,10,12};run.seen.shape={2,3};run.seen.source_sequence=1;
    const std::array first{1.f,2.f,3.f,4.f,5.f,6.f},next{2.f,3.f,4.f,5.f,6.f,7.f};const std::array weights{2.f,2.f,2.f},late{3.f,3.f,3.f};
    const auto before=allocations;
    check(run.feed<float>(1,first)==Status::ok && run.seen.good==0,"matrix waits for late first weights");
    check(run.feed<float>(2,weights,50)==Status::ok && run.seen.good==1,"weights trigger retains matrix shape and provenance");
    check(run.feed<float>(1,next,2)==Status::ok && run.seen.good==1,"new matrix cannot use stale previous weights");
    run.seen.expected={6,9,12,15,18,21};run.seen.source_sequence=2;
    check(run.feed<float>(2,late,51)==Status::ok && run.seen.good==2,"late replacement weights publish current matrix once");
    check(allocations==before,"separate trigger uses bounded retained factors");
}
void unwired_trigger_zero() {
    auto g=binary(contract(DType::Float32,Rank::Vector,3),contract(DType::Float32,Rank::Scalar),ArrayMathOperation::Multiply);
    g.edges.pop_back();std::get<TransformProcessingParameters>(g.nodes.back().parameters).values.push_back({"trigger_input",NumericLiteral{std::uint64_t(BinaryInput::Right)}});
    Execution run(std::move(g));run.seen.shape={3,1};const std::array values{1.f,2.f,3.f};
    check(run.feed<float>(1,values)==Status::ok && run.seen.good==1,"unwired trigger retains typed zero delivery on leading clock");
}
void scalar_numerator_and_port_order() {
    auto g=binary(contract(DType::Float32,Rank::Scalar),contract(DType::Float32,Rank::Vector,3),ArrayMathOperation::Divide,BinaryInput::Right);
    auto& node=g.nodes.back();
    std::swap(node.inputs[0],node.inputs[1]);check(bool(derive_processing_node_contracts(node,profile().admission)),"right shape driver with permuted stable ports");
    Execution run(std::move(g));run.seen.expected={12,6,4};run.seen.shape={3,1};const std::array left{24.f};const std::array right{2.f,4.f,6.f};
    check(run.feed<float>(1,left)==Status::ok && run.feed<float>(2,right)==Status::ok && run.seen.good==1,"driver and storage order do not swap numerator");
}
void mixed_division() {
    Execution run(binary(contract(DType::Float64,Rank::Vector,2),contract(DType::Float32,Rank::Scalar),ArrayMathOperation::Divide));
    run.seen.expected={1e100,-1e100};run.seen.shape={2,1};const std::array left{2e100,-2e100};const std::array right{2.f};
    check(run.feed<float>(2,right)==Status::ok && run.feed<double>(1,left)==Status::ok && run.seen.good==1,"Double result with mixed denominator");
}
NumericProcessingGraph absolute_graph(DType dtype,Rank rank,std::size_t rows,std::size_t columns) {
    const auto input=contract(dtype,rank,rows,columns);NumericProcessingGraph g;g.graph_id="absolute";g.nodes.push_back(source(1,input));
    ConfiguredProcessingNode operation;check(bool(make_processing_node("AbsoluteValue",3,profile().admission,operation)),"create absolute");
    check(bool(configure_processing_input(operation,input,profile().admission)),"configure scalar/matrix absolute");
    g.nodes.push_back(std::move(operation));g.edges={{1,3,0,0}};return g;
}
void absolute_cases() {
    Execution matrix(absolute_graph(DType::Float64,Rank::Matrix,2,2));matrix.seen.expected={0,2,1e100,3};matrix.seen.shape={2,2};
    const std::array values{-0.,-2.,-1e100,3.};const auto before=allocations;
    check(matrix.feed<double>(1,values)==Status::ok && matrix.seen.good==1,"Double absolute retains magnitude and geometry");
    const std::array invalid{0.,std::numeric_limits<double>::quiet_NaN(),1.,2.};
    check(matrix.feed<double>(1,invalid,2)!=Status::ok && matrix.seen.good==1,"nonfinite absolute does not publish");
    check(allocations==before,"absolute hot path allocates no memory");
    Execution scalar(absolute_graph(DType::Float32,Rank::Scalar,1,1));scalar.seen.expected={7};const std::array value{-7.f};
    check(scalar.feed<float>(1,value)==Status::ok && scalar.seen.good==1,"Float32 scalar absolute");
}
NumericContract variable(DType dtype,Rank rank=Rank::Vector,std::size_t rows=4,std::size_t columns=1) {
    auto result=contract(dtype,rank,rows,columns);result.shape.extents[0].minimum=0;return result;
}
std::array<double,8> arithmetic_expected(ArrayMathOperation operation) {
    if(operation==ArrayMathOperation::Multiply)return {16,36,64,100};
    if(operation==ArrayMathOperation::Add)return {10,15,20,25};
    if(operation==ArrayMathOperation::Subtract)return {6,9,12,15};
    return {4,4,4,4};
}
template<class T> void variable_arithmetic(ArrayMathOperation operation) {
    const auto c=variable(dtype_of<T>());Execution run(binary(c,c,operation));
    const std::array<T,4> left{8,12,16,20},right{2,3,4,5};
    run.seen.expected=arithmetic_expected(operation);run.seen.shape={2,1};const auto before=allocations;
    check(run.feed<T>(2,right,1,2)==Status::ok && run.feed<T>(1,left,1,2)==Status::ok && run.seen.good==1,"equal short vector prefixes");
    run.seen.shape={0,1};
    check(run.feed<T>(2,right,2,0)==Status::ok && run.feed<T>(1,left,2,0)==Status::ok && run.seen.good==2,"two empty vectors publish empty result without repeating or dividing");
    run.seen.shape={4,1};
    check(run.feed<T>(2,right,3,4)==Status::ok && run.feed<T>(1,left,3,4)==Status::ok && run.seen.good==3,"full vector after short and empty prefixes has no stale tail");
    check(allocations==before,"variable vector arithmetic allocates no memory");
}
void mismatched_vector_prefixes() {
    const auto c=variable(DType::Float32);Execution run(binary(c,c,ArrayMathOperation::Multiply));
    const std::array left{8.f,12.f,16.f,20.f},right{2.f,3.f,4.f,5.f};run.seen.expected={16,36};run.seen.shape={2,1};
    const auto before=allocations;
    check(run.feed<float>(2,right,1,3)==Status::ok,"retain three-element vector");
    check(run.feed<float>(1,left,1,2)==Status::incompatible_contract && run.seen.good==0 && run.seen.unavailable>0,"shorter peer does not crop or repeat");
    check(run.feed<float>(2,right,2,2)==Status::ok && run.seen.good==0,"shape correction cannot publish rejected leading frame");
    check(run.feed<float>(1,left,2,2)==Status::ok && run.seen.good==1,"new matching leading frame publishes");
    check(run.feed<float>(2,right,3,0)==Status::ok,"retain available empty factor");
    check(run.feed<float>(1,left,3,2)==Status::incompatible_contract && run.seen.good==1,"empty factor cannot broadcast into nonempty vector");
    run.seen.shape={0,1};
    check(run.feed<float>(1,left,4,0)==Status::ok && run.seen.good==2,"matching empty frame after rejection publishes no stale values");
    check(allocations==before,"shape mismatch paths allocate no memory");
}
void fixed_vector_requires_equal_actual() {
    Execution run(binary(variable(DType::Float32),contract(DType::Float32,Rank::Vector,4),ArrayMathOperation::Multiply));
    const std::array left{8.f,12.f,16.f,20.f},right{2.f,3.f,4.f,5.f};
    run.seen.expected={16,36,64,100};run.seen.shape={4,1};
    check(run.feed<float>(2,right)==Status::ok,"fixed factor retained");
    check(run.feed<float>(1,left,1,2)==Status::incompatible_contract && run.seen.good==0,"fixed mask is never implicitly cropped to shorter driver");
    check(run.feed<float>(1,left,2,4)==Status::ok && run.seen.good==1,"fixed and variable vectors can share equal actual length");
}
void variable_matrix_weights() {
    Execution run(binary(variable(DType::Float64,Rank::Matrix,3,2),variable(DType::Float32,Rank::Vector,2),ArrayMathOperation::Multiply));
    const std::array left{1.,2.,3.,4.,5.,6.};const std::array right{2.f,3.f};
    run.seen.expected={2,6,6,12,10,18};run.seen.shape={2,2};const auto before=allocations;
    check(run.feed<float>(2,right,1,2)==Status::ok && run.feed<double>(1,left,1,2)==Status::ok && run.seen.good==1,"variable vector weights exactly match fixed matrix columns");
    run.seen.shape={0,2};
    check(run.feed<double>(1,left,2,0)==Status::ok && run.seen.good==2,"empty matrix retains declared columns");
    check(run.feed<float>(2,right,2,1)==Status::incompatible_contract && run.seen.good==2,"one-element vector is not a scalar column broadcast");
    check(run.feed<float>(2,right,3,0)==Status::incompatible_contract && run.seen.good==2,"empty vector cannot supply matrix columns");
    check(run.feed<double>(1,left,3,3)==Status::ok && run.seen.good==2,"matrix waits for a fresh compatible weight vector");
    run.seen.shape={3,2};
    check(run.feed<float>(2,right,4,2)==Status::ok && run.seen.good==3,"repaired weights publish current matrix");
    check(allocations==before,"variable row weights allocate no memory");
}
void variable_scalar_and_late_trigger() {
    auto g=binary(variable(DType::Float64),contract(DType::Float32,Rank::Scalar),ArrayMathOperation::Divide);
    std::get<TransformProcessingParameters>(g.nodes.back().parameters).values.push_back({"trigger_input",NumericLiteral{std::uint64_t(BinaryInput::Right)}});
    Execution run(std::move(g));const std::array left{8.,12.,16.,20.};const std::array right{2.f};
    run.seen.expected={4,6,8,10};run.seen.shape={2,1};run.seen.source_sequence=1;const auto before=allocations;
    check(run.feed<double>(1,left,1,2)==Status::ok && run.seen.good==0,"short vector waits for scalar trigger");
    check(run.feed<float>(2,right,50)==Status::ok && run.seen.good==1,"late scalar retains variable leading geometry and provenance");
    run.seen.shape={0,1};run.seen.source_sequence=2;
    check(run.feed<double>(1,left,2,0)==Status::ok && run.seen.good==1,"empty driver also respects explicit trigger");
    check(run.feed<float>(2,right,51)==Status::ok && run.seen.good==2,"scalar trigger publishes empty vector");
    run.seen.shape={4,1};run.seen.source_sequence=3;
    check(run.feed<double>(1,left,3,4)==Status::ok && run.feed<float>(2,right,52)==Status::ok && run.seen.good==3,"full frame follows empty without stale leading shape");
    check(allocations==before,"late scalar variable vectors allocate no memory");
}
NumericProcessingGraph right_trigger_graph(ArrayMathOperation operation) {
    const auto c=variable(DType::Float32);auto graph=binary(c,c,operation);
    std::get<TransformProcessingParameters>(graph.nodes.back().parameters).values.push_back({"trigger_input",NumericLiteral{std::uint64_t(BinaryInput::Right)}});
    return graph;
}
void early_trigger_two_batches() {
    Execution run(right_trigger_graph(ArrayMathOperation::Subtract));
    const std::array left{8.f,12.f,16.f,20.f},right{2.f,3.f,4.f,5.f},next{1.f,2.f,3.f,4.f};
    run.seen.expected={6,9};run.seen.shape={2,1};run.seen.source_sequence=1;const auto before=allocations;
    check(run.feed<float>(2,right,1,2)==Status::ok && run.seen.good==0,"right trigger arms before leading input");
    check(run.feed<float>(1,left,1,2)==Status::ok && run.seen.good==1,"early trigger publishes first leading frame");
    check(run.feed<float>(2,next,2,2)==Status::ok && run.seen.good==1,"next early trigger cannot reuse consumed leading frame");
    run.seen.expected={7,10};run.seen.source_sequence=2;
    check(run.feed<float>(1,left,2,2)==Status::ok && run.seen.good==2,"right-before-left order works for two consecutive batches");
    run.seen.shape={0,1};run.seen.source_sequence=3;
    check(run.feed<float>(1,left,3,0)==Status::ok && run.seen.good==2,"consumed trigger cannot publish new empty driver");
    check(run.feed<float>(2,right,3,0)==Status::ok && run.seen.good==3,"late trigger publishes empty leading frame");
    check(run.feed<float>(2,right,4,1)==Status::ok,"arm incompatible right prefix");
    check(run.feed<float>(1,left,4,2)==Status::incompatible_contract && run.seen.good==3,"shape rejection clears pending early trigger");
    run.seen.shape={2,1};run.seen.expected={6,9};run.seen.source_sequence=5;
    check(run.feed<float>(2,right,5,2)==Status::ok && run.seen.good==3,"corrected trigger cannot publish rejected frame");
    check(run.feed<float>(1,left,5,2)==Status::ok && run.seen.good==4,"new frame consumes fresh corrected trigger");
    check(allocations==before,"early trigger latch allocates no memory");
}
void early_trigger_unavailable_reset() {
    Execution run(right_trigger_graph(ArrayMathOperation::Multiply));
    const std::array left{8.f,12.f,16.f,20.f},right{2.f,3.f,4.f,5.f};run.seen.expected={16,36};run.seen.shape={2,1};
    const auto before=allocations;
    check(run.feed<float>(2,right,1,2)==Status::ok && run.seen.good==0,"arm trigger before unavailable driver");
    check(run.feed<float>(1,left,1,2,false)==Status::ok && run.seen.unavailable>0,"unavailable driver clears armed invocation");
    check(run.feed<float>(1,left,2,2)==Status::ok && run.seen.good==0,"fresh driver cannot consume pre-reset trigger");
    check(run.feed<float>(2,right,2,2)==Status::ok && run.seen.good==1,"fresh trigger resumes after unavailable driver");
    check(allocations==before,"trigger reset has no hot allocation");
}
void early_trigger_waits_for_remaining_operand() {
    auto graph=right_trigger_graph(ArrayMathOperation::Multiply);
    graph.nodes[2].inputs.push_back({2,PortDirection::Input,own(contract(DType::Float32,Rank::Scalar))});
    check(bool(derive_processing_node_contracts(graph.nodes[2],profile().admission)),"derive three-factor trigger join");
    graph.nodes.push_back(source(4,contract(DType::Float32,Rank::Scalar)));graph.edges.push_back({4,3,0,2});
    Execution run(std::move(graph));const std::array left{8.f,12.f,16.f,20.f},right{2.f,3.f,4.f,5.f};const std::array scalar{2.f};
    run.seen.expected={32,72};run.seen.shape={2,1};const auto before=allocations;
    check(run.feed<float>(2,right,1,2)==Status::ok && run.feed<float>(1,left,1,2)==Status::ok && run.seen.good==0,"early trigger waits for all declared operands");
    check(run.feed<float>(4,scalar)==Status::ok && run.seen.good==1,"remaining held operand completes armed invocation");
    check(run.feed<float>(4,scalar,2)==Status::ok && run.seen.good==1,"consumed trigger cannot republish on held update");
    check(allocations==before,"trigger join uses bounded retained factors");
}
void unavailable_resets_variable_lane() {
    const auto c=variable(DType::Float32);Execution run(binary(c,c,ArrayMathOperation::Multiply));
    const std::array left{8.f,12.f,16.f,20.f},right{2.f,3.f,4.f,5.f};run.seen.expected={16,36};run.seen.shape={2,1};
    check(run.feed<float>(2,right,1,2)==Status::ok && run.feed<float>(1,left,1,2)==Status::ok && run.seen.good==1,"initial variable factors ready");
    const auto before=allocations;
    check(run.feed<float>(2,right,2,2,false)==Status::ok && run.seen.unavailable>0,"unavailable factor resets readiness");
    check(run.feed<float>(1,left,2,2)==Status::ok && run.seen.good==1,"new leading frame cannot use factor from before unavailable interval");
    check(run.feed<float>(2,right,3,2)==Status::ok && run.seen.good==2,"fresh compatible factor resumes processing");
    check(allocations==before,"variable factor reset allocates no memory");
}
template<class T> void disabled_variable_silence() {
    const auto c=variable(dtype_of<T>());auto g=binary(c,c,ArrayMathOperation::Multiply);g.nodes.back().enabled=false;
    check(bool(derive_processing_node_contracts(g.nodes.back(),profile().admission)),"derive disabled variable output");
    Execution run(std::move(g));const std::array<T,4> left{8,12,16,20},right{2,3,4,5};run.seen.shape={2,1};
    const auto before=allocations;
    check(run.feed<T>(1,left,1,2)==Status::ok && run.seen.good==1,"disabled arithmetic publishes silence on actual leading prefix");
    check(run.feed<T>(2,right,1,3)==Status::ok && run.seen.good==1,"disabled auxiliary geometry cannot publish or invalidate output");
    run.seen.shape={0,1};
    check(run.feed<T>(1,left,2,0)==Status::ok && run.seen.good==2,"disabled empty driver publishes empty silence");
    run.seen.shape={4,1};
    check(run.feed<T>(1,left,3,4)==Status::ok && run.seen.good==3,"disabled full driver has no stale passthrough values");
    check(allocations==before,"disabled variable arithmetic allocates no memory");
}
void unwired_variable_silence() {
    const auto c=variable(DType::Float32);auto g=binary(c,c,ArrayMathOperation::Multiply);g.edges.pop_back();
    Execution run(std::move(g));const std::array left{8.f,12.f,16.f,20.f};run.seen.shape={2,1};
    check(run.feed<float>(1,left,1,2)==Status::ok && run.seen.good==1,"unwired vector supplies typed zero at leading actual length");
    run.seen.shape={0,1};check(run.feed<float>(1,left,2,0)==Status::ok && run.seen.good==2,"unwired factor permits empty leading frame");
}
void variable_contract_geometry() {
    auto c=variable(DType::Float32);c.unit=NumericUnit::Values;
    auto scalar=contract(DType::Float64,Rank::Scalar);scalar.unit=NumericUnit::Hertz;
    auto g=binary(c,scalar,ArrayMathOperation::Multiply);const auto data=g.nodes.back().outputs[0].contract.view();
    const auto& output=std::get<NumericContract>(data);
    check(output.shape==c.shape && output.dtype==DType::Float64 && output.unit==NumericUnit::Hertz,"meaningful scalar cannot replace leading variable geometry");
    auto invalid=g.nodes.back();invalid.inputs[1].contract=own(variable(DType::Float32,Rank::Vector,3));
    check(!derive_processing_node_contracts(invalid,profile().admission),"different vector capacities remain incompatible");
    auto matrix=variable(DType::Float32,Rank::Matrix,3,2);matrix.shape.extents[1].minimum=0;
    invalid.inputs[0].contract=own(matrix);invalid.inputs[1].contract=own(contract(DType::Float32,Rank::Scalar));
    check(!derive_processing_node_contracts(invalid,profile().admission),"variable matrix columns remain unsupported");
}
void variable_cases() {
    for(const auto operation:{ArrayMathOperation::Multiply,ArrayMathOperation::Add,ArrayMathOperation::Subtract,ArrayMathOperation::Divide}){
        variable_arithmetic<float>(operation);variable_arithmetic<double>(operation);
    }
    mismatched_vector_prefixes();fixed_vector_requires_equal_actual();variable_matrix_weights();variable_scalar_and_late_trigger();
    early_trigger_two_batches();early_trigger_unavailable_reset();early_trigger_waits_for_remaining_operand();
    unavailable_resets_variable_lane();disabled_variable_silence<float>();disabled_variable_silence<double>();
    unwired_variable_silence();variable_contract_geometry();
}
void invalid_policy() {
    auto g=binary(contract(DType::Float32,Rank::Scalar),contract(DType::Float32,Rank::Scalar),ArrayMathOperation::Divide);
    auto& node=g.nodes.back();auto& fields=std::get<TransformProcessingParameters>(node.parameters).values;
    fields.push_back({"zero_division",std::string{"infinity"}});
    check(!derive_processing_node_contracts(node,profile().admission),"unsupported zero policy rejects atomically");
    fields.back()={"zero_division",std::string{"reject"}};fields[0].value=std::string{"unknown"};
    check(!derive_processing_node_contracts(node,profile().admission),"unknown operation selector rejects");
    fields[0].value=std::string{"divide"};fields.push_back({"trigger_input",NumericLiteral{std::uint64_t{7}}});
    check(!derive_processing_node_contracts(node,profile().admission),"undeclared trigger port rejects");
}
}
void* operator new(std::size_t count){++allocations;if(auto* memory=std::malloc(count?count:1))return memory;throw std::bad_alloc();}
void operator delete(void* memory) noexcept{std::free(memory);}
void operator delete(void* memory,std::size_t) noexcept{std::free(memory);}
int main(){scalar_broadcast_and_zero();row_broadcast_subtract();leading_operand();late_vector_trigger();unwired_trigger_zero();scalar_numerator_and_port_order();mixed_division();absolute_cases();invalid_policy();variable_cases();std::puts("array math PASS");}
