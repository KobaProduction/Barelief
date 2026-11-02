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
bool interval_operation_kind(std::string_view) noexcept;
std::span<const ParameterDescriptor> interval_operation_parameters(std::string_view) noexcept;
std::size_t interval_operation_output_count(std::string_view) noexcept;
signal_contracts::Status interval_operation_output_contracts(const ConfiguredProcessingNode&,
    const signal_contracts::NumericContract&,std::vector<OwnedProcessingContract>&) noexcept;
}
