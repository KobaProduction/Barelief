#pragma once
#include <algorithm>
#include <cstdint>

namespace visualization::rendering {
// GUI-independent submission policy. Times are monotonic nanoseconds supplied
// by the adapter. The renderer owns snapshots; this policy never stores samples.
class FrameSchedule final {
public:
    using Time=std::uint64_t;
    using Token=std::uint64_t;
    void request() noexcept {dirty_=true;}
    void setVisible(bool visible) noexcept {
        if(visible_==visible)return;
        visible_=visible;dirty_|=pending_!=0;pending_=0;deadline_=0;
    }
    void setRate(int fps) noexcept {
        const auto rate=std::clamp(fps,1,240);
        const auto period=(1000000000ULL+rate-1)/rate;
        if(period_!=period){period_=period;deadline_=0;}
    }
    bool runnable() const noexcept {return visible_ && dirty_ && !pending_;}
    Time delay(Time now) const noexcept {return deadline_>now?deadline_-now:0;}
    Token submit(Time now) noexcept {
        if(!runnable() || delay(now))return 0;
        dirty_=false;pending_=++sequence_;
        // Keep phase through ordinary jitter, but never replay missed frames.
        if(!deadline_ || now-deadline_>=period_)deadline_=now+period_;
        else deadline_+=period_;
        return pending_;
    }
    bool complete(Token token) noexcept {
        if(!token || token!=pending_)return false;
        pending_=0;return true;
    }
private:
    Time period_=16666667,deadline_=0;
    Token sequence_=0,pending_=0;
    bool dirty_=false,visible_=true;
};
}
