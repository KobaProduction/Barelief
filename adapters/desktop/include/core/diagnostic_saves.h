#pragma once
#if defined(BARELIEF_DEV_TELEMETRY) && BARELIEF_DEV_TELEMETRY
#include <cstdlib>
namespace core::diagnostics {
inline bool trace_saves() noexcept{return std::getenv("CORE_APP_TRACE_SAVES")!=nullptr;}
}
#else
namespace core::diagnostics {
inline constexpr bool trace_saves() noexcept{return false;}
}
#endif
