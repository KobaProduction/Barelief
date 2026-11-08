#pragma once
#include <domain/graph/processing_model.h>
#include <signal_contracts/window_policy.h>
#include <numbers>
namespace domain {
namespace window_generation {
struct WindowFunctionParameters {
    signal_contracts::WindowFunction function=signal_contracts::WindowFunction::Hann;
    signal_contracts::WindowEvaluation evaluation=signal_contracts::WindowEvaluation::Default;
    double pi=std::numbers::pi;
    bool periodic=false;
};
}
bool window_function_parameters(const ResolvedProcessingNode&,window_generation::WindowFunctionParameters&) noexcept;
}
