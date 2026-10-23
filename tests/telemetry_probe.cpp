#include <core/performance.h>
#include <chrono>
#include <thread>
#include <cstdio>
#include <cstdlib>
#include <type_traits>

static void measured_work() {
    BARELIEF_PROFILE_FUNCTION();
    core::perf::Scope span(core::perf::Stage::EditorNavigation);
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
}
#if BARELIEF_DEV_TELEMETRY
static bool exercise_pause() {
    const auto prior=core::perf::counters[size_t(core::perf::Stage::EditorNavigation)].count.load();
    const bool initial=core::telemetry::enabled();
    {core::perf::Scope outer(core::perf::Stage::GuiDeliveryDrain);core::telemetry::set_enabled(false);}
    measured_work();
    const bool stopped=!core::telemetry::enabled() && !core::telemetry::active() &&
        core::perf::counters[size_t(core::perf::Stage::EditorNavigation)].count.load()==prior;
    core::telemetry::set_enabled(initial);return stopped;
}
#endif
int main(int argc,char** argv) {
    core::telemetry::Session session;
    core::telemetry::thread_name("telemetry-probe");
#if BARELIEF_DEV_TELEMETRY
    if(argc==2) {
        const auto end=std::chrono::steady_clock::now()+std::chrono::seconds(std::atoi(argv[1]));
        const auto pause_at=std::chrono::steady_clock::now()+std::chrono::seconds(1);bool checked=false;
        while(std::chrono::steady_clock::now()<end) {
            measured_work();core::telemetry::metric("probe.interval_ms",2);
            if(!checked && std::chrono::steady_clock::now()>=pause_at){if(!exercise_pause())return 1;checked=true;}
        }
        return 0;
    }
    const auto* choice=std::getenv("BARELIEF_TELEMETRY");
    if(choice && choice[0]=='0' && core::telemetry::enabled())return 1;
    if(!exercise_pause())return 1;
#else
    static_assert(!core::telemetry::active());
    static_assert(std::is_empty_v<core::perf::Scope>);
    static_assert(std::is_empty_v<core::telemetry::Session>);
#endif
    measured_work();
    std::puts("telemetry lifetime, disable and compile-time policy PASS");
}
