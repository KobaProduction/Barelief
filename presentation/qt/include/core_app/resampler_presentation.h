#pragma once
#include <signal_contracts/resampling_policy.h>
namespace presentation {
inline const char* resampler_caption(signal_contracts::ResamplerMode mode) {
    using M=signal_contracts::ResamplerMode;
    switch(mode){
    case M::Bypass:return "Bypass (native sample rate)";
    case M::ExistingFir:return "FIR (full quality)";
    case M::CompactFir:return "Compact FIR";
    case M::SpeexDsp:return "SpeexDSP";
    case M::IirExperimental:return "IIR (experimental)";
    case M::LinearDiagnostic:return "Linear (diagnostic, aliasing risk)";
    }
    return "Unavailable";
}
}
