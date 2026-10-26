#pragma once
#include <dsp_kernels/dsp.h>
#include <array>
#include <cmath>
#include <numbers>
#include <span>
#include <vector>
namespace rfft_test_oracle {
// Independent O(N^2) DFT, without using the FFT implementation under test.
inline void direct_bin(std::span<const float> input,std::span<double> result,std::size_t k) {
    const double angle=-2*std::numbers::pi*double(k)/double(input.size());
    const double wr=std::cos(angle),wi=std::sin(angle);
    double re=0,im=0,cosine=1,sine=0;
    for(const double sample:input) {
        re+=sample*cosine;
        im+=sample*sine;
        const auto next=cosine*wr-sine*wi;
        sine=sine*wr+cosine*wi;
        cosine=next;
    }
    result[2*k]=re;
    result[2*k+1]=im;
}
inline std::vector<double> direct_dft(std::span<const float> input) {
    std::vector<double> result(input.size()+2);
    for(std::size_t k=0;k<=input.size()/2;++k)direct_bin(input,result,k);
    return result;
}
inline std::vector<float> signal(std::size_t n) {
    std::vector<float> input(n);
    constexpr std::array<double, 7> frequencies{3, 11.25, 23, 41.5, 67, 89.75, 111};
    for (std::size_t i = 0; i < n; ++i) {
        double value = .17 + (i % 2 ? -.11 : .11);
        for (std::size_t tone = 0; tone < frequencies.size(); ++tone)
            value += (.25 / double(tone + 1)) * std::cos(2 * std::numbers::pi * frequencies[tone] * double(i) / double(n) + .13 * tone);
        input[i] = float(value);
    }
    return input;
}
struct Projection {
    double re,im,magnitude,amplitude,power,psd,bin_power,window_scaled_magnitude;
    Projection(double real,double imaginary,double sided,const dsp_kernels::WindowScaling& scaling,double rate)
        :re(real),im(imaginary) {
        magnitude=std::hypot(re,im);
        window_scaled_magnitude=magnitude/scaling.sum;
        const auto squared=re*re+im*im;
        amplitude=sided*magnitude/scaling.sum;
        power=sided*squared/(scaling.sum*scaling.sum);
        psd=sided*squared/(rate*scaling.sum_squares);
        bin_power=sided*squared/(double(scaling.samples)*scaling.sum_squares);
    }
    double value(dsp_kernels::SpectrumProjection kind)const {
        using enum dsp_kernels::SpectrumProjection;
        switch(kind) {
        case Real:return re;
        case Imaginary:return im;
        case Phase:return magnitude==0?0:std::atan2(im,re);
        case Magnitude:return magnitude;
        case WindowScaledMagnitude:return window_scaled_magnitude;
        case PeakAmplitude:return amplitude;
        case Power:return power;
        case Psd:return psd;
        case RmsAmplitude:return std::sqrt(power);
        case BinPower:return bin_power;
        }
        return 0;
    }
};
inline double expected_projection(dsp_kernels::SpectrumProjection kind,double re,double im,double sided,
    const dsp_kernels::WindowScaling& scaling,double rate) {
    return Projection(re,im,sided,scaling,rate).value(kind);
}
} // namespace rfft_test_oracle
