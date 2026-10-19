#pragma once
#include <array>
#include <span>
#include <string_view>
namespace signal_contracts {
enum class ResamplerMode { Bypass, ExistingFir, CompactFir, SpeexDsp, IirExperimental, LinearDiagnostic };
struct ResamplerPolicy {ResamplerMode mode;std::string_view id;};
inline constexpr std::array resampler_policy_registry{
    ResamplerPolicy{ResamplerMode::Bypass,"bypass"},
    ResamplerPolicy{ResamplerMode::ExistingFir,"existing_fir"},
    ResamplerPolicy{ResamplerMode::CompactFir,"compact_fir"},
    ResamplerPolicy{ResamplerMode::SpeexDsp,"speex"},
    ResamplerPolicy{ResamplerMode::IirExperimental,"iir"},
    ResamplerPolicy{ResamplerMode::LinearDiagnostic,"linear"}};
inline std::span<const ResamplerPolicy> resampler_policies() noexcept {return resampler_policy_registry;}
inline bool parse_resampler_mode(std::string_view id,ResamplerMode& out) noexcept {
    for(const auto& policy:resampler_policies())if(policy.id==id){out=policy.mode;return true;}
    return false;
}
inline std::string_view resampler_mode_name(ResamplerMode mode) noexcept {
    for(const auto& policy:resampler_policies())if(policy.mode==mode)return policy.id;
    return {};
}
}
