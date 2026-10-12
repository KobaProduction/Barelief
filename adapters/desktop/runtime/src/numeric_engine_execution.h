#pragma once
#include <pipeline/numeric_engine.h>
#include <dataflow_runtime/runtime.h>
#include <QTimer>
#include <core/performance.h>
#include <chrono>
#include <mutex>

namespace pipeline::numeric_engine_detail {
using namespace signal_contracts;
inline constexpr FrameLimits frame_limits{8,32768,262144,16384,256,8192};
inline std::uint64_t steady_ns() noexcept {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}
inline QString failure(Status status) {return QStringLiteral("Numeric processing refused (%1)").arg(int(status));}
// One execution boundary serializes graph replacement, ingress and delivery state.
// Its mutex must cover all synchronous borrowed callbacks, never consumer calls.
struct ExecutionBoundary {
    NumericEngine& owner;
    mutable std::mutex mutex;
    EngineSnapshot state;
    dataflow_runtime::GraphExecutor executor;
    ExecutionBoundary(NumericEngine& owner,const dataflow_runtime::NumericRuntimeProfile& profile):owner(owner),executor(profile){}
};
}
