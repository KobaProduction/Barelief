#pragma once
#include <algorithm>
#include <cmath>
#include <limits>

// A renderer-only numeric projection. Source contracts and payloads stay intact.
struct ValueDisplayScale {
    enum class Mode {Source,Linear,Decibels};
    Mode mode=Mode::Source;
    bool source_decibels=false;
    int factor=20,override_factor=0;
    int effective_factor()const{return override_factor?override_factor:factor;}
    double reference=1,floor=-120;
    bool operator==(const ValueDisplayScale&) const=default;
    bool decibels()const{return mode==Mode::Decibels || (mode==Mode::Source && source_decibels);}
    double map(double value)const {
        if(!std::isfinite(value))return decibels()?floor:0;
        if(mode==Mode::Source)return value;
        if(decibels()==source_decibels)return decibels()?std::max(floor,value):value;
        if(decibels())return std::max(floor,effective_factor()*std::log10(std::max(std::abs(value)/reference,1e-300)));
        return std::min(double(std::numeric_limits<float>::max()),reference*std::pow(10.,std::clamp(value/effective_factor(),-300.,300.)));
    }
};
