#pragma once
#include <cstddef>
#include <span>
#include <string_view>
#include <vector>
namespace signal_contracts { enum class Status; struct NumericContract; }
namespace domain {
struct ParameterDescriptor;
struct ConfiguredProcessingNode;
class OwnedProcessingContract;
struct CommonOperationSpec {std::string_view kind;std::size_t inputs,outputs;std::string_view stages;};
std::span<const CommonOperationSpec> common_operation_catalogue() noexcept;
const CommonOperationSpec* common_operation_spec(std::string_view) noexcept;
bool common_operation_kind(std::string_view) noexcept;
std::span<const ParameterDescriptor> common_operation_parameters(std::string_view) noexcept;
signal_contracts::Status common_operation_output_contracts(const ConfiguredProcessingNode&,
    const signal_contracts::NumericContract&,std::vector<OwnedProcessingContract>&) noexcept;
}
