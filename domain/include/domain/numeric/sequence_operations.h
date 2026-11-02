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
enum class SequenceOperationFamily {Onset,Tempo,BeatPulse,Histogram,State,Model,Spectral};
struct SequenceOperationSpec {
    std::string_view kind;
    SequenceOperationFamily family;
    std::size_t inputs,outputs;
    // Explicit source chain; stages name shared portable processors, not hidden
    // metadata/topology dependencies. Source revisions are pinned in PIN.json.
    std::string_view stages;
};
std::span<const SequenceOperationSpec> sequence_operation_catalogue() noexcept;
const SequenceOperationSpec* sequence_operation_spec(std::string_view) noexcept;
bool sequence_operation_kind(std::string_view) noexcept;
std::size_t sequence_operation_output_count(std::string_view) noexcept;
std::span<const ParameterDescriptor> sequence_operation_parameters(std::string_view) noexcept;
std::span<const ParameterDescriptor> spectral_sequence_parameters(std::string_view) noexcept;
// Source-model operations may require explicit output contracts because their
// geometry belongs to injected classical parameters, unavailable to domain.
signal_contracts::Status sequence_operation_output_contracts(const ConfiguredProcessingNode&,
    const signal_contracts::NumericContract&,std::vector<OwnedProcessingContract>&) noexcept;
}
