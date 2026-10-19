#pragma once
#include <signal_contracts/data_contract.h>
#include <bdsp_protocol/numeric_data_message.h>
#include <cstdint>
namespace bdsp_protocol {
// External envelope editions belong to codecs and persistence adapters, never graph admission.
inline constexpr std::uint32_t document_version=4, control_version=4, data_version=numeric_data::PayloadVersion;
// Saved processing graph metadata edition differs from the DATA payload protocol.
inline constexpr std::uint32_t processing_data_version=5;
enum class ProcessingBoundary { Document, Control, Data };
inline signal_contracts::Status processing_boundary_version_supported(ProcessingBoundary boundary,std::uint32_t edition) noexcept {
    switch(boundary) {
    case ProcessingBoundary::Document:return edition==document_version?signal_contracts::Status::ok:signal_contracts::Status::unsupported_version;
    case ProcessingBoundary::Control:return edition==control_version?signal_contracts::Status::ok:signal_contracts::Status::unsupported_version;
    case ProcessingBoundary::Data:return edition==processing_data_version?signal_contracts::Status::ok:signal_contracts::Status::unsupported_version;
    }
    return signal_contracts::Status::invalid_argument;
}
}
