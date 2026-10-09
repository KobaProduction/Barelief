#pragma once
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <limits>
#include <utility>

// Presentation-only amplitude policy. Data and DSP clocks are never modified.
// A complete interval contributes extrema before its next bounds are committed.
class AmplitudeRange {
public:
    static std::int64_t now() {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
    }
    static double interval(double seconds) {
        return std::isfinite(seconds)?std::clamp(seconds,1.,60.):10.;
    }
    void reset() { *this=AmplitudeRange{}; }
    std::pair<double,double> range() const {return bounds_;}
    bool observe(double low,double high,std::int64_t milliseconds,double seconds,
                 double floorLow=0,double floorHigh=.001) {
        if(!std::isfinite(low) || !std::isfinite(high) || low>high)return false;
        // Cold zero-filled History has geometry before any useful amplitude.
        // Keep the neutral range until the first nonzero signal can fit it.
        if(!initialized_ && low==0 && high==0)return false;
        pendingLow_=std::min(pendingLow_,std::min(low,floorLow));
        pendingHigh_=std::max(pendingHigh_,std::max(high,floorHigh));
        if(initialized_ && milliseconds>=committed_ &&
           milliseconds-committed_<interval(seconds)*1000.)return false;
        const auto next=std::pair{-nice(-std::min(0.,pendingLow_)*1.2),
                                  nice(std::max(0.,pendingHigh_)*1.2)};
        const bool changed=bounds_!=next;
        bounds_=next;initialized_=true;committed_=milliseconds;
        pendingLow_=std::numeric_limits<double>::infinity();
        pendingHigh_=-std::numeric_limits<double>::infinity();
        return changed;
    }
private:
    static double nice(double magnitude) {
        if(magnitude<=0)return 0;
        const double limit=std::numeric_limits<float>::max();
        if(!std::isfinite(magnitude) || magnitude>=limit)return limit;
        const double unit=std::pow(10.,std::floor(std::log10(magnitude)));
        for(double step:{1.,1.25,1.6,2.,2.5,3.2,4.,5.,6.3,8.,10.})
            if(step*unit>=magnitude)return std::min(limit,step*unit);
        return std::min(limit,10*unit);
    }
    std::pair<double,double> bounds_{0,1};
    double pendingLow_=std::numeric_limits<double>::infinity();
    double pendingHigh_=-std::numeric_limits<double>::infinity();
    std::int64_t committed_=0;
    bool initialized_=false;
};
