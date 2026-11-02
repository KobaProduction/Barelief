// SPDX-License-Identifier: BSD-2-Clause
// Adapted from libsamplerate src/src_sinc.c, calc_output_single and
// sinc_mono_vari_process, c96f5e3de9c4488f4e6c97f59f5245f22fda22f7.
// Copyright (c) 2012-2016, Erik de Castro Lopo <erikd@mega-nerd.com>
/*
Copyright (c) 2012-2016, Erik de Castro Lopo <erikd@mega-nerd.com>
All rights reserved.

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions are
met:

1. Redistributions of source code must retain the above copyright
   notice, this list of conditions and the following disclaimer.

2. Redistributions in binary form must reproduce the above copyright
   notice, this list of conditions and the following disclaimer in the
   documentation and/or other materials provided with the distribution.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS
IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED
TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A
PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED
TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING
NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*/
#include <dsp_kernels/sinc_resampling_window.h>
#include <algorithm>
#include <cmath>
#include <functional>
#include <limits>
#include <utility>
namespace dsp_kernels {
namespace {
constexpr std::int64_t fixed_one=4096;
bool table_valid(SincCoefficientTable table) noexcept {
    return table.increment && table.values.size()>2 && table.values.size()<=INT32_MAX &&
        std::int64_t(table.increment)<=std::int64_t(table.values.size()-2) &&
        std::all_of(table.values.begin(),table.values.end(),[](float x){return std::isfinite(x);});
}
bool disjoint(std::span<const float> input,std::span<const float> output) noexcept {
    const std::less<const float*> less;
    return !less(input.data(),output.data()+output.size()) || !less(output.data(),input.data()+input.size());
}
struct WindowCursor {
    std::int64_t length=0,half=0,current=0,end=0,real_end=-1,used=0;
    WindowCursor(SincCoefficientTable table,double ratio) noexcept {
        const double count=double(table.values.size())/double(table.increment);
        length=std::max<std::int64_t>(3*std::int64_t(std::nearbyint(count*256.+1.)),4096)+1;
        half=std::int64_t(std::nearbyint(count/std::min(ratio,1.)))+1;
    }
    void compact() noexcept {
        end=half+end-current;current=half;
    }
    void mark_end() noexcept {
        if(length-end<half+5)compact();
        real_end=end;end+=std::min(half+5,length-end);
    }
    void load(std::int64_t input_samples) noexcept {
        if(real_end>=0)return;
        std::int64_t count=0;
        if(!current){count=length-2*half;current=end=half;}
        else {
            if(end+half+1>=length)compact();
            count=std::max(length-current-half,std::int64_t{0});
        }
        count=std::min(input_samples-used,count);end+=count;used+=count;
        if(used==input_samples && end-current<2*half)mark_end();
    }
    bool available(std::int64_t input_samples,double fraction,double terminate) noexcept {
        if((end-current+length)%length<=half)load(input_samples);
        if((end-current+length)%length<=half)return false;
        return real_end<0 || double(current)+fraction+terminate<=double(real_end);
    }
    void advance(std::int64_t samples) noexcept {current=(current+samples)%length;}
};
double fractional_position(double value) noexcept {
    const double remainder=value-std::nearbyint(value);
    return remainder<0.?remainder+1.:remainder;
}
std::int64_t advance_position(double& fraction,double step) noexcept {
    fraction+=step;
    const double remainder=fractional_position(fraction);
    const auto whole=std::int64_t(std::nearbyint(fraction-remainder));fraction=remainder;
    return whole;
}
float zero_extended(std::span<const float> input,std::int64_t index) noexcept {
    return index<0 || std::uint64_t(index)>=input.size()?0.f:input[std::size_t(index)];
}
}
bool PreparedSincResamplingWindow::storage_bytes(const ResamplingWindowConfig& c,std::size_t& bytes) noexcept {
    bytes=0;
    if(c.mode!=ResamplingWindowMode::SincBestQuality || !valid_resampling_window(c) ||
       c.output_samples>SIZE_MAX/sizeof(Position))return false;
    bytes=c.output_samples*sizeof(Position);return true;
}
bool PreparedSincResamplingWindow::prepare(ResamplingWindowConfig c,SincCoefficientTable table) {
    PreparedSincResamplingWindow next;
    if(!next.configure(c,table))return false;
    *this=std::move(next);return true;
}
bool PreparedSincResamplingWindow::configure(ResamplingWindowConfig c,SincCoefficientTable table) {
    std::size_t bytes=0;
    if(!storage_bytes(c,bytes) || !table_valid(table))return false;
    config_=c;table_=table;maximum_=std::int64_t(table.values.size()-2)*fixed_one;
    positions_.resize(c.output_samples);
    return plan_positions();
}
bool PreparedSincResamplingWindow::generate_positions(double ratio,double scaled_increment,double step,double terminate) {
    double fraction=0.;
    std::int64_t center=0;
    WindowCursor cursor(table_,ratio);
    for(auto& position:positions_){
        if(!cursor.available(std::int64_t(config_.input_samples),fraction,terminate))break;
        position={center,std::int64_t(std::nearbyint(fraction*scaled_increment*double(fixed_one)))};
        ++generated_;
        const auto advance=advance_position(fraction,step);
        center+=advance;
        cursor.advance(advance);
    }
    return increment_>0 && generated_!=0;
}

bool PreparedSincResamplingWindow::plan_positions() {
    const double ratio=double(config_.output_samples)/double(config_.input_samples);
    const double scaled_increment=double(table_.increment)*std::min(ratio,1.);
    const double step=1./ratio,terminate=step+1e-20;
    increment_=std::int64_t(std::nearbyint(scaled_increment*double(fixed_one)));
    scale_=float(scaled_increment/double(table_.increment));
    reference_scale_=scaled_increment/double(table_.increment);
    return generate_positions(ratio,scaled_increment,step,terminate);
}
float PreparedSincResamplingWindow::interpolate(std::int64_t fixed) const noexcept {
    const auto index=std::size_t(fixed/fixed_one);
    const float fraction=float(fixed%fixed_one)/float(fixed_one);
    return table_.values[index]+fraction*(table_.values[index+1]-table_.values[index]);
}
float PreparedSincResamplingWindow::half_sum(std::span<const float> input,
    std::int64_t coefficient,std::int64_t sample,int direction) const noexcept {
    const auto count=(maximum_-coefficient)/increment_;
    coefficient+=count*increment_;sample-=std::int64_t(direction)*count;
    float sum=0;
    // Left includes the center coefficient; right excludes its duplicated zero.
    do {
        sum+=interpolate(coefficient)*zero_extended(input,sample);
        coefficient-=increment_;sample+=direction;
    } while(direction>0?coefficient>=0:coefficient>0);
    return sum;
}
float PreparedSincResamplingWindow::convolve(std::span<const float> input,Position p) const noexcept {
    const float left=half_sum(input,p.coefficient,p.center,1);
    const float right=half_sum(input,increment_-p.coefficient,p.center+1,-1);
    return scale_*(left+right);
}
double PreparedSincResamplingWindow::reference_interpolate(std::int64_t fixed) const noexcept {
    const auto index=std::size_t(fixed/fixed_one);
    const double fraction=double(fixed%fixed_one)/double(fixed_one);
    const float difference=table_.values[index+1]-table_.values[index];
    return double(table_.values[index])+fraction*double(difference);
}
double PreparedSincResamplingWindow::reference_half_sum(std::span<const float> input,
    std::int64_t coefficient,std::int64_t sample,int direction) const noexcept {
    const auto count=(maximum_-coefficient)/increment_;
    coefficient+=count*increment_;sample-=std::int64_t(direction)*count;
    double sum=0;
    do {
        sum+=reference_interpolate(coefficient)*double(zero_extended(input,sample));
        coefficient-=increment_;sample+=direction;
    }while(direction>0?coefficient>=0:coefficient>0);
    return sum;
}
float PreparedSincResamplingWindow::reference_convolve(std::span<const float> input,Position p) const noexcept {
    const double left=reference_half_sum(input,p.coefficient,p.center,1);
    const double right=reference_half_sum(input,increment_-p.coefficient,p.center+1,-1);
    const double value=reference_scale_*(left+right);
    if(!std::isfinite(value) || std::abs(value)>std::numeric_limits<float>::max())return std::numeric_limits<float>::quiet_NaN();
    return float(value);
}
bool PreparedSincResamplingWindow::process_channel(std::span<const float> values,std::span<float> output) const noexcept {
    if(!std::all_of(values.begin(),values.end(),[](float x){return std::isfinite(x);}))return false;
    for(std::size_t i=0;i<generated_;++i){
        const float value=config_.precision==NumericalPrecision::DesktopReference?
            reference_convolve(values,positions_[i]):convolve(values,positions_[i]);
        if(!std::isfinite(value))return false;
        output[i]=value;
    }
    return true;
}

bool PreparedSincResamplingWindow::process(std::span<const float> input,std::size_t input_stride,
    std::span<float> output,std::size_t output_stride,std::size_t& written) const noexcept {
    written=0;
    if(!generated_ || input_stride<config_.input_samples || output_stride<generated_ ||
        input_stride>input.size()/config_.channels || output_stride>output.size()/config_.channels ||
        !disjoint(input,output))return false;
    for(std::size_t channel=0;channel<config_.channels;++channel){
        const auto values=input.subspan(channel*input_stride,config_.input_samples);
        if(!process_channel(values,output.subspan(channel*output_stride,generated_)))return false;
    }
    written=generated_;
    return true;
}
}
