#pragma once
#ifndef BARELIEF_DEV_TELEMETRY
#define BARELIEF_DEV_TELEMETRY 0
#endif

#if BARELIEF_DEV_TELEMETRY
#include <tracy/TracyC.h>
#include <cstddef>
namespace core::telemetry {
bool enabled() noexcept;
bool active() noexcept;
bool sampling_active() noexcept;
void set_enabled(bool enabled);
void metric(const char* static_name, double milliseconds) noexcept;
void thread_name(const char* name);
class Session {
public:
    Session();
    ~Session();
    Session(const Session&)=delete;
    Session& operator=(const Session&)=delete;
};
class Zone {
    TracyCZoneCtx context_{};
public:
    explicit Zone(const ___tracy_source_location_data& source) noexcept {
        if(active())context_=___tracy_emit_zone_begin(&source,1);
    }
    ~Zone(){if(context_.active)___tracy_emit_zone_end(context_);}
    Zone(const Zone&)=delete;
    Zone& operator=(const Zone&)=delete;
    void text(const char* data,std::size_t size) {
        if(context_.active)___tracy_emit_zone_text(context_,data,size);
    }
};
}
#define BARELIEF_TELEMETRY_JOIN_(a,b) a##b
#define BARELIEF_TELEMETRY_JOIN(a,b) BARELIEF_TELEMETRY_JOIN_(a,b)
#define BARELIEF_PROFILE_FUNCTION() \
    static const ___tracy_source_location_data BARELIEF_TELEMETRY_JOIN(telemetry_source_,__LINE__){nullptr,__func__,__FILE__,__LINE__,0}; \
    const core::telemetry::Zone BARELIEF_TELEMETRY_JOIN(telemetry_zone_,__LINE__){BARELIEF_TELEMETRY_JOIN(telemetry_source_,__LINE__)}
#else
namespace core::telemetry {
inline constexpr bool enabled() noexcept{return false;}
inline constexpr bool active() noexcept{return false;}
inline constexpr bool sampling_active() noexcept{return false;}
inline void set_enabled(bool){}
inline void metric(const char*,double) noexcept{}
inline void thread_name(const char*){}
struct Session {};
}
#define BARELIEF_PROFILE_FUNCTION() ((void)0)
#endif
