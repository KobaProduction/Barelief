#pragma once
#include <domain/processing_contracts.h>
#include <QString>
#include <optional>
namespace desktop {
enum class NumericSourceFormatError {
    None, UnsupportedBounds, MissingSource, InvalidContract, ConnectedOutputUnavailable, IncompatibleRoute, AdmissionLimit
};
struct NumericSourceFormatDiagnostic {
    NumericSourceFormatError error=NumericSourceFormatError::None;
    domain::NodeId node=-1;
    std::optional<domain::PortId> port;
};
std::uint32_t numeric_source_output_count(const domain::ConfiguredProcessingNode&,std::uint32_t native_outputs) noexcept;
// Device discovery declares only the native source. Domain derives dependent
// contracts; generators, clocks, cadence and other source settings stay explicit.
bool resolve_numeric_source_format(domain::NumericProcessingGraph&,domain::NodeId,
    std::uint32_t sample_rate,std::uint32_t outputs,const domain::ProcessingAdmissionLimits& admission,
    QString* error=nullptr,
    NumericSourceFormatDiagnostic* diagnostic=nullptr);
}
