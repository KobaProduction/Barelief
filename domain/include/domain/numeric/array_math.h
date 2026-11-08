#pragma once
#include <domain/graph/processing_model.h>
#include <optional>
#include <string_view>
namespace domain {
enum class ArrayMathOperation {Multiply,Add,Subtract,Divide};
enum class ZeroDivisionPolicy {Reject};
enum class ArrayTrigger:std::int32_t {Automatic=-1};
inline constexpr std::size_t automatic_array_trigger=static_cast<std::size_t>(-1);
enum class AbsoluteValueInput:PortId {Values=0};
enum class AbsoluteValueOutput:PortId {AbsoluteValues=0};
enum class BinaryInput:PortId {Left=0,Right=1};
enum class DivisionInput:PortId {Numerator=0,Denominator=1};
inline constexpr PortId array_result_output=0;
constexpr std::optional<ArrayMathOperation> array_math_operation(std::string_view text) noexcept {
    if(text=="multiply")return ArrayMathOperation::Multiply;
    if(text=="add")return ArrayMathOperation::Add;
    if(text=="subtract")return ArrayMathOperation::Subtract;
    if(text=="divide")return ArrayMathOperation::Divide;
    return {};
}
constexpr std::string_view array_math_name(ArrayMathOperation value) noexcept {
    switch(value){case ArrayMathOperation::Multiply:return "multiply";case ArrayMathOperation::Add:return "add";case ArrayMathOperation::Subtract:return "subtract";case ArrayMathOperation::Divide:return "divide";}
    return {};
}
constexpr std::optional<ZeroDivisionPolicy> zero_division_policy(std::string_view text) noexcept {
    return text=="reject"?std::optional{ZeroDivisionPolicy::Reject}:std::nullopt;
}
}
