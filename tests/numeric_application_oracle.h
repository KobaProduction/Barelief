#pragma once
#include <signal_contracts/data_frame.h>
#include <algorithm>
#include <cmath>
#include <cstring>

namespace numeric_application_oracle {
inline bool centered_band(const signal_contracts::NumericView& values,std::size_t band) {
    double sum=0,scale=1;
    for(std::size_t sample=0;sample<values.actual[1];++sample) {
        float value=0;
        if(signal_contracts::load_element(values,{0,band,sample},value)!=signal_contracts::Status::ok || !std::isfinite(value))return false;
        sum+=value;
        scale=std::max(scale,std::abs(double(value)));
    }
    return std::abs(sum/values.actual[1])<scale*1e-6;
}
inline bool centered_history(const signal_contracts::NumericView& values) {
    if(values.actual[0]!=50 || values.actual[1]!=400)return false;
    for(std::size_t band=0;band<values.actual[0];++band)
        if(!centered_band(values,band))return false;
    return true;
}
inline bool temporal_matrix(const signal_contracts::NumericView& matrix,double expected_bin_hz,double& step) {
    using namespace signal_contracts;
    if(matrix.actual[0]!=50 || matrix.actual[1]!=256)return false;
    if(matrix.contract->shape.axes[0]!=Axis::Band || matrix.contract->shape.axes[1]!=Axis::Bin)return false;
    if(matrix.contract->coordinates[1].unit!=NumericUnit::Hertz)return false;
    if(matrix.coordinates.axes[1].size()!=256*sizeof(double))return false;
    std::memcpy(&step,matrix.coordinates.axes[1].data()+sizeof(double),sizeof(double));
    return std::abs(step-expected_bin_hz)<1e-6;
}
inline bool accumulate_square(const signal_contracts::NumericView& values,double& energy) {
    for(std::size_t index=0;index<values.actual[0];++index) {
        float value=0;
        if(signal_contracts::load_element(values,{0,index,0},value)!=signal_contracts::Status::ok || !std::isfinite(value))return false;
        energy+=double(value)*value;
    }
    return true;
}
inline bool positive_matrix_energy(const signal_contracts::NumericView& values) {
    double energy=0;
    for(std::size_t band=0;band<50;++band)for(std::size_t bin=0;bin<256;++bin) {
        float value=0;
        if(signal_contracts::load_element(values,{0,band,bin},value)!=signal_contracts::Status::ok || !std::isfinite(value))return false;
        energy+=double(value)*value;
    }
    return energy>0;
}
inline bool zero_matrix(const signal_contracts::NumericView& silence) {
    for(std::size_t band=0;band<50;++band)for(std::size_t bin=0;bin<256;++bin) {
        float value=1;
        if(signal_contracts::load_element(silence,{0,band,bin},value)!=signal_contracts::Status::ok||value!=0)return false;
    }
    return true;
}
}
