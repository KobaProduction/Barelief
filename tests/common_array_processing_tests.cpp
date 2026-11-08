#include "numeric_operation_lifecycle_fixture.h"
#include <domain/numeric/array_processing_ports.h>
#include <rhythm_graph_adapters/tracking_composition.h>
#include <rhythm_graph_adapters/essentia_ports.h>
#include <bit>
#include <dsp_kernels/recursive_filter.h>
#include <dsp_kernels/vector_normalization.h>
#include <cmath>
#include <limits>
namespace {
using namespace operation_lifecycle;
ProcessingParameter size_parameter(std::string key,std::size_t value){return {std::move(key),NumericLiteral{std::uint64_t(value)}};}
OwnedProcessingContract matrix_contract(std::size_t rows,std::size_t columns){
    static constexpr std::uint32_t mono=0;NumericContract value;value.channel_order={&mono,1};value.dtype=DType::Float32;
    value.unit=NumericUnit::Values;value.semantic=Semantic::Numeric;value.shape={Rank::Matrix,{Axis::Declared,Axis::Component},{{{0,rows},{columns,columns}}}};
    OwnedProcessingContract result;
    require(own_processing_contract(value,profile().admission.frame,65536,result)==Status::ok,"own explicit matrix");return result;
}
ConfiguredProcessingNode array_node(std::string_view kind,const OwnedProcessingContract& input,TransformProcessingParameters parameters,
    const OwnedProcessingContract* second=nullptr){
    ConfiguredProcessingNode node;node.id=10;node.kind=kind;node.parameters=std::move(parameters);
    node.inputs={{0,PortDirection::Input,input}};if(second)node.inputs.push_back({1,PortDirection::Input,*second});
    std::vector<OwnedProcessingContract> outputs;
    require(common_operation_output_contracts(node,std::get<NumericContract>(input.view()),outputs)==Status::ok,"derive array output contracts");
    for(std::size_t i=0;i<outputs.size();++i)node.outputs.push_back({PortId(i),PortDirection::Output,std::move(outputs[i])});return node;
}
struct ArrayOperation:Operation {
    explicit ArrayOperation(ConfiguredProcessingNode node):Operation(std::move(node)){
        for(std::size_t i=0;i<this->node.outputs.size();++i){
            const auto& contract=output_contracts[i];const auto columns=std::size_t(contract.shape.extents[1].maximum);
            output_planes[i][0]={0,{columns*sizeof(float),contract.shape.rank==Rank::Matrix?sizeof(float):0}};
            output_storage[i].value.actual={std::size_t(contract.shape.extents[0].maximum),columns};
        }
    }
    NumericOperationInput matrix_input(std::size_t slot,std::span<const float> values,std::size_t rows,std::size_t columns){
        input_planes[slot][0]={0,{columns*sizeof(float),sizeof(float)}};
        auto result=input(slot,values);result.value.actual={rows,columns};result.contiguous_float32={};return result;
    }
    void expected(std::size_t port,std::span<const float> values,std::size_t rows,std::size_t columns,float tolerance=0){
        const auto& output=output_storage[port];require(output.status==Status::ok,"array publication available");
        require(output.value.actual==std::array<std::size_t,2>{rows,columns},"actual array geometry");
        for(std::size_t row=0;row<rows;++row)for(std::size_t column=0;column<columns;++column){
            float actual=0;require(load_element(output.value.view(),{0,row,column},actual)==Status::ok,"read array output");
            require(std::abs(actual-values[row*columns+column])<=tolerance,"explicit mathematical array result");
        }
    }
};
void explicit_filter_state(){
    const auto initial=contract(DType::Float32,2);TransformProcessingParameters parameters{{
        {"numerator",NumericArray{{.5f,.5f}}},{"denominator",NumericArray{{1}}},size_parameter("state_policy",1)}};
    ArrayOperation operation(array_node("RecursiveFilter",matrix_contract(2,2),parameters,&initial));
    const std::array<float,4> data{2,4,6,8};const std::array<float,2> state{7,0};
    const std::array inputs{operation.matrix_input(0,data,2,2),operation.input(1,std::span<const float>(state))};
    require(operation.processor->process(inputs,operation.outputs(),{})==Status::ok,"state crosses explicit filter edge");
    operation.expected(0,std::array<float,4>{8,3,5,7},2,2);operation.expected(1,std::array<float,2>{4,0},2,1);
    require(operation.processor->process(inputs,operation.outputs(),{})==Status::ok,"batch reset is deterministic");
    operation.expected(0,std::array<float,4>{8,3,5,7},2,2);
    std::get<TransformProcessingParameters>(operation.node.parameters).values.back()=size_parameter("state_policy",2);
    ArrayOperation independent(operation.node);
    const std::array independent_inputs{independent.matrix_input(0,data,2,2),independent.input(1,std::span<const float>(state))};
    require(independent.processor->process(independent_inputs,independent.outputs(),{})==Status::ok,"independent row state");
    independent.expected(0,std::array<float,4>{8,3,10,7},2,2);
}
void state_rejection_is_atomic(){
    dsp_kernels::PreparedRecursiveFilter filter;require(filter.prepare(std::array{.5f,.5f},std::array{1.f}),"prepare state primitive");
    require(filter.set_state(std::array{7.f,0.f}),"set delay state");
    require(!filter.set_state(std::array{1.f}),"wrong state extent rejected");
    require(!filter.set_state(std::array{std::numeric_limits<float>::quiet_NaN(),0.f}),"nonfinite state rejected");
    float result=0;require(filter.process(2,result) && result==8,"rejected state leaves the prior delay intact");
}
void matrix_normalization(){
    ArrayOperation operation(array_node("VectorNormalize",matrix_contract(3,2),TransformProcessingParameters{{size_parameter("method",6),size_parameter("axis",0)}}));
    const std::array<float,6> data{1,2,3,2,5,2};auto input=operation.matrix_input(0,data,3,2);
    require(operation.processor->process(std::span(&input,1),operation.outputs(),{})==Status::ok,"normalize selected time axis");
    const float deviation=std::sqrt(8.f/3.f);
    operation.expected(0,std::array<float,6>{1/deviation,2,3/deviation,2,5/deviation,2},3,2);
    ArrayOperation sum(array_node("VectorNormalize",matrix_contract(2,2),TransformProcessingParameters{{size_parameter("method",5)}}));
    const std::array<float,4> sums{1,3,2,6};input=sum.matrix_input(0,sums,2,2);
    require(sum.processor->process(std::span(&input,1),sum.outputs(),{})==Status::ok,"row sum normalization");
    sum.expected(0,std::array<float,4>{.25f,.75f,.25f,.75f},2,2);
    const float tiny=std::numeric_limits<float>::denorm_min();std::array values{tiny,2*tiny};
    require(dsp_kernels::normalize_vector(values,dsp_kernels::VectorNormalization::Maximum,2,0) && values==std::array{.5f,1.f},"explicit zero threshold normalizes subnormal values");
}
void matrix_correlation(){
    const auto contract=matrix_contract(2,3);
    ArrayOperation operation(array_node("DirectCorrelation",contract,TransformProcessingParameters{{size_parameter("lags",3),size_parameter("normalization",1)}},&contract));
    const auto declared=std::get<NumericContract>(operation.node.outputs[0].contract.view());
    require(declared.shape.extents[0]==ExtentBounds{0,2} && declared.shape.extents[1]==ExtentBounds{3,3},"correlation keeps matrix row bounds and fixed lag columns");
    const std::array<float,6> data{1,2,3,2,0,0};
    const std::array inputs{operation.matrix_input(0,data,2,3),operation.matrix_input(1,data,2,3)};
    require(operation.processor->process(inputs,operation.outputs(),{})==Status::ok,"matrix lag rows");
    operation.expected(0,std::array<float,6>{14.f/3,4,3,4.f/3,0,0},2,3);
}
void fixed_vector_correlation(){
    const auto input_contract=contract(DType::Float32,5,true);
    ArrayOperation operation(array_node("DirectCorrelation",input_contract,TransformProcessingParameters{{size_parameter("lags",3)}},&input_contract));
    const auto declared=std::get<NumericContract>(operation.node.outputs[0].contract.view());
    require(declared.shape.extents[0]==ExtentBounds{3,3},"correlation declares its fixed configured lag count for bounded input");
    const std::array<float,5> data{1,2,3,4,0};
    std::array inputs{operation.input(0,std::span<const float>(data)),operation.input(1,std::span<const float>(data))};
    for(auto& input:inputs){input.value.actual[0]=4;input.contiguous_float32=input.contiguous_float32.first(4);}
    require(operation.processor->process(inputs,operation.outputs(),{})==Status::ok,"bounded vector correlation publishes available configured lags");
    operation.expected(0,std::array<float,3>{30,20,11},3,1);
}
void explicit_reduction_order(){
    const auto input_contract=contract(DType::Float32,10);
    ArrayOperation plain(array_node("FeatureAggregate",input_contract,TransformProcessingParameters{{size_parameter("method",3),{"compensated",false}}}));
    ArrayOperation compensated(array_node("FeatureAggregate",input_contract,TransformProcessingParameters{{size_parameter("method",3)}}));
    const std::array<float,10> data{1e8f,1,1,1,1,1,1,1,1,-1e8f};
    auto input=plain.input(0,std::span<const float>(data));
    require(plain.processor->process(std::span(&input,1),plain.outputs(),{})==Status::ok,"explicit plain Float32 sum");plain.expected(0,std::array{0.f},1,1);
    input=compensated.input(0,std::span<const float>(data));
    require(compensated.processor->process(std::span(&input,1),compensated.outputs(),{})==Status::ok,"default compensated sum remains available");
    compensated.expected(0,std::array{8.f},1,1);
}
void matrix_inverse_and_log(){
    const auto contract=matrix_contract(2,4);
    ArrayOperation inverse(array_node("InverseFourier",contract,TransformProcessingParameters{{size_parameter("fft_size",4)}},&contract));
    const std::array<float,8> real{4,0,0,0,0,4,0,0},imaginary{};
    const std::array inputs{inverse.matrix_input(0,real,2,4),inverse.matrix_input(1,imaginary,2,4)};
    require(inverse.processor->process(inputs,inverse.outputs(),{})==Status::ok,"independent inverse Fourier rows");
    inverse.expected(0,std::array<float,8>{1,1,1,1,1,0,-1,0},2,4,1e-6f);
    inverse.expected(1,std::array<float,8>{0,0,0,0,0,1,0,-1},2,4,1e-6f);
    ArrayOperation clamp(array_node("Clamp",matrix_contract(2,2),TransformProcessingParameters{{scalar("minimum",.01f),scalar("maximum",100.f)}}));
    const std::array<float,4> power{0,.1f,1,1000};auto input=clamp.matrix_input(0,power,2,2);
    require(clamp.processor->process(std::span(&input,1),clamp.outputs(),{})==Status::ok,"explicit matrix log floor");
    clamp.expected(0,std::array<float,4>{.01f,.1f,1,100},2,2);
    ArrayOperation logarithm(array_node("LogCompression",matrix_contract(2,2),TransformProcessingParameters{{scalar("add",0),size_parameter("base",0)}}));
    const std::array<float,4> floored{.01f,.1f,1,100};input=logarithm.matrix_input(0,floored,2,2);
    require(logarithm.processor->process(std::span(&input,1),logarithm.outputs(),{})==Status::ok,"row log10");
    logarithm.expected(0,std::array<float,4>{-2,-1,0,2},2,2,1e-6f);
}
void renewal_tick_grid(){
    const std::array contracts{contract(DType::Float32,128),contract(DType::Float32,1,true),contract(DType::Float32,1,true)};
    const float rate=44100.f/1024.f,resolution=1.f/(2*rate);
    TransformProcessingParameters settings{{scalar("frame_rate",rate),size_parameter("upsampling",2),scalar("maximum_period_s",1.5f)}};
    ConfiguredProcessingNode node;
    require(rhythm_graph_adapters::make_tracking_operation_node(10,"RenewalBeatDecoder",contracts,settings,
        profile().admission.frame,65536,node)==Status::ok,"compose explicit renewal ports");
    ArrayOperation operation(std::move(node));std::array<float,128> values{};
    values[25]=values[68]=values[111]=1;
    const std::array<float,1> periods{.5f},ends{2.f};
    const std::array inputs{operation.input(0,std::span<const float>(values)),operation.input(1,std::span<const float>(periods)),operation.input(2,std::span<const float>(ends))};
    require(operation.processor->process(inputs,operation.outputs(),{})==Status::ok,"renewal exposes beat mask and seconds");
    const auto ticks=std::size_t(rhythm_graph_adapters::RenewalOutput::Times);std::size_t count=0;
    for(std::size_t i=0;i<values.size();++i){
        std::uint8_t beat=0;require(load_element(operation.output_storage[0].value.view(),{0,i,0},beat)==Status::ok,"read explicit beat mask");
        if(!beat)continue;
        const float actual=operation.value(ticks,count++),expected=float(i)*resolution;
        require(std::bit_cast<std::uint32_t>(actual)==std::bit_cast<std::uint32_t>(expected),"source tick grid multiplies by the prepared Float32 resolution");
    }
    require(count>0 && operation.output_storage[ticks].value.actual[0]==count,"tick count matches the complete mask");
}
void expansion_and_quantile(){
    ArrayOperation expansion(array_node("LinearExpansion",contract(DType::Float32,5,true),TransformProcessingParameters{{size_parameter("factor",2)}}));
    const std::array<float,5> data{1,3,7,0,0};auto input=expansion.input(0,std::span<const float>(data));input.value.actual={3,1};
    require(expansion.processor->process(std::span(&input,1),expansion.outputs(),{})==Status::ok,"variable endpoint expansion");
    expansion.expected(0,std::array<float,5>{1,2,3,5,7},5,1);
    input.value.actual={0,1};require(expansion.processor->process(std::span(&input,1),expansion.outputs(),{})==Status::ok,"empty expansion prefix");
    expansion.expected(0,{},0,1);
    ArrayOperation quantile(array_node("QuantileMask",contract(DType::Float32,5),TransformProcessingParameters{}));
    const std::array<float,5> weights{1,1,2,3,3};input=quantile.input(0,std::span<const float>(weights));
    require(quantile.processor->process(std::span(&input,1),quantile.outputs(),{})==Status::ok,"inclusive quantile mask");
    quantile.expected(0,std::array<float,5>{0,0,0,1,1},5,1);
}
}
int main(){
    explicit_filter_state();state_rejection_is_atomic();matrix_normalization();matrix_correlation();fixed_vector_correlation();explicit_reduction_order();matrix_inverse_and_log();renewal_tick_grid();expansion_and_quantile();
    std::puts("common array processing PASS");
}
