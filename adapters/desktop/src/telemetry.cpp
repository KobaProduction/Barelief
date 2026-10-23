#include <core/telemetry.h>
#include <atomic>
#include <cstdlib>
#include <cstring>

namespace core::telemetry {
namespace {
std::atomic<bool> collecting{false};
bool sampling=false;
}
bool enabled() noexcept{return collecting.load(std::memory_order_acquire);}
bool active() noexcept{return enabled() && TracyCIsConnected;}
bool sampling_active() noexcept{return sampling;}
void set_enabled(bool value) {
    if(value==enabled())return;
    if(value) {
        if(!TracyCIsStarted)___tracy_startup_profiler();
        collecting.store(true,std::memory_order_release);
        sampling=TracyCBeginSamplingProfiling()!=0;
    } else {
        collecting.store(false,std::memory_order_release);
        TracyCEndSamplingProfiling();sampling=false;
    }
}
Session::Session() {
    const auto* choice=std::getenv("BARELIEF_TELEMETRY");
    set_enabled(!choice || std::strcmp(choice,"0")!=0);
}
Session::~Session() {
    set_enabled(false);
    if(TracyCIsStarted)___tracy_shutdown_profiler();
}
void metric(const char* name,double milliseconds) noexcept {
    if(active())TracyCPlot(name,milliseconds);
}
void thread_name(const char* name) {
    if(TracyCIsStarted)___tracy_set_thread_name(name);
}
}
