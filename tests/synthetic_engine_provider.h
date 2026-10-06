#pragma once
#include "synthetic_capture.h"
#include "core_app/desktop_engine_provider.h"
namespace test_fixture {
inline std::shared_ptr<DesktopEngineProvider> synthetic_engine_provider(bool deliver_audio = true) {
    return std::make_shared<DesktopEngineProvider>(synthetic_capture_factory(deliver_audio));
}
} // namespace test_fixture
