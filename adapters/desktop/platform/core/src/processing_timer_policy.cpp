#include <desktop/processing_timer_policy.h>
#include <mutex>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

namespace desktop {
namespace {
struct State {
    std::mutex mutex;
    unsigned users=0;
#ifdef _WIN32
    struct PowerState {ULONG version,control,state;};
    using Query=BOOL(WINAPI*)(HANDLE,int,LPVOID,DWORD);
    using Set=BOOL(WINAPI*)(HANDLE,int,LPVOID,DWORD);
    static constexpr int power_class=4; // ProcessPowerThrottling
    static constexpr ULONG ignore_timer_resolution=4;
    Query query=nullptr;Set set=nullptr;
    PowerState original{1,0,0};
    bool applied=false;
    void acquire() {
        const auto kernel=GetModuleHandleW(L"kernel32.dll");
        query=kernel?reinterpret_cast<Query>(GetProcAddress(kernel,"GetProcessInformation")):nullptr;
        set=kernel?reinterpret_cast<Set>(GetProcAddress(kernel,"SetProcessInformation")):nullptr;
        if(!query||!set||!query(GetCurrentProcess(),power_class,&original,sizeof(original)))return;
        // Windows 11 can ignore Qt's existing precise timer requests when the
        // GUI is occluded. Processing must keep the same clock policy then.
        auto desired=original;
        desired.control|=ignore_timer_resolution;
        desired.state&=~ignore_timer_resolution;
        applied=set(GetCurrentProcess(),power_class,&desired,sizeof(desired));
    }
    void release() {
        if(!applied)return;
        PowerState current{1,0,0};
        if(query(GetCurrentProcess(),power_class,&current,sizeof(current))) {
            // Restore only our policy bit; preserve other policies changed
            // during the host lifetime by another application component.
            current.control=(current.control&~ignore_timer_resolution)|(original.control&ignore_timer_resolution);
            current.state=(current.state&~ignore_timer_resolution)|(original.state&ignore_timer_resolution);
            set(GetCurrentProcess(),power_class,&current,sizeof(current));
        }
        applied=false;
    }
#else
    void acquire() {}
    void release() {}
#endif
};
State& state(){static State value;return value;}
}
ProcessingTimerPolicy::ProcessingTimerPolicy(){auto& value=state();std::lock_guard lock(value.mutex);if(!value.users++)value.acquire();}
ProcessingTimerPolicy::~ProcessingTimerPolicy(){auto& value=state();std::lock_guard lock(value.mutex);if(!--value.users)value.release();}
}
