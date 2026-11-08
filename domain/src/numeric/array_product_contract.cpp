#include <domain/numeric/contracts.h>
#include <domain/numeric/array_math.h>
#include <cmath>
#include <algorithm>
#include <optional>
namespace domain {
using namespace signal_contracts;
namespace {
Status find_driver_port(const ConfiguredProcessingNode& node,double requested,std::size_t& out) {
    if(!std::isfinite(requested) || requested<0 || requested>65535 || std::floor(requested)!=requested)return Status::invalid_argument;
    for(std::size_t i=0;i<node.inputs.size();++i)if(node.inputs[i].id==PortId(requested)){out=i;return Status::ok;}
    return Status::invalid_argument;
}
Status selected_driver(const ConfiguredProcessingNode& node,std::size_t& out) {
    const ResolvedProcessingNode view{node.id,node.kind,NodeRole::Processor,ProcessingOperation::ArrayProduct,node.inputs,node.outputs,&node.parameters,node.enabled};
    ProcessingParameterView parameter;
    if(resolve_processing_parameter(view,"frame_input",parameter)!=Status::ok)return Status::invalid_argument;
    const auto* literal=std::get_if<NumericLiteral>(&parameter);if(!literal)return Status::invalid_argument;
    const auto requested=std::visit([](auto v){return static_cast<double>(v);},*literal);
    return find_driver_port(node,requested,out);
}
Status selected_trigger(const ConfiguredProcessingNode& node,std::size_t& out) {
    const ResolvedProcessingNode view{node.id,node.kind,NodeRole::Processor,ProcessingOperation::ArrayProduct,node.inputs,node.outputs,&node.parameters,node.enabled};
    ProcessingParameterView parameter;
    if(resolve_processing_parameter(view,"trigger_input",parameter)!=Status::ok)return Status::invalid_argument;
    const auto* literal=std::get_if<NumericLiteral>(&parameter);if(!literal)return Status::invalid_argument;
    const auto requested=std::visit([](auto v){return static_cast<double>(v);},*literal);
    if(requested==double(ArrayTrigger::Automatic)){out=automatic_array_trigger;return Status::ok;}
    return find_driver_port(node,requested,out);
}
bool product_value_valid(const NumericContract& value) {
    const bool fixed_columns=value.shape.rank!=Rank::Matrix ||
        (value.shape.extents[1].minimum && value.shape.extents[1].minimum==value.shape.extents[1].maximum);
    return fixed_columns && (value.dtype==DType::Float32 || value.dtype==DType::Float64) && value.channel_order.size()==1 && value.shape.extents[0].maximum &&
        value.representation.encoding==StorageEncoding::Native && value.representation.layout==ChannelLayout::Planar &&
        (value.representation.normalization==Normalization::None || value.representation.normalization==Normalization::FloatUnit);
}
struct VectorGrid {Axis axis;AxisCoordinates coordinates;bool operator==(const VectorGrid&)const=default;};
bool accept_grid(const NumericContract& value,std::size_t axis,std::optional<VectorGrid>& grid) {
    if(value.coordinates[axis].mode==CoordinateMode::None)return true;
    const VectorGrid current{value.shape.axes[axis],value.coordinates[axis]};
    if(grid && *grid!=current)return false;
    grid=current;return true;
}
bool product_geometry(const NumericContract& driver,const NumericContract& factor,std::array<std::optional<VectorGrid>,2>& grid) {
    if(factor.shape.rank==Rank::Scalar)return true;
    if(driver.shape.rank==Rank::Scalar)return false;
    const bool broadcast=driver.shape.rank==Rank::Matrix && factor.shape.rank==Rank::Vector;
    if(broadcast)return factor.shape.extents[0].maximum==driver.shape.extents[1].maximum && accept_grid(factor,0,grid[1]);
    if(driver.shape.rank!=factor.shape.rank)return false;
    if(driver.shape.rank==Rank::Vector)return driver.shape.extents[0].maximum==factor.shape.extents[0].maximum && accept_grid(factor,0,grid[0]);
    if(driver.shape.extents!=factor.shape.extents)return false;
    return accept_grid(factor,0,grid[0]) && (factor.shape.rank!=Rank::Matrix || accept_grid(factor,1,grid[1]));
}
Status validate_inputs(const ConfiguredProcessingNode& node,std::size_t driver) {
    const auto contract=node.inputs[driver].contract.view();const auto* shape=std::get_if<NumericContract>(&contract);
    if(!shape || !product_value_valid(*shape))return Status::incompatible_contract;
    std::array<std::optional<VectorGrid>,2> grid;
    for(const auto& port:node.inputs) {
        const auto data=port.contract.view();const auto* value=std::get_if<NumericContract>(&data);
        if(!value || !product_value_valid(*value))return Status::incompatible_contract;
        if(!product_geometry(*shape,*value,grid))return Status::incompatible_contract;
    }
    return Status::ok;
}
bool division_policy_valid(const ResolvedProcessingNode& node) {
    ProcessingParameterView field;
    if(resolve_processing_parameter(node,"zero_division",field)!=Status::ok)return false;
    const auto* policy=std::get_if<std::string_view>(&field);
    return policy && zero_division_policy(*policy).has_value();
}
bool operation_valid(const ConfiguredProcessingNode& node) {
    const ResolvedProcessingNode view{node.id,node.kind,NodeRole::Processor,ProcessingOperation::ArrayProduct,node.inputs,node.outputs,&node.parameters,node.enabled};
    ProcessingParameterView field;
    if(resolve_processing_parameter(view,"operation",field)!=Status::ok)return false;
    const auto* text=std::get_if<std::string_view>(&field);
    if(!text || !array_math_operation(*text))return false;
    if(*text!="divide" && *text!="subtract")return true;
    if(node.inputs.size()!=2)return false;
    return *text=="subtract" || division_policy_valid(view);
}
bool binary_inputs_valid(const ConfiguredProcessingNode& node) {
    const ResolvedProcessingNode view{node.id,node.kind,NodeRole::Processor,ProcessingOperation::ArrayProduct,node.inputs,node.outputs,&node.parameters,node.enabled};
    ProcessingParameterView field;if(resolve_processing_parameter(view,"operation",field)!=Status::ok)return false;
    const auto* text=std::get_if<std::string_view>(&field);if(!text)return false;
    if(*text!="divide" && *text!="subtract")return true;
    const auto left=PortId(BinaryInput::Left),right=PortId(BinaryInput::Right);
    return (node.inputs[0].id==left && node.inputs[1].id==right) || (node.inputs[0].id==right && node.inputs[1].id==left);
}
NumericContract product_value_contract(const ConfiguredProcessingNode& node,std::size_t driver) {
    const auto contract=node.inputs[driver].contract.view();auto output=std::get<NumericContract>(contract);
    if(output.quantity!=Quantity::Unspecified || output.unit!=NumericUnit::Values)return output;
    for(const auto& input:node.inputs) {
        const auto data=input.contract.view();const auto& value=std::get<NumericContract>(data);
        if(value.quantity==Quantity::Unspecified && value.unit==NumericUnit::Values)continue;
        output.quantity=value.quantity;output.unit=value.unit;output.semantic=value.semantic;return output;
    }
    return output;
}
Status own_product_contract(const ConfiguredProcessingNode& node,std::size_t driver,OwnedProcessingContract& result) {
    auto output=node.enabled?product_value_contract(node,driver):std::get<NumericContract>(node.inputs[driver].contract.view());
    output.clock=std::get<NumericContract>(node.inputs[driver].contract.view()).clock;
    if(node.enabled){
        output.representation.normalization=Normalization::None;
        for(const auto& port:node.inputs)if(std::get<NumericContract>(port.contract.view()).dtype==DType::Float64){output.dtype=DType::Float64;output.representation.alignment=alignof(double);}
    }
    return own_processing_contract(output,FrameLimits{1,SIZE_MAX,SIZE_MAX,SIZE_MAX,SIZE_MAX,SIZE_MAX},SIZE_MAX,result);
}
Status assign_product_contract(const ConfiguredProcessingNode& node,std::size_t driver,std::size_t trigger,
    OwnedProcessingContract& result,std::size_t* frame_input,std::size_t* trigger_input) {
    OwnedProcessingContract candidate;
    const auto status=own_product_contract(node,driver,candidate);
    if(status!=Status::ok)return status;
    result=std::move(candidate);
    if(frame_input)*frame_input=driver;
    if(trigger_input)*trigger_input=trigger;
    return Status::ok;
}
}
Status array_product_output_contract(const ConfiguredProcessingNode& node,OwnedProcessingContract& result,std::size_t* frame_input,std::size_t* trigger_input) noexcept {
    if(node.kind!="ArrayMultiply" || node.inputs.size()<2 || node.inputs.size()>max_variadic_inputs || !operation_valid(node))return Status::invalid_argument;
    std::size_t driver=0,trigger=0;
    if(selected_driver(node,driver)!=Status::ok || selected_trigger(node,trigger)!=Status::ok)return Status::invalid_argument;
    if(validate_inputs(node,driver)!=Status::ok)return Status::incompatible_contract;
    if(!binary_inputs_valid(node))return Status::invalid_argument;
    return assign_product_contract(node,driver,trigger,result,frame_input,trigger_input);
}
}
