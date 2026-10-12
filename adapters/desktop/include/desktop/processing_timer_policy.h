#pragma once

namespace desktop {
// Owns the desktop process timer policy while processing hosts exist. This
// resource is separate from DSP time and never changes sample timestamps.
class ProcessingTimerPolicy {
public:
    ProcessingTimerPolicy();
    ~ProcessingTimerPolicy();
    ProcessingTimerPolicy(const ProcessingTimerPolicy&)=delete;
    ProcessingTimerPolicy& operator=(const ProcessingTimerPolicy&)=delete;
};
}
