#include "parameter_sets.h"
#include "calibration_parameter_specs.h"
namespace domain {
namespace {
constexpr auto calibrated_fill=detail::with_calibration_frontend(detail::fill_parameters);
using DescriptorFamily=std::span<const ParameterDescriptor> (*)(NodeKind) noexcept;
constexpr DescriptorFamily families[]{detail::numeric_parameter_descriptors,detail::signal_parameter_descriptors,
    detail::music_parameter_descriptors,detail::routing_parameter_descriptors,
    detail::adapter_parameter_descriptors,detail::view_parameter_descriptors};
}
std::span<const ParameterDescriptor> parameter_descriptors(NodeKind kind) noexcept {
    // Calibration fields extend the generic fill schema at the composition boundary.
    if(kind==NodeKind::ArrayFill)return calibrated_fill;
    for(const auto family:families) {
        const auto fields=family(kind);if(!fields.empty())return fields;
    }
    return {};
}
}
