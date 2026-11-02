#include <dsp_kernels/correlation.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <limits>

namespace {
void check(bool value,const char* why){if(!value){std::fprintf(stderr,"FAIL %s\n",why);std::exit(1);}}
void near(float actual,double expected){check(std::abs(double(actual)-expected)<1e-6,"analytic correlation coefficient");}
void analytic_coefficients(){
    const std::array<float,4> alternating{1,-1,1,-1};std::array<float,4> output{};
    check(dsp_kernels::linear_autocorrelation(alternating,output),"alternating signal accepted");
    for(std::size_t i=0;i<4;++i)near(output[i],i%2?-1:1);
    const std::array<float,4> edges{1,0,0,1};
    check(dsp_kernels::linear_autocorrelation(edges,output),"edge impulses accepted");
    near(output[0],1);near(output[1],0);near(output[2],0);near(output[3],1);
    // Circular correlation would give nonzero lag 1 by wrapping the last pulse.
    const std::array<float,4> ramp{1,2,3,4};
    check(dsp_kernels::linear_autocorrelation(ramp,output),"asymmetric signal accepted");
    near(output[1],20/std::sqrt(29.*14.));near(output[2],11/std::sqrt(25.*5.));near(output[3],1);
}
void scaling_and_silence(){
    std::array<float,5> values{2,-1,0,3,1};std::array<float,5> original{},scaled{};
    check(dsp_kernels::linear_autocorrelation(values,original),"reference coefficients");
    // Keep products and overlap energies representable in the Float32 API.
    for(const float gain:{-1e-15f,1e15f}){
        auto input=values;for(auto& value:input)value*=gain;
        check(dsp_kernels::linear_autocorrelation(input,scaled),"finite extreme scale accepted");
        for(std::size_t lag=0;lag<5;++lag)near(scaled[lag],original[lag]);
    }
    values.fill(0);check(dsp_kernels::linear_autocorrelation(values,scaled),"silence accepted");
    for(const auto value:scaled)near(value,0);
    values.fill(1e-30f);
    check(dsp_kernels::linear_autocorrelation(values,scaled),"underflowed Float32 energies are zero");
    for(const auto value:scaled)near(value,0);
    values.fill(std::numeric_limits<float>::max());
    check(!dsp_kernels::linear_autocorrelation(values,scaled),"Float32 product overflow is reported");
}
void atomic_failures(){
    std::array<float,4> data{1,2,3,4},output{7,7,7,7};
    const auto preserved=output;
    data[3]=std::numeric_limits<float>::infinity();
    check(!dsp_kernels::linear_autocorrelation(data,output) && output==preserved,"nonfinite tail rejected before output mutation");
    data[3]=4;
    check(!dsp_kernels::linear_autocorrelation(std::span(data).first(2),output) && output==preserved,"excess lag rejected atomically");
    const auto before=data;
    check(!dsp_kernels::linear_autocorrelation(data,std::span(data).subspan(1)) && data==before,"overlapping storage rejected atomically");
    check(dsp_kernels::linear_autocorrelation({},{}),"empty operation allowed");
    check(dsp_kernels::linear_autocorrelation(std::span(data).first(1),std::span(output).first(1)),"singleton lag zero allowed");
    near(output[0],1);
}
}
int main(){analytic_coefficients();scaling_and_silence();atomic_failures();std::puts("linear signed ACF analytic/non-circular/scale/zero/extreme/atomic PASS");}
