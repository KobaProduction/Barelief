#include "numeric_operation_lifecycle_fixture.h"
#include <domain/numeric/array_processing_ports.h>
#include <limits>
#include <dsp_kernels/axis_math.h>

namespace {
using namespace operation_lifecycle;
ProcessingParameter size_parameter(std::string key,std::size_t value) {
    return {std::move(key),NumericLiteral{std::uint64_t(value)}};
}
OwnedProcessingContract matrix_contract(DType dtype,std::size_t rows=4,std::size_t columns=3) {
    static constexpr std::uint32_t mono=0;NumericContract value;
    value.channel_order={&mono,1};value.dtype=dtype;value.unit=NumericUnit::Values;
    value.semantic=Semantic::Numeric;value.representation.alignment=dtype_descriptor(dtype)->alignment;
    value.shape={Rank::Matrix,{Axis::Declared,Axis::Component},{{{0,rows},{columns,columns}}}};
    OwnedProcessingContract result;
    require(own_processing_contract(value,profile().admission.frame,65536,result)==Status::ok,"own matrix prefix contract");
    return result;
}
ConfiguredProcessingNode node_for(std::string_view kind,const OwnedProcessingContract& input,
    TransformProcessingParameters parameters={},const OwnedProcessingContract* second=nullptr) {
    ConfiguredProcessingNode node;node.id=10;node.kind=kind;node.parameters=std::move(parameters);
    node.inputs={{0,PortDirection::Input,input}};
    if(second)node.inputs.push_back({1,PortDirection::Input,*second});
    std::vector<OwnedProcessingContract> outputs;
    require(common_operation_output_contracts(node,std::get<NumericContract>(input.view()),outputs)==Status::ok,"derive prefix operation outputs");
    for(std::size_t i=0;i<outputs.size();++i)node.outputs.push_back({PortId(i),PortDirection::Output,std::move(outputs[i])});
    return node;
}
double number(const NumericBufferView& value,ElementIndex index) {
    if(value.contract->dtype==DType::UInt64){
        std::uint64_t result=0;require(load_element(value,index,result)==Status::ok,"read index output");return double(result);
    }
    if(value.contract->dtype==DType::Float64){
        double result=0;require(load_element(value,index,result)==Status::ok,"read Double output");return result;
    }
    float result=0;require(load_element(value,index,result)==Status::ok,"read Float32 output");return result;
}
struct GeometryOperation:Operation {
    explicit GeometryOperation(ConfiguredProcessingNode node):Operation(std::move(node)) {
        for(std::size_t i=0;i<this->node.outputs.size();++i)layout_output(i);
    }
    void layout_output(std::size_t slot) {
        const auto& c=output_contracts[slot];const auto width=std::size_t(dtype_descriptor(c.dtype)->bytes);
        const auto columns=std::size_t(c.shape.extents[1].maximum);
        output_planes[slot][0]={0,{columns*width,c.shape.rank==Rank::Matrix?width:0}};
        output_storage[slot].value.actual={std::size_t(c.shape.extents[0].maximum),columns};
        buffers[slot].bytes.fill(std::byte{0x7f});
    }
    template<class T,std::size_t Extent> NumericOperationInput prefix(std::size_t slot,std::span<const T,Extent> storage,std::size_t rows) {
        const auto& c=input_contracts[slot];const auto columns=std::size_t(c.shape.extents[1].maximum);
        input_planes[slot][0]={0,{columns*sizeof(T),c.shape.rank==Rank::Matrix?sizeof(T):0}};
        auto result=input(slot,std::span<const T>(storage));result.value.actual={rows,columns};result.contiguous_float32={};return result;
    }
    void zero(std::size_t slot,std::size_t rows,std::size_t columns=1) const {
        const auto& value=output_storage[slot];require(value.status==Status::ok,"disabled output published");
        require(value.value.actual==std::array<std::size_t,2>{rows,columns},"disabled output keeps actual geometry");
        for(std::size_t row=0;row<rows;++row)for(std::size_t column=0;column<columns;++column)
            require(number(value.value.view(),{0,row,column})==0,"disabled output contains typed silence");
    }
    void disabled(const NumericOperationInput& input) {
        require(processor->process(std::span(&input,1),outputs(),{.enabled=false})==Status::ok,"process disabled prefix");
    }
};
template<class T> void disabled_maps() {
    const std::array<T,12> storage{1,2,3,4,5,6,7,8,9,10,11,12};
    for(const auto type:{std::size_t{0},std::size_t{1},std::size_t{2}}){
        const TransformProcessingParameters parameters{{size_parameter("output_dtype",type)}};
        GeometryOperation vector(node_for("NumericMap",contract(dtype_of<T>(),5,true),parameters));
        GeometryOperation matrix(node_for("NumericMap",matrix_contract(dtype_of<T>()),parameters));
        for(const auto rows:{std::size_t{1},std::size_t{0},std::size_t{4}}){
            vector.disabled(vector.prefix(0,std::span(storage).first(5),rows));vector.zero(0,rows);
            matrix.disabled(matrix.prefix(0,std::span(storage),rows));matrix.zero(0,rows,3);
        }
    }
}
template<class T> void disabled_matrix_reductions() {
    const std::array<T,12> storage{1,2,3,4,5,6,7,8,9,10,11,12};
    for(const auto axis:{std::size_t{0},std::size_t{1},std::size_t{2}}){
        const auto input=matrix_contract(dtype_of<T>());
        const TransformProcessingParameters parameters{{size_parameter("axis",axis)}};
        GeometryOperation maximum(node_for("AxisMaximum",input,parameters));
        GeometryOperation sum(node_for("AxisSum",input,parameters));
        auto cumulative_parameters=parameters;cumulative_parameters.values.push_back({"cumulative",true});
        GeometryOperation cumulative(node_for("AxisSum",input,cumulative_parameters));
        for(const auto rows:{std::size_t{2},std::size_t{0},std::size_t{4}}){
            const auto reduced=axis?rows:3;
            maximum.disabled(maximum.prefix(0,std::span(storage),rows));
            maximum.zero(0,reduced);maximum.zero(1,reduced);maximum.zero(2,rows,3);
            sum.disabled(sum.prefix(0,std::span(storage),rows));sum.zero(0,reduced);
            cumulative.disabled(cumulative.prefix(0,std::span(storage),rows));cumulative.zero(0,rows,3);
        }
    }
}
template<class T> void disabled_vector_reductions() {
    const std::array<T,5> storage{1,2,3,4,5};const auto input=contract(dtype_of<T>(),5,true);
    GeometryOperation maximum(node_for("AxisMaximum",input));
    GeometryOperation sum(node_for("AxisSum",input));
    GeometryOperation cumulative(node_for("AxisSum",input,TransformProcessingParameters{{{"cumulative",true}}}));
    for(const auto rows:{std::size_t{2},std::size_t{0},std::size_t{5}}){
        maximum.disabled(maximum.prefix(0,std::span(storage),rows));
        maximum.zero(0,1);maximum.zero(1,1);maximum.zero(2,rows);
        sum.disabled(sum.prefix(0,std::span(storage),rows));sum.zero(0,1);
        cumulative.disabled(cumulative.prefix(0,std::span(storage),rows));cumulative.zero(0,rows);
    }
}
template<class T> void disabled_comb() {
    GeometryOperation comb(node_for("CombFilterBank",contract(dtype_of<T>(),5,true),
        TransformProcessingParameters{{{"delays",NumericArray{{1,2,3}}},{"alpha",NumericArray{{.5f}}}}}));
    const std::array<T,5> storage{1,2,3,4,5};
    for(const auto rows:{std::size_t{1},std::size_t{0},std::size_t{5}}){
        comb.disabled(comb.prefix(0,std::span(storage),rows));comb.zero(0,rows,3);comb.zero(1,3);
    }
}
template<class T> void gather_prefix() {
    const auto indices=contract(DType::UInt64,4,true);
    GeometryOperation gather(node_for("ArrayGather",contract(dtype_of<T>(),5),{},&indices));
    const std::array<T,5> table{10,20,30,40,50};const std::array<std::uint64_t,4> selected{4,1,0,0};
    std::array inputs{gather.prefix(0,std::span(table),5),gather.prefix(1,std::span(selected),2)};
    require(gather.processor->process(inputs,gather.outputs(),{})==Status::ok,"gather actual index prefix");
    const auto& output=gather.output_storage[0];
    require(output.value.actual==std::array<std::size_t,2>{2,1},"gather publishes actual index count");
    require(number(output.value.view(),{0,0,0})==50 && number(output.value.view(),{0,1,0})==20,"gather selects explicit table values");
    require(gather.processor->process(inputs,gather.outputs(),{.enabled=false})==Status::ok,"disabled gather uses index driver");
    gather.zero(0,2);inputs[1].value.actual[0]=0;
    require(gather.processor->process(inputs,gather.outputs(),{.enabled=false})==Status::ok,"disabled gather empty indices");gather.zero(0,0);
}
void scalar_map_offset_precision() {
    const auto base=contract(DType::Float64);auto declared=std::get<NumericContract>(base.view());
    declared.clock={"scalar-map",{1,44100},44100};OwnedProcessingContract clocked;
    require(own_processing_contract(declared,profile().admission.frame,65536,clocked)==Status::ok,"own clocked scalar map input");
    auto node=node_for("NumericMap",clocked,
        TransformProcessingParameters{{{"offset",NumericLiteral{.0001}},size_parameter("output_dtype",1)}});
    const auto output_contract=node.outputs[0].contract.view();const auto& full=std::get<NumericContract>(output_contract);
    require(full.clock.domain==declared.clock.domain && full.clock.tick_seconds==declared.clock.tick_seconds && full.clock.sample_rate==44100,
        "scalar map preserves its input clock before runtime metadata validation");
    GeometryOperation operation(std::move(node));
    const std::array<double,1> zero{0};auto input=operation.prefix(0,std::span(zero),1);
    require(operation.processor->process(std::span(&input,1),operation.outputs(),{})==Status::ok,"map adds offset on the first scalar frame");
    const auto& output=operation.output_storage[0];
    require(output.value.actual==std::array<std::size_t,2>{1,1},"scalar map preserves scalar geometry");
    require(number(output.value.view(),{0,0,0})==.0001 && .0001!=double(.0001f),"offset retains the explicit Double literal");
    operation.disabled(input);operation.zero(0,1);
}
void map_rounds_before_offset() {
    GeometryOperation operation(node_for("NumericMap",contract(DType::Float64,2,true),
        TransformProcessingParameters{{scalar("scale",0x1.000002p0f),{"offset",NumericLiteral{-0x1.000002p0}},size_parameter("output_dtype",1)}}));
    const std::array<double,2> values{0x1.0000000000001p0,3};auto input=operation.prefix(0,std::span(values),1);
    require(operation.processor->process(std::span(&input,1),operation.outputs(),{})==Status::ok,"map affine prefix");
    require(number(operation.output_storage[0].value.view(),{0,0,0})==0x1p-52,"scale then offset uses two rounded operations, not FMA");
    input.value.actual[0]=0;
    require(operation.processor->process(std::span(&input,1),operation.outputs(),{})==Status::ok && operation.output_storage[0].value.actual[0]==0,"affine map publishes an empty prefix");
    auto node=operation.node;
    std::get<TransformProcessingParameters>(node.parameters).values[1].value=NumericLiteral{std::numeric_limits<double>::infinity()};
    const auto p=profile();
    require(!p.operation_factory->assess(numeric_operation_node_view(resolved(node)),numeric_operation_profile(p)).supported,"map rejects nonfinite offsets before preparation");
}
void map_retains_explicit_coordinates() {
    const auto base=matrix_contract(DType::Float64);auto declared=std::get<NumericContract>(base.view());
    declared.clock={"coordinate-map",{1,120},0};
    declared.coordinates[0]={CoordinateMode::ExplicitFloat64LE,NumericUnit::Seconds,{},{}};
    declared.coordinates[1]={CoordinateMode::Regular,NumericUnit::Hertz,{0,1},{100,1}};
    OwnedProcessingContract input;
    require(own_processing_contract(declared,profile().admission.frame,65536,input)==Status::ok,"own explicit-coordinate map input");
    const auto node=node_for("NumericMap",input,TransformProcessingParameters{{size_parameter("output_dtype",1)}});
    const auto view=node.outputs[0].contract.view();const auto& output=std::get<NumericContract>(view);
    require(output.shape==declared.shape && output.coordinates==declared.coordinates,"pointwise map preserves shape and both coordinate declarations");
    const auto p=profile();std::shared_ptr<const NumericOperationDescriptor> descriptor;
    require(p.operation_factory->describe(numeric_operation_node_view(resolved(node)),descriptor)==Status::ok,"describe coordinate-preserving map");
    const std::vector<NumericOperationCoordinateRoute> expected{{0,0,0,0},{0,0,1,1}};
    require(descriptor->policy.coordinate_routes==expected,"pointwise map routes actual explicit coordinates from its input");
}
TransformProcessingParameters pairwise_parameters(std::size_t axis=2) {
    return {{size_parameter("axis",axis),size_parameter("accumulation",std::size_t(dsp_kernels::AxisSumAccumulation::Pairwise))}};
}
template<class T> void vector_sum_accumulation() {
    const T large=std::is_same_v<T,float>?T(1e8):T(1e16);
    const T expected=std::is_same_v<T,float>?T(100000008.):T(10000000000000014.);
    std::array<T,16> values;values.fill(T{1});values[0]=large;
    GeometryOperation sequential(node_for("AxisSum",contract(dtype_of<T>(),16,true)));
    GeometryOperation pairwise(node_for("AxisSum",contract(dtype_of<T>(),16,true),pairwise_parameters()));
    auto plain=sequential.prefix(0,std::span<const T>(values),16),tree=pairwise.prefix(0,std::span<const T>(values),16);
    require(sequential.processor->process(std::span(&plain,1),sequential.outputs(),{})==Status::ok,"default sequential vector sum");
    require(pairwise.processor->process(std::span(&tree,1),pairwise.outputs(),{})==Status::ok,"explicit pairwise vector sum");
    require(number(sequential.output_storage[0].value.view(),{})==double(large),"default accumulation keeps sequential rounding");
    require(number(pairwise.output_storage[0].value.view(),{})==double(expected),"explicit accumulation matches native typed sum");
    tree.value.actual[0]=0;
    require(pairwise.processor->process(std::span(&tree,1),pairwise.outputs(),{})==Status::ok && number(pairwise.output_storage[0].value.view(),{})==0,"empty pairwise vector retains a typed zero reduction");
    pairwise.disabled(tree);pairwise.zero(0,1);
}
void matrix_pairwise_axis(std::size_t axis) {
    const auto rows=axis?3u:16u,columns=axis?16u:3u;
    std::array<double,48> values;values.fill(1.);
    if(axis){for(std::size_t row=0;row<3;++row)values[row*16]=1e16;values[15]=values[47]=0;}
    else {for(std::size_t column=0;column<3;++column)values[column]=1e16;values[45]=values[47]=0;}
    GeometryOperation pairwise(node_for("AxisSum",matrix_contract(DType::Float64,rows,columns),pairwise_parameters(axis)));
    auto input=pairwise.prefix(0,std::span<const double>(values),rows);
    require(pairwise.processor->process(std::span(&input,1),pairwise.outputs(),{})==Status::ok,"pairwise matrix reduction executes on either axis");
    const auto result=pairwise.output_storage[0].value.view();
    require(result.actual==std::array<std::size_t,2>{3,1},"pairwise matrix reduction retains the unselected dimension");
    require(number(result,{0,0,0})==1e16+12 && number(result,{0,1,0})==1e16+14 && number(result,{0,2,0})==1e16+12,"matrix reduction preserves strict native peak on either axis");
    pairwise.disabled(input);pairwise.zero(0,3);
}
void require_rejected_sum_policy(const ConfiguredProcessingNode& node) {
    const auto p=profile();const auto physical=numeric_operation_profile(p);
    const auto definition=numeric_operation_node_view(resolved(node));
    require(!p.operation_factory->assess(definition,physical).supported,"invalid accumulation policy rejected at cold admission");
    std::shared_ptr<const NumericOperationDescriptor> descriptor;
    require(p.operation_factory->describe(definition,descriptor)==Status::ok,"describe accumulation test declaration");
    std::unique_ptr<PreparedNumericOperation> prepared;
    require(p.operation_factory->prepare(std::move(descriptor),physical,prepared)==Status::incompatible_contract && !prepared,"invalid accumulation cannot construct a prepared kernel");
}
void invalid_sum_accumulation() {
    auto node=node_for("AxisSum",contract(DType::Float64,16,true),pairwise_parameters());
    auto cumulative=node;std::get<TransformProcessingParameters>(cumulative.parameters).values.push_back({"cumulative",true});
    require_rejected_sum_policy(cumulative);
    for(auto& field:std::get<TransformProcessingParameters>(node.parameters).values)
        if(field.key=="accumulation")field.value=NumericLiteral{std::uint64_t{2}};
    require_rejected_sum_policy(node);
}
void gather_rejects_multiple_index_planes() {
    const auto indices=contract(DType::UInt64,4,true);
    auto node=numeric_operation_node(node_for("ArrayGather",contract(DType::Float32,5),{},&indices));
    auto p=profile();p.admission.frame.max_planes=2;const auto physical=numeric_operation_profile(p);
    require(p.operation_factory->assess(node,physical).supported,"mono gather is supported in a two-plane profile");
    node.inputs[1].contract.plane_count=2;
    require(validate_contract(node.inputs[1].contract,physical.admission.frame)==Status::ok,"two-plane indices are a valid physical contract");
    require(!p.operation_factory->assess(node,physical).supported,"gather rejects multiple index planes at admission");
    std::shared_ptr<const NumericOperationDescriptor> descriptor;
    require(p.operation_factory->describe(node,descriptor)==Status::ok,"describe well-formed two-plane inputs");
    std::unique_ptr<PreparedNumericOperation> prepared;
    require(p.operation_factory->prepare(std::move(descriptor),physical,prepared)==Status::incompatible_contract && !prepared,
        "gather rejects multiple index planes at preparation");
}
}
int main() {
    disabled_maps<float>();disabled_maps<double>();
    disabled_matrix_reductions<float>();disabled_matrix_reductions<double>();
    disabled_vector_reductions<float>();disabled_vector_reductions<double>();
    disabled_comb<float>();disabled_comb<double>();gather_prefix<float>();gather_prefix<double>();
    gather_rejects_multiple_index_planes();scalar_map_offset_precision();map_rounds_before_offset();map_retains_explicit_coordinates();
    vector_sum_accumulation<float>();vector_sum_accumulation<double>();
    matrix_pairwise_axis(0);matrix_pairwise_axis(1);invalid_sum_accumulation();
    std::puts("common axis geometry PASS");
}
